/*
 * Copyright (C) 2018 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/ioctls.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <linux/log2.h>
#include <asm/unaligned.h>

#include "format.h"
#include "counters.h"
#include "inode.h"
#include "block.h"
#include "btree.h"
#include "scoutfs_trace.h"
#include "msg.h"
#include "server.h"
#include "net.h"
#include "lock_server.h"
#include "endian_swap.h"
#include "quorum.h"
#include "trans.h"
#include "srch.h"
#include "alloc.h"
#include "forest.h"
#include "recov.h"
#include "omap.h"
#include "fence.h"

/*
 * Every active mount can act as the server that listens on a net
 * connection and accepts connections from all the other mounts acting
 * as clients.
 *
 * The server is started by the mount that is elected leader by quorum.
 * If it sees errors it shuts down the server in the hopes that another
 * mount will become the leader and have less trouble.
 */

struct server_info {
	struct super_block *sb;
	spinlock_t lock;
	wait_queue_head_t waitq;

	struct workqueue_struct *wq;
	struct work_struct work;
	int err;
	bool shutting_down;
	struct completion start_comp;
	u64 term;
	struct scoutfs_net_connection *conn;

	/* request processing coordinates shared commits */
	struct rw_semaphore commit_rwsem;
	struct llist_head commit_waiters;
	struct work_struct commit_work;

	/* server tracks seq use */
	struct rw_semaphore seq_rwsem;

	struct list_head clients;
	unsigned long nr_clients;

	/* track clients waiting in unmmount for farewell response */
	spinlock_t farewell_lock;
	struct list_head farewell_requests;
	struct work_struct farewell_work;

	struct mutex alloc_mutex;
	/* swap between two fs meta roots to increase time to reuse */
	struct scoutfs_alloc_root *meta_avail;
	struct scoutfs_alloc_root *meta_freed;
	/* server's meta allocators alternate between persistent heads */
	struct scoutfs_alloc alloc;
	int other_ind;
	struct scoutfs_alloc_list_head *other_avail;
	struct scoutfs_alloc_list_head *other_freed;
	struct scoutfs_block_writer wri;

	struct mutex logs_mutex;
	struct mutex srch_mutex;
	struct mutex mounted_clients_mutex;

	/* stable versions stored from commits, given in locks and rpcs */
	seqcount_t roots_seqcount;
	struct scoutfs_net_roots roots;

	/* serializing and get and set volume options */
	seqcount_t volopt_seqcount;
	struct mutex volopt_mutex;
	struct scoutfs_volume_options volopt;

	/* recovery timeout fences from work */
	struct work_struct fence_pending_recov_work;
	/* while running we check for fenced mounts to reclaim */
	struct delayed_work reclaim_dwork;
};

#define DECLARE_SERVER_INFO(sb, name) \
	struct server_info *name = SCOUTFS_SB(sb)->server_info

/*
 * The server tracks each connected client.
 */
struct server_client_info {
	u64 rid;
	struct list_head head;
};

static __le64 *first_valopt(struct scoutfs_volume_options *valopt)
{
	return &valopt->set_bits + 1;
}

/*
 * A server caller wants to know if a volume option is set and wants to
 * know it's value.  This is quite early in the file to make it
 * available to all of the server paths.
 */
static bool get_volopt_val(struct server_info *server, int nr, u64 *val)
{
	u64 bit = 1ULL << nr;
	__le64 *opt = first_valopt(&server->volopt) + nr;
	bool is_set = false;
	unsigned seq;

	do {
		seq = read_seqcount_begin(&server->volopt_seqcount);
		if ((le64_to_cpu(server->volopt.set_bits) & bit)) {
			is_set = true;
			*val = le64_to_cpup(opt);
		} else {
			is_set = false;
			*val = 0;
		};
	} while (read_seqcount_retry(&server->volopt_seqcount, seq));

	return is_set;
}


struct commit_waiter {
	struct completion comp;
	struct llist_node node;
	int ret;
};

static void stop_server(struct server_info *server)
{
	/* wait_event/wake_up provide barriers */
	server->shutting_down = true;
	wake_up(&server->waitq);
}

/*
 * Hold the shared rwsem that lets multiple holders modify blocks in the
 * current commit and prevents the commit worker from acquiring the
 * exclusive write lock to write the commit.
 *
 * This is exported for server components isolated in their own files
 * (lock_server) and which are not called directly by the server core
 * (async timeout work).
 */
int scoutfs_server_hold_commit(struct super_block *sb)
{
	DECLARE_SERVER_INFO(sb, server);

	scoutfs_inc_counter(sb, server_commit_hold);

	down_read(&server->commit_rwsem);

	return 0;
}

/*
 * This is called while holding the commit and returns once the commit
 * is successfully written.  Many holders can all wait for all holders
 * to drain before their shared commit is applied and they're all woken.
 *
 * It's important to realize that our commit_waiter list node might be
 * serviced by a currently executing commit work that is blocked waiting
 * for the holders to release the commit_rwsem.  This caller can return
 * from wait_for_commit() while another future commit_work is still
 * queued.
 *
 * This could queue delayed work but we're first trying to have batching
 * work by having concurrent modification line up behind a commit in
 * flight.  Once the commit finishes it'll unlock and hopefully everyone
 * will race to make their changes and they'll all be applied by the
 * next commit after that.
 */
int scoutfs_server_apply_commit(struct super_block *sb, int err)
{
	DECLARE_SERVER_INFO(sb, server);
	struct commit_waiter cw;

	if (err == 0) {
		cw.ret = 0;
		init_completion(&cw.comp);
		llist_add(&cw.node, &server->commit_waiters);
		scoutfs_inc_counter(sb, server_commit_queue);
		queue_work(server->wq, &server->commit_work);
	}

	up_read(&server->commit_rwsem);

	if (err == 0) {
		wait_for_completion(&cw.comp);
		err = cw.ret;
	}

	return err;
}

static void get_roots(struct super_block *sb,
			      struct scoutfs_net_roots *roots)
{
	DECLARE_SERVER_INFO(sb, server);
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&server->roots_seqcount);
		*roots = server->roots;
	} while (read_seqcount_retry(&server->roots_seqcount, seq));
}

static void set_roots(struct server_info *server,
		      struct scoutfs_btree_root *fs_root,
		      struct scoutfs_btree_root *logs_root,
		      struct scoutfs_btree_root *srch_root)
{
	preempt_disable();
	write_seqcount_begin(&server->roots_seqcount);
	server->roots.fs_root = *fs_root;
	server->roots.logs_root = *logs_root;
	server->roots.srch_root = *srch_root;
	write_seqcount_end(&server->roots_seqcount);
	preempt_enable();
}

/*
 * Concurrent request processing dirties blocks in a commit and makes
 * the modifications persistent before replying.  We'd like to batch
 * these commits as much as is reasonable so that we don't degrade to a
 * few IO round trips per request.
 *
 * Getting that batching right is bound up in the concurrency of request
 * processing so a clear way to implement the batched commits is to
 * implement commits with a single pending work func like the
 * processing.
 *
 * Processing paths acquire the rwsem for reading while they're making
 * multiple dependent changes.  When they're done and want it persistent
 * they add themselves to the list of waiters and queue the commit work.
 * This work runs, acquires the lock to exclude other writers, and
 * performs the commit.  Readers can run concurrently with these
 * commits.
 */
static void scoutfs_server_commit_func(struct work_struct *work)
{
	struct server_info *server = container_of(work, struct server_info,
						  commit_work);
	struct super_block *sb = server->sb;
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct commit_waiter *cw;
	struct commit_waiter *pos;
	struct llist_node *node;
	int ret;

	trace_scoutfs_server_commit_work_enter(sb, 0, 0);
	scoutfs_inc_counter(sb, server_commit_worker);

	down_write(&server->commit_rwsem);

	if (scoutfs_forcing_unmount(sb)) {
		ret = -EIO;
		goto out;
	}

	/* make sure next avail has sufficient blocks */
	ret = scoutfs_alloc_fill_list(sb, &server->alloc, &server->wri,
				      server->other_avail,
				      server->meta_avail,
				      SCOUTFS_SERVER_META_FILL_LO,
				      SCOUTFS_SERVER_META_FILL_TARGET);
	if (ret) {
		scoutfs_err(sb, "server error refilling avail: %d", ret);
		goto out;
	}

	/* merge freed blocks into extents, might be partial */
	ret = scoutfs_alloc_empty_list(sb, &server->alloc, &server->wri,
				       server->meta_freed,
				       server->other_freed);
	if (ret) {
		scoutfs_err(sb, "server error emptying freed: %d", ret);
		goto out;
	}

	ret = scoutfs_alloc_prepare_commit(sb, &server->alloc, &server->wri);
	if (ret < 0) {
		scoutfs_err(sb, "server error prepare alloc commit: %d", ret);
		goto out;
	}

	ret = scoutfs_block_writer_write(sb, &server->wri);
	if (ret) {
		scoutfs_err(sb, "server error writing btree blocks: %d", ret);
		goto out;
	}

	super->server_meta_avail[server->other_ind ^ 1] = server->alloc.avail;
	super->server_meta_freed[server->other_ind ^ 1] = server->alloc.freed;

	ret = scoutfs_write_super(sb, super);
	if (ret) {
		scoutfs_err(sb, "server error writing super block: %d", ret);
		goto out;
	}

	set_roots(server, &super->fs_root, &super->logs_root,
		  &super->srch_root);

	/* swizzle the active and idle server alloc/freed heads */
	server->other_ind ^= 1;
	server->alloc.avail = super->server_meta_avail[server->other_ind ^ 1];
	server->alloc.freed = super->server_meta_freed[server->other_ind ^ 1];
	server->other_avail = &super->server_meta_avail[server->other_ind];
	server->other_freed = &super->server_meta_freed[server->other_ind];

	/* swap avail/free if avail gets low and freed is high */
	if (le64_to_cpu(server->meta_avail->total_len) <=
	    SCOUTFS_SERVER_META_ALLOC_MIN &&
	    le64_to_cpu(server->meta_freed->total_len) >
	    SCOUTFS_SERVER_META_ALLOC_MIN)
		swap(server->meta_avail, server->meta_freed);

	ret = 0;
out:
	node = llist_del_all(&server->commit_waiters);

	/* waiters always wait on completion, cw could be free after complete */
	llist_for_each_entry_safe(cw, pos, node, node) {
		cw->ret = ret;
		complete(&cw->comp);
	}

	up_write(&server->commit_rwsem);
	trace_scoutfs_server_commit_work_exit(sb, 0, ret);
}

static int server_alloc_inodes(struct super_block *sb,
			       struct scoutfs_net_connection *conn,
			       u8 cmd, u64 id, void *arg, u16 arg_len)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_net_inode_alloc ial = { 0, };
	__le64 lecount;
	u64 ino;
	u64 nr;
	int ret;

	if (arg_len != sizeof(lecount)) {
		ret = -EINVAL;
		goto out;
	}

	memcpy(&lecount, arg, arg_len);

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto out;

	spin_lock(&sbi->next_ino_lock);
	ino = le64_to_cpu(super->next_ino);
	nr = min(le64_to_cpu(lecount), U64_MAX - ino);
	le64_add_cpu(&super->next_ino, nr);
	spin_unlock(&sbi->next_ino_lock);

	ret = scoutfs_server_apply_commit(sb, ret);
	if (ret == 0) {
		ial.ino = cpu_to_le64(ino);
		ial.nr = cpu_to_le64(nr);
	}
out:
	return scoutfs_net_response(sb, conn, cmd, id, ret, &ial, sizeof(ial));
}

/*
 * Refill the destination root if it's fallen below the lo threshold by
 * moving from the src root to bring it up to the target.
 */
static int alloc_move_refill_zoned(struct super_block *sb, struct scoutfs_alloc_root *dst,
				   struct scoutfs_alloc_root *src, u64 lo, u64 target,
				   __le64 *exclusive, __le64 *vacant, u64 zone_blocks)
{
	DECLARE_SERVER_INFO(sb, server);

	if (le64_to_cpu(dst->total_len) >= lo)
		return 0;

	return scoutfs_alloc_move(sb, &server->alloc, &server->wri, dst, src,
				  min(target - le64_to_cpu(dst->total_len),
				      le64_to_cpu(src->total_len)),
				  exclusive, vacant, zone_blocks);
}

static inline int alloc_move_refill(struct super_block *sb, struct scoutfs_alloc_root *dst,
				    struct scoutfs_alloc_root *src, u64 lo, u64 target)
{
	return alloc_move_refill_zoned(sb, dst, src, lo, target, NULL, NULL, 0);
}

static int alloc_move_empty(struct super_block *sb,
			    struct scoutfs_alloc_root *dst,
			    struct scoutfs_alloc_root *src)
{
	DECLARE_SERVER_INFO(sb, server);

	return scoutfs_alloc_move(sb, &server->alloc, &server->wri,
				  dst, src, le64_to_cpu(src->total_len), NULL, NULL, 0);
}

/*
 * Set all the bits in the destination which overlap with the extent.
 */
static void mod_extent_bits(__le64 *bits, u64 zone_blocks, u64 blkno, u64 len, bool set)
{
	u64 nr = div64_u64(blkno, zone_blocks);
	u64 last_nr = div64_u64(blkno + len - 1, zone_blocks);

	if (WARN_ON_ONCE(len == 0))
		return;

	while (nr <= last_nr) {
		if (set)
			set_bit_le(nr, bits);
		else
			clear_bit_le(nr, bits);

		nr++;
	}
}

/*
 * Translate the bits in the source bitmap into extents and modify bits
 * in the destination that map those extents.
 */
static void mod_bitmap_bits(__le64 *dst, u64 dst_zone_blocks,
			    __le64 *src, u64 src_zone_blocks, bool set)
{
	int nr = 0;

	for (;;) {
		nr = find_next_bit_le(src, SCOUTFS_DATA_ALLOC_MAX_ZONES, nr);
		if (nr >= SCOUTFS_DATA_ALLOC_MAX_ZONES)
			break;

		mod_extent_bits(dst, dst_zone_blocks,
				(u64)nr * src_zone_blocks, src_zone_blocks, set);
		nr++;
	}
}

/*
 * Iterate over all the log_tree items and initialize the caller's zone
 * bitmaps.  Exclusive bits are only found in the caller's items.
 * Vacant bits are not found in any items.
 *
 * The log_tree item zone bitmaps could have been stored with different
 * zone_blocks sizes.  We translate the bits into block extents and
 * record overlaps with the current zone size.
 *
 * The caller has the log items locked.
 */
static int get_data_alloc_zone_bits(struct super_block *sb, u64 rid, __le64 *exclusive,
				    __le64 *vacant, u64 zone_blocks)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_log_trees *lt;
	struct scoutfs_key key;
	int ret;

	memset(exclusive, 0, SCOUTFS_DATA_ALLOC_ZONE_BYTES);
	memset(vacant, 0, SCOUTFS_DATA_ALLOC_ZONE_BYTES);

	mod_extent_bits(vacant, zone_blocks, 0, le64_to_cpu(super->total_data_blocks), true);

	scoutfs_key_init_log_trees(&key, 0, 0);
	for (;;) {
		ret = scoutfs_btree_next(sb, &super->logs_root, &key, &iref);
		if (ret == 0) {
			if (iref.val_len == sizeof(struct scoutfs_log_trees)) {
				lt = iref.val;

				/* vacant bits have no bits found in items */
				mod_bitmap_bits(vacant, zone_blocks,
						lt->data_alloc_zones,
						le64_to_cpu(lt->data_alloc_zone_blocks),
						false);

				/* exclusive bits are only found in caller's items */
				if (le64_to_cpu(iref.key->sklt_rid) == rid) {
					mod_bitmap_bits(exclusive, zone_blocks,
							lt->data_alloc_zones,
							le64_to_cpu(lt->data_alloc_zone_blocks),
							true);
				} else {
					mod_bitmap_bits(exclusive, zone_blocks,
							lt->data_alloc_zones,
							le64_to_cpu(lt->data_alloc_zone_blocks),
							false);
				}

				key = *iref.key;
				scoutfs_key_inc(&key);
			} else {
				ret = -EIO;
			}
			scoutfs_btree_put_iref(&iref);
		}
		if (ret < 0) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}
	}

	return ret;
}

static void zero_data_alloc_zone_bits(struct scoutfs_log_trees *lt)
{
	lt->data_alloc_zone_blocks = 0;
	memset(lt->data_alloc_zones, 0, sizeof(lt->data_alloc_zones));
}

struct alloc_extent_cb_args {
	__le64 *zones;
	u64 zone_blocks;
};

static void set_extent_zone_bits(struct super_block *sb, void *cb_arg, struct scoutfs_extent *ext)
{
	struct alloc_extent_cb_args *cba = cb_arg;

	mod_extent_bits(cba->zones, cba->zone_blocks, ext->start, ext->len, true);
}

/*
 * Give the client roots to all the trees that they'll use to build
 * their transaction.
 *
 * We make sure that their alloc trees have sufficient blocks to
 * allocate metadata and data for the transaction.  We merge their freed
 * trees back into the core allocators.  They're were committed with the
 * previous transaction so they're stable and can now be reused, even by
 * the server in this commit.
 */
static int server_get_log_trees(struct super_block *sb,
				struct scoutfs_net_connection *conn,
				u8 cmd, u64 id, void *arg, u16 arg_len)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	u64 rid = scoutfs_net_client_rid(conn);
	DECLARE_SERVER_INFO(sb, server);
	__le64 exclusive[SCOUTFS_DATA_ALLOC_ZONE_LE64S];
	__le64 vacant[SCOUTFS_DATA_ALLOC_ZONE_LE64S];
	struct alloc_extent_cb_args cba;
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_log_trees lt;
	struct scoutfs_key key;
	u64 data_zone_blocks;
	int ret;

	if (arg_len != 0) {
		ret = -EINVAL;
		goto out;
	}

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto out;

	mutex_lock(&server->logs_mutex);

	scoutfs_key_init_log_trees(&key, rid, U64_MAX);

	ret = scoutfs_btree_prev(sb, &super->logs_root, &key, &iref);
	if (ret < 0 && ret != -ENOENT)
		goto unlock;
	if (ret == 0) {
		if (iref.val_len == sizeof(struct scoutfs_log_trees)) {
			key = *iref.key;
			memcpy(&lt, iref.val, iref.val_len);
			if (le64_to_cpu(key.sklt_rid) != rid)
				ret = -ENOENT;
		} else {
			ret = -EIO;
		}
		scoutfs_btree_put_iref(&iref);
		if (ret == -EIO)
			goto unlock;
	}

	/* initialize new roots if we don't have any */
	if (ret == -ENOENT) {
		key.sklt_rid = cpu_to_le64(rid);
		key.sklt_nr = cpu_to_le64(1);
		memset(&lt, 0, sizeof(lt));
		lt.rid = key.sklt_rid;
		lt.nr = key.sklt_nr;
	}

	if (get_volopt_val(server, SCOUTFS_VOLOPT_DATA_ALLOC_ZONE_BLOCKS_NR, &data_zone_blocks)) {
		ret = get_data_alloc_zone_bits(sb, rid, exclusive, vacant, data_zone_blocks);
		if (ret < 0)
			goto unlock;
	} else {
		data_zone_blocks = 0;
	}

	/* return freed to server for emptying, refill avail  */
	mutex_lock(&server->alloc_mutex);
	ret = scoutfs_alloc_splice_list(sb, &server->alloc, &server->wri,
					server->other_freed,
					&lt.meta_freed) ?:
	      alloc_move_empty(sb, &super->data_alloc, &lt.data_freed) ?:
	      scoutfs_alloc_fill_list(sb, &server->alloc, &server->wri,
				      &lt.meta_avail, server->meta_avail,
				      SCOUTFS_SERVER_META_FILL_LO,
				      SCOUTFS_SERVER_META_FILL_TARGET) ?:
	      alloc_move_refill_zoned(sb, &lt.data_avail, &super->data_alloc,
				      SCOUTFS_SERVER_DATA_FILL_LO,
				      SCOUTFS_SERVER_DATA_FILL_TARGET,
				      exclusive, vacant, data_zone_blocks);
	mutex_unlock(&server->alloc_mutex);
	if (ret < 0)
		goto unlock;

	/* record data alloc zone bits */
	zero_data_alloc_zone_bits(&lt);
	if (data_zone_blocks != 0) {
		cba.zones = lt.data_alloc_zones;
		cba.zone_blocks = data_zone_blocks;
		ret = scoutfs_alloc_extents_cb(sb, &lt.data_avail, set_extent_zone_bits, &cba);
		if (ret < 0) {
			zero_data_alloc_zone_bits(&lt);
			goto unlock;
		}

		lt.data_alloc_zone_blocks = cpu_to_le64(data_zone_blocks);
	}

	/* update client's log tree's item */
	ret = scoutfs_btree_force(sb, &server->alloc, &server->wri,
				  &super->logs_root, &key, &lt, sizeof(lt));
unlock:
	mutex_unlock(&server->logs_mutex);

	ret = scoutfs_server_apply_commit(sb, ret);
out:
	WARN_ON_ONCE(ret < 0);
	return scoutfs_net_response(sb, conn, cmd, id, ret, &lt, sizeof(lt));
}

/*
 * The client is sending the roots of all the btree blocks that they
 * wrote to their free space for their transaction.  Make it persistent
 * by referencing the roots from their log item in the logs root and
 * committing.
 */
static int server_commit_log_trees(struct super_block *sb,
				   struct scoutfs_net_connection *conn,
				   u8 cmd, u64 id, void *arg, u16 arg_len)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	DECLARE_SERVER_INFO(sb, server);
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_log_trees lt;
	struct scoutfs_key key;
	int ret;

	if (arg_len != sizeof(struct scoutfs_log_trees)) {
		ret = -EINVAL;
		goto out;
	}

	/* don't modify the caller's log_trees */
	memcpy(&lt, arg, sizeof(struct scoutfs_log_trees));

	ret = scoutfs_server_hold_commit(sb);
	if (ret < 0) {
		scoutfs_err(sb, "server error preparing commit: %d", ret);
		goto out;
	}

	mutex_lock(&server->logs_mutex);

	/* find the client's existing item */
	scoutfs_key_init_log_trees(&key, le64_to_cpu(lt.rid),
				   le64_to_cpu(lt.nr));
	ret = scoutfs_btree_lookup(sb, &super->logs_root, &key, &iref);
	if (ret < 0) {
		scoutfs_err(sb, "server error finding client logs: %d", ret);
		goto unlock;
	}
	if (ret == 0)
		scoutfs_btree_put_iref(&iref);

	/* try to rotate the srch log when big enough */
	mutex_lock(&server->srch_mutex);
	ret = scoutfs_srch_rotate_log(sb, &server->alloc, &server->wri,
				      &super->srch_root, &lt.srch_file);
	mutex_unlock(&server->srch_mutex);
	if (ret < 0) {
		scoutfs_err(sb, "server error, rotating srch log: %d", ret);
		goto unlock;
	}

	ret = scoutfs_btree_update(sb, &server->alloc, &server->wri,
				   &super->logs_root, &key, &lt, sizeof(lt));
	if (ret < 0)
		scoutfs_err(sb, "server error updating client logs: %d", ret);

unlock:
	mutex_unlock(&server->logs_mutex);

	ret = scoutfs_server_apply_commit(sb, ret);
	if (ret < 0)
		scoutfs_err(sb, "server error commiting client logs: %d", ret);
out:
	WARN_ON_ONCE(ret < 0);
	return scoutfs_net_response(sb, conn, cmd, id, ret, NULL, 0);
}

/*
 * Give the client the most recent version of the fs btrees that are
 * visible in persistent storage.  We don't want to accidentally give
 * them our in-memory dirty version.  This can be racing with commits.
 */
static int server_get_roots(struct super_block *sb,
			    struct scoutfs_net_connection *conn,
			    u8 cmd, u64 id, void *arg, u16 arg_len)
{
	struct scoutfs_net_roots roots;
	int ret;

	if (arg_len != 0) {
		memset(&roots, 0, sizeof(roots));
		ret = -EINVAL;
	}  else {
		get_roots(sb, &roots);
		ret = 0;
	}

	return scoutfs_net_response(sb, conn, cmd, id, 0,
				    &roots, sizeof(roots));
}

/*
 * A client is being evicted so we want to reclaim resources from their
 * log tree items.  The item trees and bloom refs stay around to be read
 * and eventually merged and we reclaim all the allocator items.
 *
 * The caller holds the commit rwsem which means we do all this work in
 * one server commit.  We'll need to keep the total amount of blocks in
 * trees in check.
 *
 * By the time we're evicting a client they've either synced their data
 * or have been forcefully removed.  The free blocks in the allocator
 * roots are stable and can be merged back into allocator items for use
 * without risking overwriting stable data.
 *
 * We can return an error without fully reclaiming all the log item's
 * referenced data.
 */
static int reclaim_log_trees(struct super_block *sb, u64 rid)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	DECLARE_SERVER_INFO(sb, server);
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_log_trees lt;
	struct scoutfs_key key;
	int ret;
	int err;

	mutex_lock(&server->logs_mutex);

	/* find the client's existing item */
	scoutfs_key_init_log_trees(&key, rid, 0);
	ret = scoutfs_btree_next(sb, &super->logs_root, &key, &iref);
	if (ret == 0) {
		if (iref.val_len == sizeof(struct scoutfs_log_trees)) {
			key = *iref.key;
			memcpy(&lt, iref.val, iref.val_len);
			if (le64_to_cpu(key.sklt_rid) != rid)
				ret = -ENOENT;
		} else {
			ret = -EIO;
		}
		scoutfs_btree_put_iref(&iref);
	}
	if (ret < 0) {
		if (ret == -ENOENT)
			ret = 0;
		goto out;
	}

	/*
	 * All of these can return errors after having modified the
	 * allocator trees.  We have to try and update the roots in the
	 * log item.
	 */
	mutex_lock(&server->alloc_mutex);
	ret = scoutfs_alloc_splice_list(sb, &server->alloc, &server->wri,
					server->other_freed,
					&lt.meta_freed) ?:
	      scoutfs_alloc_splice_list(sb, &server->alloc, &server->wri,
					server->other_freed,
					&lt.meta_avail) ?:
	      alloc_move_empty(sb, &super->data_alloc, &lt.data_avail) ?:
	      alloc_move_empty(sb, &super->data_alloc, &lt.data_freed);
	mutex_unlock(&server->alloc_mutex);

	/* the mount is no longer writing to the zones */
	zero_data_alloc_zone_bits(&lt);

	err = scoutfs_btree_update(sb, &server->alloc, &server->wri,
				  &super->logs_root, &key, &lt, sizeof(lt));
	BUG_ON(err != 0); /* alloc and log item roots out of sync */

out:
	mutex_unlock(&server->logs_mutex);

	return ret;
}

static void init_trans_seq_key(struct scoutfs_key *key, u64 seq, u64 rid)
{
	*key = (struct scoutfs_key) {
		.sk_zone = SCOUTFS_TRANS_SEQ_ZONE,
		.skts_trans_seq = cpu_to_le64(seq),
		.skts_rid = cpu_to_le64(rid),
	};
}

/*
 * Remove all trans_seq items owned by the client rid, the caller holds
 * the seq_rwsem.
 */
static int remove_trans_seq_locked(struct super_block *sb, u64 rid)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_key key;
	int ret = 0;

	init_trans_seq_key(&key, 0, 0);

	for (;;) {
		ret = scoutfs_btree_next(sb, &super->trans_seqs, &key, &iref);
		if (ret < 0) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}

		key = *iref.key;
		scoutfs_btree_put_iref(&iref);

		if (le64_to_cpu(key.skts_rid) == rid) {
			trace_scoutfs_trans_seq_remove(sb, rid,
					le64_to_cpu(key.skts_trans_seq));
			ret = scoutfs_btree_delete(sb, &server->alloc,
						   &server->wri,
						   &super->trans_seqs, &key);
			if (ret < 0)
				break;
		}

		scoutfs_key_inc(&key);
	}

	return ret;
}

/*
 * Give the client the next sequence number for the transaction that
 * they're opening.
 *
 * We track the sequence numbers of transactions that clients have open.
 * This limits the transaction sequence numbers that can be returned in
 * the index of inodes by meta and data transaction numbers.  We
 * communicate the largest possible sequence number to clients via an
 * rpc.
 *
 * The transaction sequence tracking is stored in a btree so it is
 * shared across servers.  Final entries are removed when processing a
 * client's farewell or when it's removed.  We can be processent a
 * resent request that was committed by a previous server before the
 * reply was lost.  At this point the client has no transactions open
 * and may or may not have just finished one.  To keep it simple we
 * always remove any previous seq items, if there are any, and then
 * insert a new item for the client at the next greatest seq.
 */
static int server_advance_seq(struct super_block *sb,
			      struct scoutfs_net_connection *conn,
			      u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	u64 rid = scoutfs_net_client_rid(conn);
	struct scoutfs_key key;
	__le64 leseq = 0;
	u64 seq;
	int ret;

	if (arg_len != 0) {
		ret = -EINVAL;
		goto out;
	}

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto out;

	down_write(&server->seq_rwsem);

	ret = remove_trans_seq_locked(sb, rid);
	if (ret < 0)
		goto unlock;

	seq = le64_to_cpu(super->next_trans_seq);
	le64_add_cpu(&super->next_trans_seq, 1);

	trace_scoutfs_trans_seq_advance(sb, rid, seq);

	init_trans_seq_key(&key, seq, rid);
	ret = scoutfs_btree_insert(sb, &server->alloc, &server->wri,
				   &super->trans_seqs, &key, NULL, 0);
	if (ret == 0)
		leseq = cpu_to_le64(seq);
unlock:
	up_write(&server->seq_rwsem);
	ret = scoutfs_server_apply_commit(sb, ret);

out:
	return scoutfs_net_response(sb, conn, cmd, id, ret,
				    &leseq, sizeof(leseq));
}

/*
 * Remove any transaction sequences owned by the client who's sent a
 * farewell They must have committed any final transaction by the time
 * they get here via sending their farewell message.  This can be called
 * multiple times as the client's farewell is retransmitted so it's OK
 * to not find any entries.  This is called with the server commit rwsem
 * held.
 */
static int remove_trans_seq(struct super_block *sb, u64 rid)
{
	DECLARE_SERVER_INFO(sb, server);
	int ret = 0;

	down_write(&server->seq_rwsem);
	ret = remove_trans_seq_locked(sb, rid);
	up_write(&server->seq_rwsem);

	return ret;
}

/*
 * Give the calling client the last valid trans_seq that it can return
 * in results from the indices of trans seqs to inodes.  These indices
 * promise to only advance so we can't return results past those that
 * are still outstanding and not yet visible in the indices.  If there
 * are no outstanding transactions (what?  how?) we give them the max
 * possible sequence.
 */
static int server_get_last_seq(struct super_block *sb,
			       struct scoutfs_net_connection *conn,
			       u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	SCOUTFS_BTREE_ITEM_REF(iref);
	u64 rid = scoutfs_net_client_rid(conn);
	struct scoutfs_key key;
	__le64 last_seq = 0;
	int ret;

	if (arg_len != 0) {
		ret = -EINVAL;
		goto out;
	}

	down_read(&server->seq_rwsem);

	init_trans_seq_key(&key, 0, 0);
	ret = scoutfs_btree_next(sb, &super->trans_seqs, &key, &iref);
	if (ret == 0) {
		key = *iref.key;
		scoutfs_btree_put_iref(&iref);
		last_seq = key.skts_trans_seq;

	} else if (ret == -ENOENT) {
		last_seq = super->next_trans_seq;
		ret = 0;
	}

	le64_add_cpu(&last_seq, -1ULL);
	trace_scoutfs_trans_seq_last(sb, rid, le64_to_cpu(last_seq));

	up_read(&server->seq_rwsem);
out:
	return scoutfs_net_response(sb, conn, cmd, id, ret,
				    &last_seq, sizeof(last_seq));
}

static int server_lock(struct super_block *sb,
		       struct scoutfs_net_connection *conn,
		       u8 cmd, u64 id, void *arg, u16 arg_len)
{
	u64 rid = scoutfs_net_client_rid(conn);

	if (arg_len != sizeof(struct scoutfs_net_lock))
		return -EINVAL;

	return scoutfs_lock_server_request(sb, rid, id, arg);
}

static int lock_response(struct super_block *sb,
			 struct scoutfs_net_connection *conn,
			 void *resp, unsigned int resp_len,
			 int error, void *data)
{
	u64 rid = scoutfs_net_client_rid(conn);

	if (resp_len != sizeof(struct scoutfs_net_lock))
		return -EINVAL;

	return scoutfs_lock_server_response(sb, rid, resp);
}

int scoutfs_server_lock_request(struct super_block *sb, u64 rid,
				struct scoutfs_net_lock *nl)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;

	return scoutfs_net_submit_request_node(sb, server->conn, rid,
					      SCOUTFS_NET_CMD_LOCK,
					      nl, sizeof(*nl),
					      lock_response, NULL, NULL);
}

int scoutfs_server_lock_response(struct super_block *sb, u64 rid, u64 id,
				 struct scoutfs_net_lock *nl)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;

	return scoutfs_net_response_node(sb, server->conn, rid,
					 SCOUTFS_NET_CMD_LOCK, id, 0,
					 nl, sizeof(*nl));
}

static bool invalid_recover(struct scoutfs_net_lock_recover *nlr,
			    unsigned long bytes)
{
	return ((bytes < sizeof(*nlr)) ||
	        (bytes != offsetof(struct scoutfs_net_lock_recover,
			       locks[le16_to_cpu(nlr->nr)])));
}

static int lock_recover_response(struct super_block *sb,
				 struct scoutfs_net_connection *conn,
				 void *resp, unsigned int resp_len,
				 int error, void *data)
{
	u64 rid = scoutfs_net_client_rid(conn);

	if (invalid_recover(resp, resp_len))
		return -EINVAL;

	return scoutfs_lock_server_recover_response(sb, rid, resp);
}

int scoutfs_server_lock_recover_request(struct super_block *sb, u64 rid,
					struct scoutfs_key *key)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;

	return scoutfs_net_submit_request_node(sb, server->conn, rid,
					      SCOUTFS_NET_CMD_LOCK_RECOVER,
					      key, sizeof(*key),
					      lock_recover_response,
					      NULL, NULL);
}

static int server_srch_get_compact(struct super_block *sb,
				   struct scoutfs_net_connection *conn,
				   u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	u64 rid = scoutfs_net_client_rid(conn);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_srch_compact *sc = NULL;
	int ret;

	if (arg_len != 0) {
		ret = -EINVAL;
		goto out;
	}

	sc = kzalloc(sizeof(struct scoutfs_srch_compact), GFP_NOFS);
	if (sc == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto out;

	mutex_lock(&server->srch_mutex);
	ret = scoutfs_srch_get_compact(sb, &server->alloc, &server->wri,
				       &super->srch_root, rid, sc);
	mutex_unlock(&server->srch_mutex);
	if (ret == 0 && sc->nr == 0)
		ret = -ENOENT;
	if (ret < 0)
		goto apply;

	mutex_lock(&server->alloc_mutex);
	ret = scoutfs_alloc_fill_list(sb, &server->alloc, &server->wri,
				      &sc->meta_avail, server->meta_avail,
				      SCOUTFS_SERVER_META_FILL_LO,
				      SCOUTFS_SERVER_META_FILL_TARGET) ?:
	      scoutfs_alloc_splice_list(sb, &server->alloc, &server->wri,
					server->other_freed, &sc->meta_freed);
	mutex_unlock(&server->alloc_mutex);
	if (ret < 0)
		goto apply;

	mutex_lock(&server->srch_mutex);
	ret = scoutfs_srch_update_compact(sb, &server->alloc, &server->wri,
					  &super->srch_root, rid, sc);
	mutex_unlock(&server->srch_mutex);

apply:
	ret = scoutfs_server_apply_commit(sb, ret);
	WARN_ON_ONCE(ret < 0 && ret != -ENOENT); /* XXX leaked busy item */
out:
	ret = scoutfs_net_response(sb, conn, cmd, id, ret,
				   sc, sizeof(struct scoutfs_srch_compact));
	kfree(sc);
	return ret;
}

/*
 * Commit the client's compaction.  Their freed allocator contains the
 * source srch files blocks that are currently in use which can't be
 * available for allocation until after the commit.  We move them into
 * freed so they won't satisfy allocations.
 */
static int server_srch_commit_compact(struct super_block *sb,
				      struct scoutfs_net_connection *conn,
				      u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	u64 rid = scoutfs_net_client_rid(conn);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_srch_compact *sc;
	struct scoutfs_alloc_list_head av;
	struct scoutfs_alloc_list_head fr;
	int ret;

	if (arg_len != sizeof(struct scoutfs_srch_compact)) {
		ret = -EINVAL;
		goto out;
	}
	sc = arg;

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto out;

	mutex_lock(&server->srch_mutex);
	ret = scoutfs_srch_commit_compact(sb, &server->alloc, &server->wri,
					  &super->srch_root, rid, sc,
					  &av, &fr);
	mutex_unlock(&server->srch_mutex);
	if (ret < 0) /* XXX very bad, leaks allocators */
		goto apply;

	/* reclaim allocators if they were set by _srch_commit_ */
	mutex_lock(&server->alloc_mutex);
	ret = scoutfs_alloc_splice_list(sb, &server->alloc, &server->wri,
					server->other_freed, &av) ?:
	      scoutfs_alloc_splice_list(sb, &server->alloc, &server->wri,
					server->other_freed, &fr);
	mutex_unlock(&server->alloc_mutex);
apply:
	ret = scoutfs_server_apply_commit(sb, ret);
out:
	WARN_ON(ret < 0); /* XXX leaks allocators */
	return scoutfs_net_response(sb, conn, cmd, id, ret, NULL, 0);
}

/* The server is receiving an omap response from the client */
static int open_ino_map_response(struct super_block *sb, struct scoutfs_net_connection *conn,
				 void *resp, unsigned int resp_len, int error, void *data)
{
	u64 rid = scoutfs_net_client_rid(conn);

	if (resp_len != sizeof(struct scoutfs_open_ino_map))
		return -EINVAL;

	return scoutfs_omap_server_handle_response(sb, rid, resp);
}

/* The server is sending an omap request to the client */
int scoutfs_server_send_omap_request(struct super_block *sb, u64 rid,
				     struct scoutfs_open_ino_map_args *args)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;

	return scoutfs_net_submit_request_node(sb, server->conn, rid, SCOUTFS_NET_CMD_OPEN_INO_MAP,
					      args, sizeof(*args),
					      open_ino_map_response, NULL, NULL);
}

/* The server is sending an omap response to the client */
int scoutfs_server_send_omap_response(struct super_block *sb, u64 rid, u64 id,
				      struct scoutfs_open_ino_map *map, int err)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;

	return scoutfs_net_response_node(sb, server->conn, rid,
					 SCOUTFS_NET_CMD_OPEN_INO_MAP, id, err,
					 map, sizeof(*map));
}

/* The server is receiving an omap request from the client */
static int server_open_ino_map(struct super_block *sb, struct scoutfs_net_connection *conn,
			       u8 cmd, u64 id, void *arg, u16 arg_len)
{
	u64 rid = scoutfs_net_client_rid(conn);
	int ret;

	if (arg_len != sizeof(struct scoutfs_open_ino_map_args)) {
		ret = -EINVAL;
		goto out;
	}

	ret = scoutfs_omap_server_handle_request(sb, rid, id, arg);
out:
	if (ret < 0)
		return scoutfs_net_response(sb, conn, cmd, id, ret, NULL, 0);

	return 0;
}

/* The server is receiving a request for the current volume options */
static int server_get_volopt(struct super_block *sb, struct scoutfs_net_connection *conn,
			     u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_volume_options volopt;
	unsigned seq;
	int ret = 0;

	if (arg_len != 0) {
		ret = -EINVAL;
		goto out;
	}

	do {
		seq = read_seqcount_begin(&server->volopt_seqcount);
		volopt = server->volopt;
	} while (read_seqcount_retry(&server->volopt_seqcount, seq));

out:
	return scoutfs_net_response(sb, conn, cmd, id, ret, &volopt, sizeof(volopt));
}

/*
 * The server is receiving a request to update volume options.
 *
 * The in-memory options that readers use is updated only once the
 * updated options are written in the super block.
 */
static int server_set_volopt(struct super_block *sb, struct scoutfs_net_connection *conn,
			     u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_volume_options *volopt;
	u64 opt;
	u64 nr;
	int ret = 0;

	if (arg_len != sizeof(struct scoutfs_volume_options)) {
		ret = -EINVAL;
		goto out;
	}
	volopt = arg;

	if (le64_to_cpu(volopt->set_bits) & SCOUTFS_VOLOPT_EXPANSION_BITS) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&server->volopt_mutex);

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto unlock;

	if (le64_to_cpu(volopt->set_bits) & SCOUTFS_VOLOPT_DATA_ALLOC_ZONE_BLOCKS_BIT) {
		opt = le64_to_cpu(volopt->data_alloc_zone_blocks);
		if (opt < SCOUTFS_SERVER_DATA_FILL_TARGET) {
			scoutfs_err(sb, "setting data_alloc_zone_blocks to '%llu' failed, must be at least %llu mount data allocation target blocks",
				    opt, SCOUTFS_SERVER_DATA_FILL_TARGET);
			ret = -EINVAL;
			goto apply;
		}

		nr = div_u64(le64_to_cpu(super->total_data_blocks), SCOUTFS_DATA_ALLOC_MAX_ZONES);
		if (opt < nr) {
			scoutfs_err(sb, "setting data_alloc_zone_blocks to '%llu' failed, must be greater than %llu blocks which results in max %u zones",
				    opt, nr, SCOUTFS_DATA_ALLOC_MAX_ZONES);
			ret = -EINVAL;
			goto apply;
		}

		if (opt > le64_to_cpu(super->total_data_blocks)) {
			scoutfs_err(sb, "setting data_alloc_zone_blocks to '%llu' failed, must be at most %llu total data device blocks",
				    opt, le64_to_cpu(super->total_data_blocks));
			ret = -EINVAL;
			goto apply;
		}

		super->volopt.data_alloc_zone_blocks = volopt->data_alloc_zone_blocks;
		super->volopt.set_bits |= cpu_to_le64(SCOUTFS_VOLOPT_DATA_ALLOC_ZONE_BLOCKS_BIT);
	}

apply:
	ret = scoutfs_server_apply_commit(sb, ret);

	write_seqcount_begin(&server->volopt_seqcount);
	if (ret == 0)
		server->volopt = super->volopt;
	else
		super->volopt = server->volopt;
	write_seqcount_end(&server->volopt_seqcount);

unlock:
	mutex_unlock(&server->volopt_mutex);
out:
	return scoutfs_net_response(sb, conn, cmd, id, ret, NULL, 0);
}

static int server_clear_volopt(struct super_block *sb, struct scoutfs_net_connection *conn,
			       u8 cmd, u64 id, void *arg, u16 arg_len)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_volume_options *volopt;
	__le64 *opt;
	u64 bit;
	int ret = 0;
	int i;

	if (arg_len != sizeof(struct scoutfs_volume_options)) {
		ret = -EINVAL;
		goto out;
	}
	volopt = arg;

	if (le64_to_cpu(volopt->set_bits) & SCOUTFS_VOLOPT_EXPANSION_BITS) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&server->volopt_mutex);

	ret = scoutfs_server_hold_commit(sb);
	if (ret)
		goto unlock;

	for (i = 0, bit = 1, opt = first_valopt(&super->volopt); i < 64; i++, bit <<= 1, opt++) {
		if (le64_to_cpu(volopt->set_bits) & bit) {
			super->volopt.set_bits &= ~cpu_to_le64(bit);
			*opt = 0;
		}
	}

	ret = scoutfs_server_apply_commit(sb, ret);

	write_seqcount_begin(&server->volopt_seqcount);
	if (ret == 0)
		server->volopt = super->volopt;
	else
		super->volopt = server->volopt;
	write_seqcount_end(&server->volopt_seqcount);

unlock:
	mutex_unlock(&server->volopt_mutex);
out:
	return scoutfs_net_response(sb, conn, cmd, id, ret, NULL, 0);
}

static void init_mounted_client_key(struct scoutfs_key *key, u64 rid)
{
	*key = (struct scoutfs_key) {
		.sk_zone = SCOUTFS_MOUNTED_CLIENT_ZONE,
		.skmc_rid = cpu_to_le64(rid),
	};
}

/*
 * Insert a new mounted client item for a client that is sending us a
 * greeting that hasn't yet seen a response.  The greeting can be
 * retransmitted to a new server after the previous inserted the item so
 * it's acceptable to see -EEXIST.
 */
static int insert_mounted_client(struct super_block *sb, u64 rid,
				 u64 gr_flags)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_mounted_client_btree_val mcv;
	struct scoutfs_key key;
	int ret;

	init_mounted_client_key(&key, rid);
	mcv.flags = 0;
	if (gr_flags & SCOUTFS_NET_GREETING_FLAG_QUORUM)
		mcv.flags |= SCOUTFS_MOUNTED_CLIENT_QUORUM;

	mutex_lock(&server->mounted_clients_mutex);
	ret = scoutfs_btree_insert(sb, &server->alloc, &server->wri,
				   &super->mounted_clients, &key, &mcv,
				   sizeof(mcv));
	if (ret == -EEXIST)
		ret = 0;
	mutex_unlock(&server->mounted_clients_mutex);

	return ret;
}

/*
 * Remove the record of a mounted client.  The record can already be
 * removed if we're processing a farewell on behalf of a client that
 * already had a previous server process its farewell.
 *
 * The caller has to serialize with farewell processing.
 */
static int delete_mounted_client(struct super_block *sb, u64 rid)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_key key;
	int ret;

	init_mounted_client_key(&key, rid);

	mutex_lock(&server->mounted_clients_mutex);
	ret = scoutfs_btree_delete(sb, &server->alloc, &server->wri,
				   &super->mounted_clients, &key);
	mutex_unlock(&server->mounted_clients_mutex);
	if (ret == -ENOENT)
		ret = 0;

	return ret;
}

/*
 * Remove all the busy items for srch compactions that the mount might
 * have been responsible for and reclaim all their allocators.  The freed
 * allocator could still contain stable srch file blknos.
 */
static int cancel_srch_compact(struct super_block *sb, u64 rid)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_alloc_list_head av;
	struct scoutfs_alloc_list_head fr;
	int ret;

	for (;;) {
		mutex_lock(&server->srch_mutex);
		ret = scoutfs_srch_cancel_compact(sb, &server->alloc,
						  &server->wri,
						  &super->srch_root, rid,
						  &av, &fr);
		mutex_unlock(&server->srch_mutex);
		if (ret < 0) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}

		mutex_lock(&server->alloc_mutex);
		ret = scoutfs_alloc_splice_list(sb, &server->alloc,
						&server->wri,
						server->other_freed, &av) ?:
		      scoutfs_alloc_splice_list(sb, &server->alloc,
						&server->wri,
						server->other_freed, &fr);
		mutex_unlock(&server->alloc_mutex);
		if (WARN_ON_ONCE(ret < 0))
			break;
	}

	return ret;
}

/*
 * Farewell processing is async to the request processing work.  Shutdown
 * waits for request processing to finish and then tears down the connection.
 * We don't want to queue farewell processing once we start shutting down
 * so that we don't have farewell processing racing with the connecting
 * being shutdown.  If a mount's farewell message is dropped by a server
 * it will be processed by the next server.
 */
static void queue_farewell_work(struct server_info *server)
{
	if (!server->shutting_down)
		queue_work(server->wq, &server->farewell_work);
}

/*
 * Process an incoming greeting request in the server from the client.
 * We try to send responses to failed greetings so that the sender can
 * log some detail before shutting down.  A failure to send a greeting
 * response shuts down the connection.
 *
 * If a client reconnects they'll send their previously received
 * serer_term in their greeting request.
 *
 * XXX The logic of this has gotten convoluted.  The lock server can
 * send a recovery request so it needs to be called after the core net
 * greeting call enables messages.  But we want the greeting reply to be
 * sent first, so we currently queue it on the send queue before
 * enabling messages.  That means that a lot of errors that happen after
 * the reply can't be sent to the client.  They'll just see a disconnect
 * and won't know what's happened.  This all needs to be refactored.
 */
static int server_greeting(struct super_block *sb,
			   struct scoutfs_net_connection *conn,
			   u8 cmd, u64 id, void *arg, u16 arg_len)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_net_greeting *gr = arg;
	struct scoutfs_net_greeting greet;
	DECLARE_SERVER_INFO(sb, server);
	bool reconnecting;
	bool first_contact;
	bool farewell;
	int ret = 0;
	int err;

	if (arg_len != sizeof(struct scoutfs_net_greeting)) {
		ret = -EINVAL;
		goto send_err;
	}

	if (gr->fsid != super->hdr.fsid) {
		scoutfs_warn(sb, "client sent fsid 0x%llx, server has 0x%llx",
			     le64_to_cpu(gr->fsid),
			     le64_to_cpu(super->hdr.fsid));
		ret = -EINVAL;
		goto send_err;
	}

	if (gr->version != super->version) {
		scoutfs_warn(sb, "client sent format 0x%llx, server has 0x%llx",
			     le64_to_cpu(gr->version),
			     le64_to_cpu(super->version));
		ret = -EINVAL;
		goto send_err;
	}

	if (gr->server_term == 0) {
		ret = scoutfs_server_hold_commit(sb);
		if (ret < 0)
			goto send_err;

		ret = insert_mounted_client(sb, le64_to_cpu(gr->rid),
					    le64_to_cpu(gr->flags));

		ret = scoutfs_server_apply_commit(sb, ret);
		queue_work(server->wq, &server->farewell_work);
		if (ret < 0)
			goto send_err;
	}

	scoutfs_server_recov_finish(sb, le64_to_cpu(gr->rid), SCOUTFS_RECOV_GREETING);
	ret = 0;

send_err:
	err = ret;

	greet.fsid = super->hdr.fsid;
	greet.version = super->version;
	greet.server_term = cpu_to_le64(server->term);
	greet.rid = gr->rid;
	greet.flags = 0;

	/* queue greeting response to be sent first once messaging enabled */
	ret = scoutfs_net_response(sb, conn, cmd, id, err,
				   &greet, sizeof(greet));
	if (ret == 0 && err)
		ret = err;
	if (ret)
		goto out;

	/* have the net core enable messaging and resend */
	reconnecting = gr->server_term != 0;
	first_contact = le64_to_cpu(gr->server_term) != server->term;
	if (gr->flags & cpu_to_le64(SCOUTFS_NET_GREETING_FLAG_FAREWELL))
		farewell = true;
	else
		farewell = false;

	scoutfs_net_server_greeting(sb, conn, le64_to_cpu(gr->rid), id,
				    reconnecting, first_contact, farewell);

	/* let layers know we have a client connecting for the first time */
	if (le64_to_cpu(gr->server_term) != server->term) {
		ret = scoutfs_lock_server_greeting(sb, le64_to_cpu(gr->rid)) ?:
		      scoutfs_omap_add_rid(sb, le64_to_cpu(gr->rid));
		if (ret)
			goto out;
	}

out:
	return ret;
}

struct farewell_request {
	struct list_head entry;
	u64 net_id;
	u64 rid;
};

static bool invalid_mounted_client_item(struct scoutfs_btree_item_ref *iref)
{
	return (iref->val_len !=
			sizeof(struct scoutfs_mounted_client_btree_val));
}

/*
 * Reclaim all the resources for a mount which has gone away.  It's sent
 * us a farewell promising to leave or we actively fenced it.
 *
 * It's safe to call this multiple times for a given rid.  Each
 * individual action knows to recognize that it's already been performed
 * and return success.
 */
static int reclaim_rid(struct super_block *sb, u64 rid, bool clear_leader)
{
	int ret;

	ret = scoutfs_server_hold_commit(sb);
	if (ret < 0)
		return ret;

	/* delete mounted client last, recovery looks for it */
	ret = scoutfs_lock_server_farewell(sb, rid) ?:
	      remove_trans_seq(sb, rid) ?:
	      reclaim_log_trees(sb, rid) ?:
	      cancel_srch_compact(sb, rid) ?:
	      scoutfs_omap_remove_rid(sb, rid) ?:
	      (clear_leader ? scoutfs_quorum_clear_rid_leader(sb, rid) : 0) ?:
	      delete_mounted_client(sb, rid);

	return scoutfs_server_apply_commit(sb, ret);
}

/*
 * This work processes farewell requests asynchronously.  Requests from
 * quorum members can be held until only the final majority remains and
 * they've all sent farewell requests.
 *
 * A client can be disconnected before receiving our farewell response.
 * Before reconnecting they check for their mounted client item, if it's
 * been removed then they know that their farewell has been processed
 * and that they finish unmounting without reconnecting.
 *
 * Responses for clients who aren't quorum members are immediately sent.
 * Clients that don't have a mounted client record have already had
 * their farewell processed by another server and can proceed.
 *
 * Farewell responses are unique in that sending them causes the server
 * to shutdown the connection to the client next time the socket
 * disconnects.  If the socket is destroyed before the client gets the
 * response they'll reconnect and we'll see them as a brand new client
 * who immediately sends a farewell.  It'll be processed and it all
 * works out.
 *
 * If this worker sees an error it assumes that this sever is done for
 * and that another had better take its place.
 */
static void farewell_worker(struct work_struct *work)
{
	struct server_info *server = container_of(work, struct server_info,
						  farewell_work);
	struct super_block *sb = server->sb;
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct scoutfs_mounted_client_btree_val *mcv;
	struct farewell_request *tmp;
	struct farewell_request *fw;
	SCOUTFS_BTREE_ITEM_REF(iref);
	unsigned int quo_reqs = 0;
	unsigned int quo_mnts = 0;
	unsigned int non_mnts = 0;
	struct scoutfs_key key;
	LIST_HEAD(reqs);
	LIST_HEAD(send);
	bool more_reqs;
	int ret;

	spin_lock(&server->farewell_lock);
	list_splice_init(&server->farewell_requests, &reqs);
	spin_unlock(&server->farewell_lock);

	/* first count mounted clients who could send requests */
	init_mounted_client_key(&key, 0);
	for (;;) {
		mutex_lock(&server->mounted_clients_mutex);
		ret = scoutfs_btree_next(sb, &super->mounted_clients, &key,
					 &iref);
		mutex_unlock(&server->mounted_clients_mutex);
		if (ret == 0 && invalid_mounted_client_item(&iref)) {
			scoutfs_btree_put_iref(&iref);
			ret = -EIO;
		}
		if (ret != 0) {
			if (ret == -ENOENT)
				break;
			goto out;
		}

		key = *iref.key;
		mcv = iref.val;

		if (mcv->flags & SCOUTFS_MOUNTED_CLIENT_QUORUM)
			quo_mnts++;
		else
			non_mnts++;

		scoutfs_btree_put_iref(&iref);
		scoutfs_key_inc(&key);
	}

	/* walk requests, checking their mounted client items */
	list_for_each_entry_safe(fw, tmp, &reqs, entry) {
		init_mounted_client_key(&key, fw->rid);
		mutex_lock(&server->mounted_clients_mutex);
		ret = scoutfs_btree_lookup(sb, &super->mounted_clients, &key,
					   &iref);
		mutex_unlock(&server->mounted_clients_mutex);
		if (ret == 0 && invalid_mounted_client_item(&iref)) {
			scoutfs_btree_put_iref(&iref);
			ret = -EIO;
		}
		if (ret < 0) {
			/* missing items means we've already processed */
			if (ret == -ENOENT) {
				list_move(&fw->entry, &send);
				continue;
			}
			goto out;
		}

		mcv = iref.val;

		/* count quo reqs, can always send to non-quo clients */
		if (mcv->flags & SCOUTFS_MOUNTED_CLIENT_QUORUM) {
			quo_reqs++;
		} else {
			list_move(&fw->entry, &send);
			non_mnts--;
		}

		scoutfs_btree_put_iref(&iref);
	}

	/*
	 * Only requests from quorum members remain and we've counted
	 * them and remaining mounts.  Send responses as long as enough
	 * quorum clients remain for a majority, or all the requests are
	 * from the final majority of quorum clients they're the only
	 * mounted clients.
	 */
	list_for_each_entry_safe(fw, tmp, &reqs, entry) {
		if ((quo_mnts > scoutfs_quorum_votes_needed(sb)) ||
		    ((quo_reqs == quo_mnts) && (non_mnts == 0))) {
			list_move_tail(&fw->entry, &send);
			quo_mnts--;
			quo_reqs--;
		}
	}

	/* clean up resources for mounts before sending responses */
	list_for_each_entry_safe(fw, tmp, &send, entry) {
		ret = reclaim_rid(sb, fw->rid, false);
		if (ret)
			goto out;
	}

	/* and finally send all the responses */
	list_for_each_entry_safe(fw, tmp, &send, entry) {

		ret = scoutfs_net_response_node(sb, server->conn, fw->rid,
						SCOUTFS_NET_CMD_FAREWELL,
						fw->net_id, 0, NULL, 0);
		if (ret)
			break;

		list_del_init(&fw->entry);
		kfree(fw);
	}

	ret = 0;
out:
	spin_lock(&server->farewell_lock);
	more_reqs = !list_empty(&server->farewell_requests);
	list_splice_init(&reqs, &server->farewell_requests);
	list_splice_init(&send, &server->farewell_requests);
	spin_unlock(&server->farewell_lock);

	if (ret < 0)
		stop_server(server);
	else if (more_reqs)
		queue_farewell_work(server);
}

static void free_farewell_requests(struct super_block *sb, u64 rid)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;
	struct farewell_request *tmp;
	struct farewell_request *fw;
	LIST_HEAD(rid_list);

	spin_lock(&server->farewell_lock);
	list_for_each_entry_safe(fw, tmp, &server->farewell_requests, entry) {
		if (rid == 0 || fw->rid == rid)
			list_move_tail(&fw->entry, &rid_list);
	}
	spin_unlock(&server->farewell_lock);

	list_for_each_entry_safe(fw, tmp, &rid_list, entry)
		kfree(fw);
}

/*
 * The server is receiving a farewell message from a client that is
 * unmounting.  It won't send any more requests and once it receives our
 * response it will not reconnect.
 *
 * XXX we should make sure that all our requests to the client have finished
 * before we respond.  Locking will have its own messaging for orderly
 * shutdown.  That leaves compaction which will be addressed as part of
 * the larger work of recovering compactions that were in flight when
 * a client crashed.
 */
static int server_farewell(struct super_block *sb,
			   struct scoutfs_net_connection *conn,
			   u8 cmd, u64 id, void *arg, u16 arg_len)
{
	struct server_info *server = SCOUTFS_SB(sb)->server_info;
	u64 rid = scoutfs_net_client_rid(conn);
	struct farewell_request *fw;

	if (arg_len != 0)
		return -EINVAL;

	/* XXX tear down if we fence, or if we shut down */

	fw = kmalloc(sizeof(struct farewell_request), GFP_NOFS);
	if (fw == NULL)
		return -ENOMEM;

	fw->rid = rid;
	fw->net_id = id;

	spin_lock(&server->farewell_lock);
	list_add_tail(&fw->entry, &server->farewell_requests);
	spin_unlock(&server->farewell_lock);

	queue_farewell_work(server);

	/* response will be sent later */
	return 0;
}

static scoutfs_net_request_t server_req_funcs[] = {
	[SCOUTFS_NET_CMD_GREETING]		= server_greeting,
	[SCOUTFS_NET_CMD_ALLOC_INODES]		= server_alloc_inodes,
	[SCOUTFS_NET_CMD_GET_LOG_TREES]		= server_get_log_trees,
	[SCOUTFS_NET_CMD_COMMIT_LOG_TREES]	= server_commit_log_trees,
	[SCOUTFS_NET_CMD_GET_ROOTS]		= server_get_roots,
	[SCOUTFS_NET_CMD_ADVANCE_SEQ]		= server_advance_seq,
	[SCOUTFS_NET_CMD_GET_LAST_SEQ]		= server_get_last_seq,
	[SCOUTFS_NET_CMD_LOCK]			= server_lock,
	[SCOUTFS_NET_CMD_SRCH_GET_COMPACT]	= server_srch_get_compact,
	[SCOUTFS_NET_CMD_SRCH_COMMIT_COMPACT]	= server_srch_commit_compact,
	[SCOUTFS_NET_CMD_OPEN_INO_MAP]		= server_open_ino_map,
	[SCOUTFS_NET_CMD_GET_VOLOPT]		= server_get_volopt,
	[SCOUTFS_NET_CMD_SET_VOLOPT]		= server_set_volopt,
	[SCOUTFS_NET_CMD_CLEAR_VOLOPT]		= server_clear_volopt,
	[SCOUTFS_NET_CMD_FAREWELL]		= server_farewell,
};

static void server_notify_up(struct super_block *sb,
			     struct scoutfs_net_connection *conn,
			     void *info, u64 rid)
{
	struct server_client_info *sci = info;
	DECLARE_SERVER_INFO(sb, server);

	if (rid != 0) {
		sci->rid = rid;
		spin_lock(&server->lock);
		list_add_tail(&sci->head, &server->clients);
		server->nr_clients++;
		trace_scoutfs_server_client_up(sb, rid, server->nr_clients);
		spin_unlock(&server->lock);
	}
}

static void server_notify_down(struct super_block *sb,
			       struct scoutfs_net_connection *conn,
			       void *info, u64 rid)
{
	struct server_client_info *sci = info;
	DECLARE_SERVER_INFO(sb, server);

	if (rid != 0) {
		spin_lock(&server->lock);
		list_del_init(&sci->head);
		server->nr_clients--;
		trace_scoutfs_server_client_down(sb, rid,
						 server->nr_clients);
		spin_unlock(&server->lock);

		free_farewell_requests(sb, rid);
	} else {
		stop_server(server);
	}
}

/*
 * All clients have recovered all state.  Now we can kick all the work
 * that was waiting on recovery.
 *
 * It's a bit of a false dependency to have all work wait for completion
 * before any work can make progress, but recovery is naturally
 * concerned about in-memory state.  It should all be quick to recover
 * once a client arrives.
 */
static void finished_recovery(struct super_block *sb)
{
	DECLARE_SERVER_INFO(sb, server);
	int ret = 0;

	scoutfs_info(sb, "all clients recovered");

	ret = scoutfs_omap_finished_recovery(sb) ?:
	      scoutfs_lock_server_finished_recovery(sb);
	if (ret < 0) {
		scoutfs_err(sb, "error %d resuming after recovery finished, shutting down", ret);
		stop_server(server);
	}
}

void scoutfs_server_recov_finish(struct super_block *sb, u64 rid, int which)
{
	if (scoutfs_recov_finish(sb, rid, which) > 0)
		finished_recovery(sb);
}

/*
 * If the recovery timeout is too short we'll prematurely evict mounts
 * that would have recovered.  They need time to have their sockets
 * timeout, reconnect to the current server, and fully recover their
 * state.
 *
 * If it's too long we'll needlessly delay resuming operations after
 * clients crash and will never recover.
 */
#define SERVER_RECOV_TIMEOUT_MS (30 * MSEC_PER_SEC)

/*
 * Not all clients recovered in time.  We fence them and reclaim
 * whatever resources they were using.  If we see a rid here then we're
 * going to fence it, regardless of if it manages to finish recovery
 * while we're fencing it.
 */
static void fence_pending_recov_worker(struct work_struct *work)
{
	struct server_info *server = container_of(work, struct server_info,
						  fence_pending_recov_work);
	struct super_block *sb = server->sb;
	u64 rid = 0;
	int ret;

	while ((rid = scoutfs_recov_next_pending(sb, rid, SCOUTFS_RECOV_ALL)) > 0) {
		scoutfs_err(sb, "%lu ms recovery timeout expired for client rid %016llx, fencing",
			    SERVER_RECOV_TIMEOUT_MS, rid);

		ret = scoutfs_fence_start(sb, rid, 0, SCOUTFS_FENCE_CLIENT_RECOVERY);
		if (ret) {
			scoutfs_err(sb, "fence returned err %d, shutting down server", ret);
			scoutfs_server_abort(sb);
			break;
		}
	}
}

static void recovery_timeout(struct super_block *sb)
{
	DECLARE_SERVER_INFO(sb, server);

	if (!server->shutting_down)
		queue_work(server->wq, &server->fence_pending_recov_work);
}

/*
 * As the server starts up it needs to start waiting for recovery from
 * any clients which were previously still mounted in the last running
 * server.  This is done before networking is started so we won't
 * receive any messages from clients until we've prepared them all.  If
 * the clients don't recover in time then they'll be fenced.
 */
static int start_recovery(struct super_block *sb)
{
	DECLARE_SERVER_INFO(sb, server);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_key key;
	unsigned int nr = 0;
	u64 rid;
	int ret;

	for (rid = 0; ; rid++) {
		init_mounted_client_key(&key, rid);
		ret = scoutfs_btree_next(sb, &super->mounted_clients, &key, &iref);
		if (ret == -ENOENT) {
			ret = 0;
			break;
		}
		if (ret == 0) {
			rid = le64_to_cpu(iref.key->skmc_rid);
			scoutfs_btree_put_iref(&iref);
		}
		if (ret < 0)
			goto out;

		ret = scoutfs_recov_prepare(sb, rid, SCOUTFS_RECOV_ALL);
		if (ret < 0) {
			scoutfs_err(sb, "error %d preparing recovery for client rid %016llx, shutting down",
				     ret, rid);
			goto out;
		}

		nr++;
	}

	if (nr > 0) {
		scoutfs_info(sb, "waiting for %u clients to recover", nr);

		ret = scoutfs_recov_begin(sb, recovery_timeout, SERVER_RECOV_TIMEOUT_MS);
		if (ret > 0) {
			finished_recovery(sb);
			ret = 0;
		}
	}

out:
	if (ret < 0) {
		scoutfs_err(sb, "error %d starting recovery, shutting down", ret);
		stop_server(server);
	}
	return ret;
}

static void queue_reclaim_work(struct server_info *server, unsigned long delay)
{
	if (!server->shutting_down)
		queue_delayed_work(server->wq, &server->reclaim_dwork, delay);
}

#define RECLAIM_WORK_DELAY_MS	MSEC_PER_SEC

/*
 * Fencing is performed by userspace and can happen as we're elected
 * leader before the server is running.  Once we're running we want to
 * reclaim resources from any mounts that may have been fenced.
 *
 * The reclaim worker runs regularly in the background and reclaims the
 * resources for mounts that have been fenced.  Once the fenced rid has
 * been reclaimed the fence request can be removed.
 *
 * This is queued by the server work as it starts up, requeues itself
 * until shutdown, and is then canceled by the server work as it shuts
 * down.
 */
static void reclaim_worker(struct work_struct *work)
{
	struct server_info *server = container_of(work, struct server_info, reclaim_dwork.work);
	struct super_block *sb = server->sb;
	bool error;
	int reason;
	u64 rid;
	int ret;

	ret = scoutfs_fence_next(sb, &rid, &reason, &error);
	if (ret < 0)
		goto out;

	if (error == true) {
		scoutfs_err(sb, "saw error indicator on fence request for rid %016llx, shutting down server",
			    rid);
		scoutfs_server_abort(sb);
		ret = -ESHUTDOWN;
		goto out;
	}

	ret = reclaim_rid(sb, rid, reason == SCOUTFS_FENCE_QUORUM_BLOCK_LEADER);
	if (ret < 0) {
		scoutfs_err(sb, "failure to reclaim fenced rid %016llx: err %d, shutting down server",
			    rid, ret);
		scoutfs_server_abort(sb);
		goto out;
	}

	scoutfs_info(sb, "successfully reclaimed resources for fenced rid %016llx", rid);
	scoutfs_fence_free(sb, rid);
	scoutfs_server_recov_finish(sb, rid, SCOUTFS_RECOV_ALL);
	ret = 0;

out:
	/* queue next reclaim immediately if we're making progress */
	if (ret == 0)
		queue_reclaim_work(server, 0);
	else
		queue_reclaim_work(server, msecs_to_jiffies(RECLAIM_WORK_DELAY_MS));
}

static void scoutfs_server_worker(struct work_struct *work)
{
	struct server_info *server = container_of(work, struct server_info,
						  work);
	struct super_block *sb = server->sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct mount_options *opts = &sbi->opts;
	struct scoutfs_net_connection *conn = NULL;
	DECLARE_WAIT_QUEUE_HEAD(waitq);
	struct sockaddr_in sin;
	u64 max_vers;
	int ret;

	trace_scoutfs_server_work_enter(sb, 0, 0);

	/* first make sure no other servers are still running */
	ret = scoutfs_quorum_fence_leader_blocks(sb, server->term);
	if (ret < 0)
		goto out;

	scoutfs_quorum_slot_sin(super, opts->quorum_slot_nr, &sin);
	scoutfs_info(sb, "server setting up at "SIN_FMT, SIN_ARG(&sin));

	conn = scoutfs_net_alloc_conn(sb, server_notify_up, server_notify_down,
				      sizeof(struct server_client_info),
				      server_req_funcs, "server");
	if (!conn) {
		ret = -ENOMEM;
		goto out;
	}

	ret = scoutfs_net_bind(sb, conn, &sin);
	if (ret) {
		scoutfs_err(sb, "server failed to bind to "SIN_FMT", err %d%s",
			    SIN_ARG(&sin), ret,
			    ret == -EADDRNOTAVAIL ? " (Bad address?)"
						  : "");
		goto out;
	}

	/* start up the server subsystems before accepting */
	ret = scoutfs_read_super(sb, super);
	if (ret < 0)
		goto shutdown;

	/* update volume options early, possibly for use during startup */
	write_seqcount_begin(&server->volopt_seqcount);
	server->volopt = super->volopt;
	write_seqcount_end(&server->volopt_seqcount);

	set_roots(server, &super->fs_root, &super->logs_root,
		  &super->srch_root);
	scoutfs_block_writer_init(sb, &server->wri);

	/* prepare server alloc for this transaction, larger first */
	if (le64_to_cpu(super->server_meta_avail[0].total_nr) <
	    le64_to_cpu(super->server_meta_avail[1].total_nr))
		server->other_ind = 0;
	else
		server->other_ind = 1;
	scoutfs_alloc_init(&server->alloc,
			   &super->server_meta_avail[server->other_ind ^ 1],
			   &super->server_meta_freed[server->other_ind ^ 1]);
	server->other_avail = &super->server_meta_avail[server->other_ind];
	server->other_freed = &super->server_meta_freed[server->other_ind];

	/* use largest meta_alloc to start */
	server->meta_avail = &super->meta_alloc[0];
	server->meta_freed = &super->meta_alloc[1];
	if (le64_to_cpu(server->meta_freed->total_len) >
	    le64_to_cpu(server->meta_avail->total_len))
		swap(server->meta_avail, server->meta_freed);

	ret = scoutfs_forest_get_max_vers(sb, super, &max_vers);
	if (ret) {
		scoutfs_err(sb, "server couldn't find max item vers: %d", ret);
		goto shutdown;
	}

	ret = scoutfs_lock_server_setup(sb, &server->alloc, &server->wri, max_vers) ?:
	      start_recovery(sb);
	if (ret)
		goto shutdown;

	/* start accepting connections and processing work */
	server->conn = conn;
	scoutfs_net_listen(sb, conn);

	scoutfs_info(sb, "server ready at "SIN_FMT, SIN_ARG(&sin));
	complete(&server->start_comp);

	queue_reclaim_work(server, 0);

	/* wait_event/wake_up provide barriers */
	wait_event_interruptible(server->waitq, server->shutting_down);

shutdown:
	scoutfs_info(sb, "server shutting down at "SIN_FMT, SIN_ARG(&sin));

	/* wait for farewell to finish sending messages */
	flush_work(&server->farewell_work);
	cancel_delayed_work_sync(&server->reclaim_dwork);

	/* wait for requests to finish, no more requests */
	scoutfs_net_shutdown(sb, conn);
	server->conn = NULL;

	/* stop tracking recovery, cancel timer, flush any fencing */
	scoutfs_recov_shutdown(sb);
	flush_work(&server->fence_pending_recov_work);

	/* wait for extra queues by requests, won't find waiters */
	flush_work(&server->commit_work);

	scoutfs_fence_stop(sb);
	scoutfs_lock_server_destroy(sb);
	scoutfs_omap_server_shutdown(sb);

out:
	scoutfs_net_free_conn(sb, conn);

	/* let quorum know that we've shutdown */
	scoutfs_quorum_server_shutdown(sb);

	scoutfs_info(sb, "server stopped at "SIN_FMT, SIN_ARG(&sin));
	trace_scoutfs_server_work_exit(sb, 0, ret);

	server->err = ret;
	complete(&server->start_comp);
}

/*
 * Wait for the server to successfully start.  If this returns error then
 * the super block's fence_term has been set to the new server's term so
 * that it won't be fenced.
 */
int scoutfs_server_start(struct super_block *sb, u64 term)
{
	DECLARE_SERVER_INFO(sb, server);

	server->err = 0;
	server->shutting_down = false;
	server->term = term;
	init_completion(&server->start_comp);

	queue_work(server->wq, &server->work);

	wait_for_completion(&server->start_comp);
	return server->err;
}

/*
 * Start shutdown on the server but don't want for it to finish.
 */
void scoutfs_server_abort(struct super_block *sb)
{
	DECLARE_SERVER_INFO(sb, server);

	stop_server(server);
}

/*
 * Once the server is stopped we give the caller our election info
 * which might have been modified while we were running.
 */
void scoutfs_server_stop(struct super_block *sb)
{
	DECLARE_SERVER_INFO(sb, server);

	stop_server(server);

	cancel_work_sync(&server->work);
	cancel_work_sync(&server->farewell_work);
	cancel_work_sync(&server->commit_work);
}

int scoutfs_server_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct server_info *server;

	server = kzalloc(sizeof(struct server_info), GFP_KERNEL);
	if (!server)
		return -ENOMEM;

	server->sb = sb;
	spin_lock_init(&server->lock);
	init_waitqueue_head(&server->waitq);
	INIT_WORK(&server->work, scoutfs_server_worker);
	init_rwsem(&server->commit_rwsem);
	init_llist_head(&server->commit_waiters);
	INIT_WORK(&server->commit_work, scoutfs_server_commit_func);
	init_rwsem(&server->seq_rwsem);
	INIT_LIST_HEAD(&server->clients);
	spin_lock_init(&server->farewell_lock);
	INIT_LIST_HEAD(&server->farewell_requests);
	INIT_WORK(&server->farewell_work, farewell_worker);
	mutex_init(&server->alloc_mutex);
	mutex_init(&server->logs_mutex);
	mutex_init(&server->srch_mutex);
	mutex_init(&server->mounted_clients_mutex);
	seqcount_init(&server->roots_seqcount);
	seqcount_init(&server->volopt_seqcount);
	mutex_init(&server->volopt_mutex);
	INIT_WORK(&server->fence_pending_recov_work, fence_pending_recov_worker);
	INIT_DELAYED_WORK(&server->reclaim_dwork, reclaim_worker);

	server->wq = alloc_workqueue("scoutfs_server",
				     WQ_UNBOUND | WQ_NON_REENTRANT, 0);
	if (!server->wq) {
		kfree(server);
		return -ENOMEM;
	}

	sbi->server_info = server;
	return 0;
}

/*
 * The caller should have already stopped but we do the same just in
 * case.
 */
void scoutfs_server_destroy(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct server_info *server = sbi->server_info;

	if (server) {
		stop_server(server);

		/* wait for server work to wait for everything to shut down */
		cancel_work_sync(&server->work);
		/* farewell work triggers commits */
		cancel_work_sync(&server->farewell_work);
		/* recv work/compaction could have left commit_work queued */
		cancel_work_sync(&server->commit_work);

		/* pending farewell requests are another server's problem */
		free_farewell_requests(sb, 0);

		trace_scoutfs_server_workqueue_destroy(sb, 0, 0);
		destroy_workqueue(server->wq);

		kfree(server);
		sbi->server_info = NULL;
	}
}
