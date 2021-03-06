/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vy_point_lookup.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <small/region.h>
#include <small/rlist.h>

#include "fiber.h"

#include "vy_index.h"
#include "vy_stmt.h"
#include "vy_tx.h"
#include "vy_mem.h"
#include "vy_run.h"
#include "vy_cache.h"
#include "vy_upsert.h"

/**
 * ID of an iterator source type. Can be used in bitmaps.
 */
enum iterator_src_type {
	ITER_SRC_TXW = 1,
	ITER_SRC_CACHE = 2,
	ITER_SRC_MEM = 4,
	ITER_SRC_RUN = 8,
};

/**
 * History of a key in vinyl is a continuous sequence of statements of the
 * same key in order of decreasing lsn. The history can be represented as a
 * list, the structure below describes one node of the list.
 */
struct vy_stmt_history_node {
	/* Type of source that the history statement came from */
	enum iterator_src_type src_type;
	/* The history statement. Referenced for runs. */
	struct tuple *stmt;
	/* Link in the history list */
	struct rlist link;
};

/**
 * Allocate (region) new history node.
 * @return new node or NULL on memory error (diag is set).
 */
static struct vy_stmt_history_node *
vy_stmt_history_node_new(void)
{
	struct region *region = &fiber()->gc;
	struct vy_stmt_history_node *node = region_alloc(region, sizeof(*node));
	if (node == NULL)
		diag_set(OutOfMemory, sizeof(*node), "region",
			 "struct vy_stmt_history_node");
	return node;
}

/**
 * Unref statement if necessary, remove node from history if it's there.
 */
static void
vy_stmt_history_cleanup(struct rlist *history, size_t region_svp)
{
	struct vy_stmt_history_node *node;
	rlist_foreach_entry(node, history, link)
		if (node->src_type == ITER_SRC_RUN)
			tuple_unref(node->stmt);

	region_truncate(&fiber()->gc, region_svp);
}

/**
 * Return true if the history of a key contains terminal node in the end,
 * i.e. REPLACE of DELETE statement.
 */
static bool
vy_stmt_history_is_terminal(struct rlist *history)
{
	if (rlist_empty(history))
		return false;
	struct vy_stmt_history_node *node =
		rlist_last_entry(history, struct vy_stmt_history_node, link);
	assert(vy_stmt_type(node->stmt) == IPROTO_REPLACE ||
	       vy_stmt_type(node->stmt) == IPROTO_DELETE ||
	       vy_stmt_type(node->stmt) == IPROTO_INSERT ||
	       vy_stmt_type(node->stmt) == IPROTO_UPSERT);
       return vy_stmt_type(node->stmt) != IPROTO_UPSERT;
}

/**
 * Scan TX write set for given key.
 * Add one or no statement to the history list.
 */
static int
vy_point_lookup_scan_txw(struct vy_index *index, struct vy_tx *tx,
			 struct tuple *key, struct rlist *history)
{
	if (tx == NULL)
		return 0;
	index->stat.txw.iterator.lookup++;
	struct txv *txv =
		write_set_search_key(&tx->write_set, index, key);
	assert(txv == NULL || txv->index == index);
	if (txv == NULL)
		return 0;
	vy_stmt_counter_acct_tuple(&index->stat.txw.iterator.get,
				   txv->stmt);
	struct vy_stmt_history_node *node = vy_stmt_history_node_new();
	if (node == NULL)
		return -1;
	node->src_type = ITER_SRC_TXW;
	node->stmt = txv->stmt;
	rlist_add_tail(history, &node->link);
	return 0;
}

/**
 * Scan index cache for given key.
 * Add one or no statement to the history list.
 */
static int
vy_point_lookup_scan_cache(struct vy_index *index,
			   const struct vy_read_view **rv,
			   struct tuple *key, struct rlist *history)
{
	index->cache.stat.lookup++;
	struct tuple *stmt = vy_cache_get(&index->cache, key);

	if (stmt == NULL || vy_stmt_lsn(stmt) > (*rv)->vlsn)
		return 0;

	vy_stmt_counter_acct_tuple(&index->cache.stat.get, stmt);
	struct vy_stmt_history_node *node = vy_stmt_history_node_new();
	if (node == NULL)
		return -1;

	node->src_type = ITER_SRC_CACHE;
	node->stmt = stmt;
	rlist_add_tail(history, &node->link);
	return 0;
}

/**
 * Scan one particular mem.
 * Add found statements to the history list up to terminal statement.
 */
static int
vy_point_lookup_scan_mem(struct vy_index *index, struct vy_mem *mem,
			 const struct vy_read_view **rv,
			 struct tuple *key, struct rlist *history)
{
	struct tree_mem_key tree_key;
	tree_key.stmt = key;
	tree_key.lsn = (*rv)->vlsn;
	bool exact;
	struct vy_mem_tree_iterator mem_itr =
		vy_mem_tree_lower_bound(&mem->tree, &tree_key, &exact);
	index->stat.memory.iterator.lookup++;
	const struct tuple *stmt = NULL;
	if (!vy_mem_tree_iterator_is_invalid(&mem_itr)) {
		stmt = *vy_mem_tree_iterator_get_elem(&mem->tree, &mem_itr);
		if (vy_stmt_compare(stmt, key, mem->cmp_def) != 0)
			stmt = NULL;
	}

	if (stmt == NULL)
		return 0;

	while (true) {
		struct vy_stmt_history_node *node = vy_stmt_history_node_new();
		if (node == NULL)
			return -1;

		vy_stmt_counter_acct_tuple(&index->stat.memory.iterator.get,
					   stmt);

		node->src_type = ITER_SRC_MEM;
		node->stmt = (struct tuple *)stmt;
		rlist_add_tail(history, &node->link);
		if (vy_stmt_history_is_terminal(history))
			break;

		if (!vy_mem_tree_iterator_next(&mem->tree, &mem_itr))
			break;

		const struct tuple *prev_stmt = stmt;
		stmt = *vy_mem_tree_iterator_get_elem(&mem->tree, &mem_itr);
		if (vy_stmt_lsn(stmt) >= vy_stmt_lsn(prev_stmt))
			break;
		if (vy_stmt_compare(stmt, key, mem->cmp_def) != 0)
			break;
	}
	return 0;

}

/**
 * Scan all mems that belongs to the index.
 * Add found statements to the history list up to terminal statement.
 */
static int
vy_point_lookup_scan_mems(struct vy_index *index,
			  const struct vy_read_view **rv,
			  struct tuple *key, struct rlist *history)
{
	assert(index->mem != NULL);
	int rc = vy_point_lookup_scan_mem(index, index->mem, rv, key, history);
	struct vy_mem *mem;
	rlist_foreach_entry(mem, &index->sealed, in_sealed) {
		if (rc != 0 || vy_stmt_history_is_terminal(history))
			return rc;

		rc = vy_point_lookup_scan_mem(index, mem, rv, key, history);
	}
	return 0;
}

/**
 * Scan one particular slice.
 * Add found statements to the history list up to terminal statement.
 * Set *terminal_found to true if the terminal statement (DELETE or REPLACE)
 * was found.
 */
static int
vy_point_lookup_scan_slice(struct vy_index *index, struct vy_slice *slice,
			   const struct vy_read_view **rv, struct tuple *key,
			   struct rlist *history, bool *terminal_found)
{
	int rc = 0;
	/*
	 * The format of the statement must be exactly the space
	 * format with the same identifier to fully match the
	 * format in vy_mem.
	 */
	struct vy_run_iterator run_itr;
	vy_run_iterator_open(&run_itr, &index->stat.disk.iterator, slice,
			     ITER_EQ, key, rv, index->cmp_def, index->key_def,
			     index->disk_format, index->upsert_format,
			     index->id == 0);
	struct tuple *stmt;
	rc = vy_run_iterator_next_key(&run_itr, &stmt);
	while (rc == 0 && stmt != NULL) {
		struct vy_stmt_history_node *node = vy_stmt_history_node_new();
		if (node == NULL) {
			rc = -1;
			break;
		}
		node->src_type = ITER_SRC_RUN;
		node->stmt = stmt;
		tuple_ref(stmt);
		rlist_add_tail(history, &node->link);
		if (vy_stmt_type(stmt) != IPROTO_UPSERT) {
			*terminal_found = true;
			break;
		}
		rc = vy_run_iterator_next_lsn(&run_itr, &stmt);
	}
	vy_run_iterator_close(&run_itr);
	return rc;
}

/**
 * Find a range and scan all slices that belongs to the range.
 * Add found statements to the history list up to terminal statement.
 * All slices are pinned before first slice scan, so it's guaranteed
 * that complete history from runs will be extracted.
 */
static int
vy_point_lookup_scan_slices(struct vy_index *index,
			    const struct vy_read_view **rv,
			    struct tuple *key, struct rlist *history)
{
	struct vy_range *range = vy_range_tree_find_by_key(index->tree,
							   ITER_EQ, key);
	assert(range != NULL);
	int slice_count = range->slice_count;
	struct vy_slice **slices = (struct vy_slice **)
		region_alloc(&fiber()->gc, slice_count * sizeof(*slices));
	if (slices == NULL) {
		diag_set(OutOfMemory, slice_count * sizeof(*slices),
			 "region", "slices array");
		return -1;
	}
	int i = 0;
	struct vy_slice *slice;
	rlist_foreach_entry(slice, &range->slices, in_range) {
		vy_slice_pin(slice);
		slices[i++] = slice;
	}
	assert(i == slice_count);
	int rc = 0;
	bool terminal_found = false;
	for (i = 0; i < slice_count; i++) {
		if (rc == 0 && !terminal_found)
			rc = vy_point_lookup_scan_slice(index, slices[i],
					rv, key, history, &terminal_found);
		vy_slice_unpin(slices[i]);
	}
	return rc;
}

/**
 * Get a resultant statement from collected history. Add to cache if possible.
 */
static int
vy_point_lookup_apply_history(struct vy_index *index,
			      const struct vy_read_view **rv,
			      struct tuple *key, struct rlist *history,
			      struct tuple **ret)
{
	*ret = NULL;
	if (rlist_empty(history))
		return 0;

	struct tuple *curr_stmt = NULL;
	struct vy_stmt_history_node *node =
		rlist_last_entry(history, struct vy_stmt_history_node, link);
	if (vy_stmt_history_is_terminal(history)) {
		if (vy_stmt_type(node->stmt) == IPROTO_DELETE) {
			/* Ignore terminal delete */
		} else if (node->src_type == ITER_SRC_MEM) {
			curr_stmt = vy_stmt_dup(node->stmt,
						tuple_format(node->stmt));
		} else {
			curr_stmt = node->stmt;
			tuple_ref(curr_stmt);
		}
		node = rlist_prev_entry_safe(node, history, link);
	}
	while (node != NULL) {
		assert(vy_stmt_type(node->stmt) == IPROTO_UPSERT);
		/* We could not read the data that is invisible now */
		assert(node->src_type == ITER_SRC_TXW ||
		       vy_stmt_lsn(node->stmt) <= (*rv)->vlsn);

		struct tuple *stmt = vy_apply_upsert(node->stmt, curr_stmt,
					index->cmp_def, index->mem_format,
					index->upsert_format, true);
		index->stat.upsert.applied++;
		if (stmt == NULL)
			return -1;
		if (curr_stmt != NULL)
			tuple_unref(curr_stmt);
		curr_stmt = stmt;
		node = rlist_prev_entry_safe(node, history, link);
	}
	if (curr_stmt != NULL) {
		vy_stmt_counter_acct_tuple(&index->stat.get, curr_stmt);
		*ret = curr_stmt;
	}
	/**
	 * Add a statement to the cache
	 */
	if ((*rv)->vlsn == INT64_MAX) /* Do not store non-latest data */
		vy_cache_add(&index->cache, curr_stmt, NULL, key, ITER_EQ);
	return 0;
}

int
vy_point_lookup(struct vy_index *index, struct vy_tx *tx,
		const struct vy_read_view **rv,
		struct tuple *key, struct tuple **ret)
{
	assert(tuple_field_count(key) >= index->cmp_def->part_count);

	*ret = NULL;
	size_t region_svp = region_used(&fiber()->gc);
	double start_time = ev_monotonic_now(loop());
	int rc = 0;

	index->stat.lookup++;
	/* History list */
	struct rlist history;
	/*
	 * Notify the TX manager that we are about to read the key
	 * so that if a new statement with the same key arrives
	 * while we are reading a run file, we will be sent to a
	 * read view and hence will not try to add a stale value
	 * to the cache.
	 */
	if (tx != NULL) {
		rc = vy_tx_track_point(tx, index, key);
		if (rc != 0)
			goto done;
	}
restart:
	rlist_create(&history);

	rc = vy_point_lookup_scan_txw(index, tx, key, &history);
	if (rc != 0 || vy_stmt_history_is_terminal(&history))
		goto done;

	rc = vy_point_lookup_scan_cache(index, rv, key, &history);
	if (rc != 0 || vy_stmt_history_is_terminal(&history))
		goto done;

	rc = vy_point_lookup_scan_mems(index, rv, key, &history);
	if (rc != 0 || vy_stmt_history_is_terminal(&history))
		goto done;

	/* Save version before yield */
	uint32_t mem_list_version = index->mem_list_version;

	rc = vy_point_lookup_scan_slices(index, rv, key, &history);
	if (rc != 0)
		goto done;

	ERROR_INJECT(ERRINJ_VY_POINT_ITER_WAIT, {
		while (mem_list_version == index->mem_list_version)
			fiber_sleep(0.01);
		/* Turn of the injection to avoid infinite loop */
		errinj(ERRINJ_VY_POINT_ITER_WAIT, ERRINJ_BOOL)->bparam = false;
	});

	if (mem_list_version != index->mem_list_version) {
		/*
		 * Mem list was changed during yield. This could be rotation
		 * or a dump. In case of dump the memory referenced by
		 * statement history is gone and we need to reread new history.
		 * This in unnecessary in case of rotation but since we
		 * cannot distinguish these two cases we always restart.
		 */
		vy_stmt_history_cleanup(&history, region_svp);
		goto restart;
	}

done:
	if (rc == 0) {
		rc = vy_point_lookup_apply_history(index, rv, key,
						   &history, ret);
	}
	vy_stmt_history_cleanup(&history, region_svp);

	if (rc != 0)
		return -1;

	double latency = ev_monotonic_now(loop()) - start_time;
	latency_collect(&index->stat.latency, latency);

	if (latency > index->env->too_long_threshold) {
		say_warn("%s: get(%s) => %s took too long: %.3f sec",
			 vy_index_name(index), tuple_str(key),
			 vy_stmt_str(*ret), latency);
	}
	return 0;
}
