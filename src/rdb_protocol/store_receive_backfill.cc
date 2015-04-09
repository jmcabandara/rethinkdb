#include "rdb_protocol/store.hpp"

#include "btree/backfill.hpp"
#include "btree/reql_specific.hpp"
#include "rdb_protocol/btree.hpp"

/* `MAX_CONCURRENT_BACKFILL_ITEMS` is the maximum number of coroutines we'll spawn in
parallel to apply backfill items to the B-tree. */
#define MAX_CONCURRENT_BACKFILL_ITEMS 16

/* `MAX_CHANGES_PER_TXN` is the maximum number of keys we'll modify or delete in a single
transaction when applying a multi-key backfill item. Increasing this number will make the
backfill faster, but it will also cause the backfill-related transactions to hold the
superblock for a longer time. */
#define MAX_CHANGES_PER_TXN 16

class receive_backfill_info_t {
public:
    receive_backfill_info_t(cache_conn_t *c, btree_slice_t *s) :
        cache_conn(c), slice(s), semaphore(MAX_CONCURRENT_BACKFILL_ITEMS) { }
    cache_conn_t *cache_conn;
    btree_slice_t *slice;
    new_semaphore_t semaphore;
    fifo_enforcer_source_t fifo_source;
    fifo_enforcer_sink_t btree_fifo_sink, commit_fifo_sink;
    auto_drainer_t drainer;
};

class receive_backfill_tokens_t {
public:
    receive_backfill_tokens_t(receive_backfill_info_t *i, signal_t *interruptor) :
        info(i), sem_acq(&info->semaphore, 1),
        write_token(info->fifo_source.enter_write()), keepalive(info->drainer.lock()) {
        wait_interruptible(sem_acq.acquisition_signal(), interruptor);
    }
    receive_backfill_info_t *info;
    new_semaphore_acq_t sem_acq;
    fifo_enforcer_write_token_t write_token;
    auto_drainer_t::lock_t keepalive;
    std::function<void(
        const key_range_t::right_bound_t &progress,
        real_superblock_t *superblock
        )> update_metainfo_cb;
    std::function<void(
        const key_range_t::right_bound_t &progress,
        scoped_ptr_t<txn_t> &&txn,
        buf_lock_t &&sindex_block,
        std::vector<rdb_modification_report_t> &&mod_reports
        )> commit_cb;
};

void apply_edge(
        const receive_backfill_tokens_t &tokens,
        const key_range_t::right_bound_t &edge) {
    debugf_print("apply_edge", edge);
    try {
        /* Acquire the superblock */
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        {
            fifo_enforcer_sink_t::exit_write_t exiter(
                &tokens.info->btree_fifo_sink, tokens.write_token);
            wait_interruptible(&exiter, tokens.keepalive.get_drain_signal());
            get_btree_superblock_and_txn_for_writing(tokens.info->cache_conn, nullptr,
                write_access_t::write, 1, repli_timestamp_t::distant_past,
                write_durability_t::SOFT, &superblock, &txn);
        }

        /* Update the metainfo and release the superblock. */
        tokens.update_metainfo_cb(edge, superblock.get());
        superblock.reset();

        /* Notify that we're done */
        fifo_enforcer_sink_t::exit_write_t exiter(
            &tokens.info->commit_fifo_sink, tokens.write_token);
        tokens.commit_cb(edge, std::move(txn), buf_lock_t(),
            std::vector<rdb_modification_report_t>());

    } catch (const interrupted_exc_t &exc) {
        /* The call to `receive_backfill()` was interrupted. Ignore. */
    }
}

void apply_item_pair(
        btree_slice_t *slice,
        real_superblock_t *superblock,
        backfill_item_t::pair_t &&pair,
        std::vector<rdb_modification_report_t> *mod_reports_out,
        promise_t<superblock_t *> *pass_back_superblock) {
    rdb_live_deletion_context_t deletion_context;
    mod_reports_out->resize(mod_reports_out->size() + 1);
    mod_reports_out->back().primary_key = pair.key;
    if (static_cast<bool>(pair.value)) {
        vector_read_stream_t read_stream(std::move(*pair.value));
        ql::datum_t datum;
        archive_result_t res = datum_deserialize(&read_stream, &datum);
        guarantee(res == archive_result_t::SUCCESS);

        point_write_response_t dummy_response;
        rdb_set(pair.key, datum, true, slice, pair.recency, superblock,
            &deletion_context, &dummy_response, &mod_reports_out->back().info, nullptr,
            pass_back_superblock);
    } else {
        point_delete_response_t dummy_response;
        rdb_delete(pair.key, slice, pair.recency, superblock,
            &deletion_context, delete_or_erase_t::DELETE, &dummy_response,
            &mod_reports_out->back().info, nullptr, pass_back_superblock);
    }
}

void apply_single_key_item(
        const receive_backfill_tokens_t &tokens,
        /* `item` is conceptually passed by move, but `std::bind()` isn't smart enough to
        handle that. */
        backfill_item_t &item) {
    debugf("apply_single_key_item\n");
    try {
        /* Acquire the superblock */
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        {
            fifo_enforcer_sink_t::exit_write_t exiter(
                &tokens.info->btree_fifo_sink, tokens.write_token);
            wait_interruptible(&exiter, tokens.keepalive.get_drain_signal());
            get_btree_superblock_and_txn_for_writing(tokens.info->cache_conn, nullptr,
                write_access_t::write, 1, repli_timestamp_t::distant_past,
                write_durability_t::SOFT, &superblock, &txn);
        }

        /* Acquire the sindex block and update the metainfo now, because we'll release
        the superblock soon */
        buf_lock_t sindex_block(superblock->expose_buf(),
            superblock->get_sindex_block_id(), access_t::write);
        tokens.update_metainfo_cb(item.range.right, superblock.get());

        /* Actually apply the change, releasing the superblock in the process. */
        std::vector<rdb_modification_report_t> mod_reports;
        apply_item_pair(tokens.info->slice, superblock.get(),
            std::move(item.pairs[0]), &mod_reports, nullptr);

        /* Notify that we're done and update the sindexes */
        fifo_enforcer_sink_t::exit_write_t exiter(
            &tokens.info->commit_fifo_sink, tokens.write_token);
        tokens.commit_cb(item.range.right, std::move(txn), std::move(sindex_block),
            std::move(mod_reports));

    } catch (const interrupted_exc_t &exc) {
        /* The call to `receive_backfill()` was interrupted. Ignore. */
    }
}

void apply_multi_key_item(
        const receive_backfill_tokens_t &tokens,
        /* `item` is conceptually passed by move, but `std::bind()` isn't smart enough to
        handle that. */
        backfill_item_t &item) {
    debugf("apply_multi_key_item\n");
    try {
        /* Acquire and hold both `fifo_enforcer_sink_t`s until we're completely finished;
        since we're going to be making multiple B-tree queries, we can't pipeline with
        other backfill items */
        fifo_enforcer_sink_t::exit_write_t exiter1(
            &tokens.info->btree_fifo_sink, tokens.write_token);
        wait_interruptible(&exiter1, tokens.keepalive.get_drain_signal());
        fifo_enforcer_sink_t::exit_write_t exiter2(
            &tokens.info->commit_fifo_sink, tokens.write_token);
        wait_interruptible(&exiter2, tokens.keepalive.get_drain_signal());

        /* It's possible that there are a lot of keys to be deleted, so we might do the
        backfill item in several chunks. */
        bool is_first = true;
        size_t next_pair = 0;
        key_range_t::right_bound_t threshold(item.range.left);
        while (threshold != item.range.right) {
            std::vector<rdb_modification_report_t> mod_reports;

            /* Acquire the superblock. */
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(tokens.info->cache_conn, nullptr,
                write_access_t::write, 1, repli_timestamp_t::distant_past,
                write_durability_t::SOFT, &superblock, &txn);

            /* If we haven't already done so, then update the min deletion timstamps. */
            if (is_first) {
                rdb_value_sizer_t sizer(superblock->cache()->max_block_size());
                btree_receive_backfill_item_update_deletion_timestamps(
                    superblock.get(), release_superblock_t::KEEP, &sizer, item,
                    tokens.keepalive.get_drain_signal());
                is_first = false;
            }

            /* Establish an upper limit on how much of the range we're willing to delete
            in this cycle. We choose the upper limit such that it contains no more than
            `MAX_CHANGES_PER_TXN / 2` of the pairs in the backfill item. */
            key_range_t range_to_delete;
            range_to_delete.left = threshold.key();
            if (next_pair + MAX_CHANGES_PER_TXN / 2 + 1 < item.pairs.size()) {
                range_to_delete.right = key_range_t::right_bound_t(
                    item.pairs[next_pair + MAX_CHANGES_PER_TXN / 2 + 1].key);
            } else {
                range_to_delete.right = item.range.right;
            }

            /* Delete a chunk of the range */
            always_true_key_tester_t key_tester;
            key_range_t range_deleted;
            rdb_live_deletion_context_t deletion_context;
            continue_bool_t res = rdb_erase_small_range(tokens.info->slice, &key_tester,
                range_to_delete, superblock.get(), &deletion_context,
                tokens.keepalive.get_drain_signal(), MAX_CHANGES_PER_TXN / 2,
                &mod_reports, &range_deleted);

            /* Apply any pairs from the item that fall within the deleted region */
            while (next_pair < item.pairs.size() &&
                    range_deleted.contains_key(item.pairs[next_pair].key)) {
                promise_t<superblock_t *> pass_back_superblock;
                apply_item_pair(tokens.info->slice, superblock.get(),
                    std::move(item.pairs[next_pair]), &mod_reports,
                    &pass_back_superblock);
                guarantee(superblock.get() == pass_back_superblock.assert_get_value());
                ++next_pair;
            }

            /* Update `threshold` to reflect the changes we've made */
            threshold = range_deleted.right;
            guarantee(threshold == item.range.right || res == continue_bool_t::CONTINUE);

            /* Acquire the sindex block and update the metainfo */
            buf_lock_t sindex_block(superblock->expose_buf(),
                superblock->get_sindex_block_id(), access_t::write);
            tokens.update_metainfo_cb(threshold, superblock.get());
            superblock->release();

            /* Notify the callback of our progress and update the sindexes */
            tokens.commit_cb(threshold, std::move(txn), std::move(sindex_block),
                std::move(mod_reports));
        }

    } catch (const interrupted_exc_t &exc) {
        /* The call to `receive_backfill()` was interrupted. Ignore. */
    }
}

continue_bool_t store_t::receive_backfill(
        const region_t &region,
        backfill_item_producer_t *item_producer,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    guarantee(region.beg == get_region().beg && region.end == get_region().end);
    receive_backfill_info_t info(general_cache_conn.get(), btree.get());

    /* Repeatedly request items from `item_producer` and spawn coroutines to handle them,
    but limit the number of simultaneously active coroutines. */
    key_range_t::right_bound_t
        spawn_threshold(region.inner.left),
        metainfo_threshold(region.inner.left),
        commit_threshold(region.inner.left);
    continue_bool_t result = continue_bool_t::CONTINUE;
    while (spawn_threshold != region.inner.right) {
        bool is_item;
        backfill_item_t item;
        key_range_t::right_bound_t edge;
        if (continue_bool_t::ABORT == item_producer->next_item(&is_item, &item, &edge)) {
            /* By breaking out of the loop instead of returning immediately, we ensure
            that we commit every item that we got from the item producer, as we are
            required to. */
            result = continue_bool_t::ABORT;
            break;
        }

        receive_backfill_tokens_t tokens(&info, interruptor);

        if (is_item) {
            spawn_threshold = item.get_range().right;
        } else {
            spawn_threshold = edge;
        }

        /* The `apply_*()` functions will call back to `update_metainfo_cb` when they
        want to apply the metainfo to the superblock. They may make multiple calls, but
        the last call will have `progress` equal to `item.get_range().right`. */
        tokens.update_metainfo_cb = [this, &region, &metainfo_threshold, &item_producer](
                const key_range_t::right_bound_t &progress,
                real_superblock_t *superblock) {
            /* Compute the section of metainfo we're applying */
            guarantee(progress >= metainfo_threshold);
            if (progress == metainfo_threshold) {
                /* This is a no-op */
                return;
            }
            region_t mask = region;
            mask.inner.left = metainfo_threshold.key();
            mask.inner.right = progress;
            metainfo_threshold = progress;

            /* Actually apply the metainfo */
            region_map_t<binary_blob_t> old_metainfo;
            get_metainfo_internal(superblock->get(), &old_metainfo);
            update_metainfo(
                old_metainfo, item_producer->get_metainfo()->mask(mask), superblock);
        };

        /* The `apply_*()` functions will call back to `commit_cb` when they're done
        applying the changes for a given sub-region. They may make multiple calls, but
        the last call will have `progress` equal to `item.get_range().right`. */
        tokens.commit_cb = [this, item_producer, &commit_threshold](
                const key_range_t::right_bound_t &progress,
                scoped_ptr_t<txn_t> &&txn,
                buf_lock_t &&sindex_block,
                std::vector<rdb_modification_report_t> &&mod_reports) {
            /* Apply the modifications */
            if (!mod_reports.empty()) {
                update_sindexes(txn.get(), &sindex_block, mod_reports, true);
            } else {
                sindex_block.reset_buf_lock();
            }

            /* End the transaction and notify that we've made progress */
            txn.reset();
            guarantee(progress >= commit_threshold);
            if (progress == commit_threshold) {
                /* This is a no-op */
                guarantee(mod_reports.empty());
                return;
            }
            commit_threshold = progress;
            item_producer->on_commit(progress);
        };

        if (!is_item) {
            coro_t::spawn_sometime(std::bind(
                &apply_edge, std::move(tokens), edge));
        } else if (item.is_single_key()) {
            coro_t::spawn_sometime(std::bind(
                &apply_single_key_item, std::move(tokens), std::move(item)));
        } else {
            coro_t::spawn_sometime(std::bind(
                &apply_multi_key_item, std::move(tokens), std::move(item)));
        }
    }

    /* Wait for any running coroutines to finish. We construct an `exit_write_t` instead
    of just destroying `info.drainer` because we don't want to interrupt the coroutines
    unless `interruptor` is pulsed. */
    fifo_enforcer_sink_t::exit_write_t exiter(
        &info.commit_fifo_sink, info.fifo_source.enter_write());
    wait_interruptible(&exiter, interruptor);

    if (result == continue_bool_t::CONTINUE) {
        guarantee(spawn_threshold == region.inner.right);
        guarantee(metainfo_threshold == region.inner.right);
        guarantee(commit_threshold == region.inner.right);
    }

    return result;
}

