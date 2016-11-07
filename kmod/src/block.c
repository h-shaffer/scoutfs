/*
 * Copyright (C) 2016 Versity Software, Inc.  All rights reserved.
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
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/slab.h>

#include "super.h"
#include "format.h"
#include "block.h"
#include "crc.h"
#include "counters.h"
#include "buddy.h"

/*
 * scoutfs has a fixed 4k small block size for metadata blocks.  This
 * lets us consistently use buffer heads without worrying about having a
 * block size greater than the page size.
 *
 * This block interface does the work to cow dirty blocks, track dirty
 * blocks, generate checksums as they're written, only write them in
 * transactions, verify checksums on read, and invalidate and retry
 * reads of stale cached blocks.  (That last bit only has a hint of an
 * implementation.)
 *
 * XXX
 *  - tear down dirty blocks left by write errors on unmount
 *  - should invalidate dirty blocks if freed
 */

struct scoutfs_block;

struct block_bh_private {
	struct super_block *sb;
	struct buffer_head *bh;
	struct rb_node node;
	struct rw_semaphore rwsem;
	bool rwsem_class;
};

enum {
	BH_ScoutfsVerified = BH_PrivateStart,
};
BUFFER_FNS(ScoutfsVerified, scoutfs_verified)

static int verify_block_header(struct scoutfs_sb_info *sbi,
			       struct buffer_head *bh)
{
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_block_header *hdr = (void *)bh->b_data;
	u32 crc = scoutfs_crc_block(hdr);
	int ret = -EIO;

	if (le32_to_cpu(hdr->crc) != crc) {
		printk("blkno %llu hdr crc %x != calculated %x\n",
		       (u64)bh->b_blocknr, le32_to_cpu(hdr->crc), crc);
	} else if (super->hdr.fsid && hdr->fsid != super->hdr.fsid) {
		printk("blkno %llu fsid %llx != super fsid %llx\n",
		       (u64)bh->b_blocknr, le64_to_cpu(hdr->fsid),
		       le64_to_cpu(super->hdr.fsid));
	} else if (le64_to_cpu(hdr->blkno) != bh->b_blocknr) {
		printk("blkno %llu invalid hdr blkno %llx\n",
		       (u64)bh->b_blocknr, le64_to_cpu(hdr->blkno));
	} else {
		ret = 0;
	}

	return ret;
}

static struct buffer_head *bh_from_bhp_node(struct rb_node *node)
{
	struct block_bh_private *bhp;

	bhp = container_of(node, struct block_bh_private, node);
	return bhp->bh;
}

static struct scoutfs_sb_info *sbi_from_bh(struct buffer_head *bh)
{
	struct block_bh_private *bhp = bh->b_private;

	return SCOUTFS_SB(bhp->sb);
}

static void insert_bhp_rb(struct rb_root *root, struct buffer_head *ins)
{
	struct rb_node **node = &root->rb_node;
	struct rb_node *parent = NULL;
	struct block_bh_private *bhp;
	struct buffer_head *bh;

	while (*node) {
		parent = *node;
		bh = bh_from_bhp_node(*node);

		if (ins->b_blocknr < bh->b_blocknr)
			node = &(*node)->rb_left;
		else
			node = &(*node)->rb_right;
	}

	bhp = ins->b_private;
	rb_link_node(&bhp->node, parent, node);
	rb_insert_color(&bhp->node, root);
}

/*
 * Track a dirty block by allocating private data and inserting it into
 * the dirty rbtree in the super block.
 *
 * Callers are in transactions that prevent metadata writeback so blocks
 * won't be written and cleaned while we're trying to dirty them.  We
 * serialize racing to add dirty tracking to the same block in case the
 * caller didn't.
 *
 * Presence in the dirty tree holds a bh ref.
 */
static int insert_bhp(struct super_block *sb, struct buffer_head *bh)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct block_bh_private *bhp;
	unsigned long flags;
	int ret = 0;

	if (bh->b_private)
		return 0;

	lock_buffer(bh);
	if (bh->b_private)
		goto out;

	bhp = kmalloc(sizeof(*bhp), GFP_NOFS);
	if (!bhp) {
		ret = -ENOMEM;
		goto out;
	}

	bhp->sb = sb;
	bhp->bh = bh;
	get_bh(bh);
	bh->b_private = bhp;
	/* lockdep class can be set by callers that use the lock */
	init_rwsem(&bhp->rwsem);
	bhp->rwsem_class = false;

	spin_lock_irqsave(&sbi->block_lock, flags);
	insert_bhp_rb(&sbi->block_dirty_tree, bh);
	spin_unlock_irqrestore(&sbi->block_lock, flags);

	trace_printk("blkno %llu bh %p\n", (u64)bh->b_blocknr, bh);
out:
	unlock_buffer(bh);
	return ret;
}

static void erase_bhp(struct buffer_head *bh)
{
	struct block_bh_private *bhp = bh->b_private;
	struct scoutfs_sb_info *sbi = sbi_from_bh(bh);
	unsigned long flags;

	spin_lock_irqsave(&sbi->block_lock, flags);
	rb_erase(&bhp->node, &sbi->block_dirty_tree);
	spin_unlock_irqrestore(&sbi->block_lock, flags);

	put_bh(bh);
	kfree(bhp);
	bh->b_private = NULL;

	trace_printk("blkno %llu bh %p\n", (u64)bh->b_blocknr, bh);
}

/*
 * Read an existing block from the device and verify its metadata header.
 * The buffer head is returned unlocked and uptodate.
 */
struct scoutfs_block *scoutfs_block_read(struct super_block *sb, u64 blkno)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct buffer_head *bh;
	int ret;

	bh = sb_bread(sb, blkno);
	if (!bh) {
		bh = ERR_PTR(-EIO);
		goto out;
	}

	if (!buffer_scoutfs_verified(bh)) {
		lock_buffer(bh);
		if (!buffer_scoutfs_verified(bh)) {
			ret = verify_block_header(sbi, bh);
			if (!ret)
				set_buffer_scoutfs_verified(bh);
		} else {
			ret = 0;
		}
		unlock_buffer(bh);
		if (ret < 0) {
			scoutfs_block_put((void *)bh);
			bh = ERR_PTR(ret);
		}
	}

out:
	return (void *)bh;
}

/*
 * Read an existing block from the device described by the caller's
 * reference.
 *
 * If the reference sequence numbers don't match then we could be racing
 * with another writer. We back off and try again.  If it happens too
 * many times the caller assumes that we've hit persistent corruption
 * and returns an error.
 *
 * XXX how does this race with
 *  - reads that span transactions?
 *  - writers creating a new dirty block?
 */
struct scoutfs_block *scoutfs_block_read_ref(struct super_block *sb,
					     struct scoutfs_block_ref *ref)
{
	struct scoutfs_block_header *hdr;
	struct scoutfs_block *bl;

	bl = scoutfs_block_read(sb, le64_to_cpu(ref->blkno));
	if (!IS_ERR(bl)) {
		hdr = scoutfs_block_data(bl);
		if (WARN_ON_ONCE(hdr->seq != ref->seq)) {
			clear_buffer_uptodate(bl);
			scoutfs_block_put(bl);
			bl = ERR_PTR(-EAGAIN);
		}
	}

	return bl;
}

/*
 * We stop tracking dirty metadata blocks when their IO succeeds.  This
 * happens in the context of transaction commit which excludes other
 * metadata dirtying paths.
 */
static void block_write_end_io(struct buffer_head *bh, int uptodate)
{
	struct scoutfs_sb_info *sbi = sbi_from_bh(bh);

	trace_printk("bh %p uptdate %d\n", bh, uptodate);

	/* XXX */
	unlock_buffer(bh);

	if (uptodate) {
		erase_bhp(bh);
	} else {
		/* don't care if this is racey? */
		if (!sbi->block_write_err)
			sbi->block_write_err = -EIO;
	}

	if (atomic_dec_and_test(&sbi->block_writes))
		wake_up(&sbi->block_wq);
}

/*
 * Submit writes for all the buffer heads in the dirty block tree.  The
 * write transaction machinery ensures that the dirty blocks form a
 * consistent image and excludes future dirtying while we're working.
 *
 * Presence in the dirty tree holds a reference.  Blocks are only
 * removed from the tree which drops the ref when IO completes.
 *
 * Blocks that see write errors remain in the dirty tree and will try to
 * be written again in the next transaction commit.
 *
 * Reads can traverse the blocks while they're in flight.
 */
int scoutfs_block_write_dirty(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct buffer_head *bh;
	struct rb_node *node;
	struct blk_plug plug;
	unsigned long flags;
	int ret;

	atomic_set(&sbi->block_writes, 1);
	sbi->block_write_err = 0;
	ret = 0;

	blk_start_plug(&plug);

	spin_lock_irqsave(&sbi->block_lock, flags);
	node = rb_first(&sbi->block_dirty_tree);
	while(node) {
		bh = bh_from_bhp_node(node);
		node = rb_next(node);
		spin_unlock_irqrestore(&sbi->block_lock, flags);

		atomic_inc(&sbi->block_writes);
		scoutfs_block_set_crc((void *)bh);

		lock_buffer(bh);

		bh->b_end_io = block_write_end_io;
		ret = submit_bh(WRITE, bh); /* doesn't actually fail? */

		spin_lock_irqsave(&sbi->block_lock, flags);
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&sbi->block_lock, flags);

	blk_finish_plug(&plug);

	/* wait for all io to drain */
	atomic_dec(&sbi->block_writes);
	wait_event(sbi->block_wq, atomic_read(&sbi->block_writes) == 0);

	trace_printk("ret %d\n", ret);
	return ret;
}

/*
 * The caller knows that it's not racing with writers.
 */
int scoutfs_block_has_dirty(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);

	return !RB_EMPTY_ROOT(&sbi->block_dirty_tree);
}

/*
 * Give the caller a dirty block that they can safely modify.  If the
 * reference refers to a stable clean block then we allocate a new block
 * and update the reference.
 *
 * Blocks are dirtied and modified within a transaction that has a given
 * sequence number which we use to determine if the block is currently
 * dirty or not.
 *
 * For now we're using the dirty super block in the sb_info to track the
 * dirty seq.  That'll be different when we have multiple btrees.
 *
 * Callers are responsible for serializing modification to the reference
 * which is probably embedded in some other dirty persistent structure.
 */
struct scoutfs_block *scoutfs_block_dirty_ref(struct super_block *sb,
					      struct scoutfs_block_ref *ref)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_block_header *hdr;
	struct scoutfs_block *copy_bl = NULL;
	struct scoutfs_block *bl;
	u64 blkno = 0;
	int ret;
	int err;

	bl = scoutfs_block_read(sb, le64_to_cpu(ref->blkno));
	if (IS_ERR(bl) || ref->seq == sbi->super.hdr.seq)
		return bl;

	ret = scoutfs_buddy_alloc_same(sb, &blkno, le64_to_cpu(ref->blkno));
	if (ret < 0)
		goto out;

	copy_bl = scoutfs_block_dirty(sb, blkno);
	if (IS_ERR(copy_bl)) {
		ret = PTR_ERR(copy_bl);
		goto out;
	}

	hdr = scoutfs_block_data(bl);
	ret = scoutfs_buddy_free(sb, hdr->seq, le64_to_cpu(hdr->blkno), 0);
	if (ret)
		goto out;

	memcpy(scoutfs_block_data(copy_bl), scoutfs_block_data(bl),
	       SCOUTFS_BLOCK_SIZE);

	hdr = scoutfs_block_data(copy_bl);
	hdr->blkno = cpu_to_le64(blkno);
	hdr->seq = sbi->super.hdr.seq;
	ref->blkno = hdr->blkno;
	ref->seq = hdr->seq;

	ret = 0;
out:
	scoutfs_block_put(bl);
	if (ret) {
		if (!IS_ERR_OR_NULL(copy_bl)) {
			err = scoutfs_buddy_free(sb, sbi->super.hdr.seq,
						 blkno, 0);
			WARN_ON_ONCE(err); /* freeing dirty must work */
		}
		scoutfs_block_put(copy_bl);
		copy_bl = ERR_PTR(ret);
	}

	return copy_bl;
}

/*
 * Return a dirty metadata block with an updated block header to match
 * the current dirty seq.  Callers are responsible for serializing
 * access to the block and for zeroing unwritten block contents.
 */
struct scoutfs_block *scoutfs_block_dirty(struct super_block *sb, u64 blkno)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_block_header *hdr;
	struct buffer_head *bh;
	int ret;

	/* allocate a new block and try to insert it */
	bh = sb_getblk(sb, blkno);
	if (!bh) {
		bh = ERR_PTR(-ENOMEM);
		goto out;
	}

	ret = insert_bhp(sb, bh);
	if (ret < 0) {
		scoutfs_block_put((void *)bh);
		bh = ERR_PTR(ret);
		goto out;
	}

	hdr = scoutfs_block_data((void *)bh);
	*hdr = sbi->super.hdr;
	hdr->blkno = cpu_to_le64(blkno);
	hdr->seq = sbi->super.hdr.seq;

	set_buffer_uptodate(bh);
	set_buffer_scoutfs_verified(bh);
out:
	return (void *)bh;
}

/*
 * Allocate a new dirty writable block.  The caller must be in a
 * transaction so that we can assign the dirty seq.
 */
struct scoutfs_block *scoutfs_block_dirty_alloc(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->stable_super;
	struct scoutfs_block *bl;
	u64 blkno;
	int ret;
	int err;

	ret = scoutfs_buddy_alloc(sb, &blkno, 0);
	if (ret < 0)
		return ERR_PTR(ret);

	bl = scoutfs_block_dirty(sb, blkno);
	if (IS_ERR(bl)) {
		err = scoutfs_buddy_free(sb, super->hdr.seq, blkno, 0);
		WARN_ON_ONCE(err); /* freeing dirty must work */
	}
	return bl;
}

/*
 * Make sure that we don't have a dirty block at the given blkno.  If we
 * do we remove it from our tree of dirty blocks and clear the buffer
 * dirty bit.
 *
 * XXX for now callers have only needed to forget blknos, maybe they'll
 * have the bh some day.
 */
void scoutfs_block_forget(struct super_block *sb, u64 blkno)
{
	struct block_bh_private *bhp;
	struct buffer_head *bh;

	bh = sb_find_get_block(sb, blkno);
	if (bh) {
		bhp = bh->b_private;
		if (bhp) {
			erase_bhp(bh);
			bforget(bh);
		}
	}
}

void scoutfs_block_set_crc(struct scoutfs_block *bl)
{
	struct scoutfs_block_header *hdr = scoutfs_block_data(bl);

	hdr->crc = cpu_to_le32(scoutfs_crc_block(hdr));
}

/*
 * Zero the block from the given byte to the end of the block.
 */
void scoutfs_block_zero(struct scoutfs_block *bl, size_t off)
{
	if (WARN_ON_ONCE(off > SCOUTFS_BLOCK_SIZE))
		return;

	if (off < SCOUTFS_BLOCK_SIZE)
		memset(scoutfs_block_data(bl) + off, 0,
		       SCOUTFS_BLOCK_SIZE - off);
}

/*
 * Zero the block from the given byte to the end of the block.
 */
void scoutfs_block_zero_from(struct scoutfs_block *bl, void *ptr)
{
	return scoutfs_block_zero(bl, (char *)ptr -
				  (char *)scoutfs_block_data(bl));
}

void scoutfs_block_set_lock_class(struct scoutfs_block *bl,
			          struct lock_class_key *class)
{
	struct buffer_head *bh = (void *)bl;
	struct block_bh_private *bhp = bh->b_private;

	if (bhp && !bhp->rwsem_class) {
		lockdep_set_class(&bhp->rwsem, class);
		bhp->rwsem_class = true;
	}
}

void scoutfs_block_lock(struct scoutfs_block *bl, bool write, int subclass)
{
	struct buffer_head *bh = (void *)bl;
	struct block_bh_private *bhp = bh->b_private;

	if (bhp) {
		if (write)
			down_write_nested(&bhp->rwsem, subclass);
		else
			down_read_nested(&bhp->rwsem, subclass);
	}
}

void scoutfs_block_unlock(struct scoutfs_block *bl, bool write)
{
	struct buffer_head *bh = (void *)bl;
	struct block_bh_private *bhp = bh->b_private;

	if (bhp) {
		if (write)
			up_write(&bhp->rwsem);
		else
			up_read(&bhp->rwsem);
	}
}

void *scoutfs_block_data(struct scoutfs_block *bl)
{
	struct buffer_head *bh = (void *)bl;

	return (void *)bh->b_data;
}

void scoutfs_block_put(struct scoutfs_block *bl)
{
	struct buffer_head *bh = (void *)bl;

	if (!IS_ERR_OR_NULL(bh))
		brelse(bh);
}
