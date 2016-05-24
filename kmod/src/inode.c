/*
 * Copyright (C) 2015 Versity Software, Inc.  All rights reserved.
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
#include <linux/random.h>

#include "format.h"
#include "super.h"
#include "key.h"
#include "inode.h"
#include "btree.h"
#include "dir.h"
#include "filerw.h"
#include "wrlock.h"
#include "scoutfs_trace.h"

/*
 * XXX
 *  - worry about i_ino trunctation, not sure if we do anything
 *  - use inode item value lengths for forward/back compat
 */

static struct kmem_cache *scoutfs_inode_cachep;

static void scoutfs_inode_ctor(void *obj)
{
	struct scoutfs_inode_info *ci = obj;

	inode_init_once(&ci->inode);
}

struct inode *scoutfs_alloc_inode(struct super_block *sb)
{
	struct scoutfs_inode_info *ci;

	ci = kmem_cache_alloc(scoutfs_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	return &ci->inode;
}

static void scoutfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	trace_printk("freeing inode %p\n", inode);
	kmem_cache_free(scoutfs_inode_cachep, SCOUTFS_I(inode));
}

void scoutfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, scoutfs_i_callback);
}

/*
 * Called once new inode allocation or inode reading has initialized
 * enough of the inode for us to set the ops based on the mode.
 */
static void set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_mapping->a_ops = &scoutfs_file_aops;
//		inode->i_op = &scoutfs_file_iops;
		inode->i_fop = &scoutfs_file_fops;
		break;
	case S_IFDIR:
		inode->i_op = &scoutfs_dir_iops;
		inode->i_fop = &scoutfs_dir_fops;
		break;
	case S_IFLNK:
//		inode->i_op = &scoutfs_symlink_iops;
		break;
	default:
//		inode->i_op = &scoutfs_special_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}
}

static void load_inode(struct inode *inode, struct scoutfs_inode *cinode)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);

	i_size_write(inode, le64_to_cpu(cinode->size));
	set_nlink(inode, le32_to_cpu(cinode->nlink));
	i_uid_write(inode, le32_to_cpu(cinode->uid));
	i_gid_write(inode, le32_to_cpu(cinode->gid));
	inode->i_mode = le32_to_cpu(cinode->mode);
	inode->i_rdev = le32_to_cpu(cinode->rdev);
	inode->i_atime.tv_sec = le64_to_cpu(cinode->atime.sec);
	inode->i_atime.tv_nsec = le32_to_cpu(cinode->atime.nsec);
	inode->i_mtime.tv_sec = le64_to_cpu(cinode->mtime.sec);
	inode->i_mtime.tv_nsec = le32_to_cpu(cinode->mtime.nsec);
	inode->i_ctime.tv_sec = le64_to_cpu(cinode->ctime.sec);
	inode->i_ctime.tv_nsec = le32_to_cpu(cinode->ctime.nsec);
	
	ci->salt = le32_to_cpu(cinode->salt);
}

static int scoutfs_read_locked_inode(struct inode *inode)
{
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_key key;
	int ret;

	scoutfs_set_key(&key, scoutfs_ino(inode), SCOUTFS_INODE_KEY, 0);

	ret = scoutfs_btree_lookup(sb, &key, &curs);
	if (!ret) {
		load_inode(inode, curs.val);
		scoutfs_btree_release(&curs);
	}

	return 0;
}

static int scoutfs_iget_test(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 *ino = arg;

	return ci->ino == *ino;
}

static int scoutfs_iget_set(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 *ino = arg;

	inode->i_ino = *ino;
	ci->ino = *ino;

	return 0;
}

struct inode *scoutfs_iget(struct super_block *sb, u64 ino)
{
	struct inode *inode;
	int ret;

	inode = iget5_locked(sb, ino, scoutfs_iget_test, scoutfs_iget_set,
			     &ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (inode->i_state & I_NEW) {
		ret = scoutfs_read_locked_inode(inode);
		if (ret) {
			iget_failed(inode);
			inode = ERR_PTR(ret);
		} else {
			set_inode_ops(inode);
			unlock_new_inode(inode);
		}
	}

	return inode;
}

static void store_inode(struct scoutfs_inode *cinode, struct inode *inode)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);

	cinode->size = cpu_to_le64(i_size_read(inode));
	cinode->nlink = cpu_to_le32(inode->i_nlink);
	cinode->uid = cpu_to_le32(i_uid_read(inode));
	cinode->gid = cpu_to_le32(i_gid_read(inode));
	cinode->mode = cpu_to_le32(inode->i_mode);
	cinode->rdev = cpu_to_le32(inode->i_rdev);
	cinode->atime.sec = cpu_to_le64(inode->i_atime.tv_sec);
	cinode->atime.nsec = cpu_to_le32(inode->i_atime.tv_nsec);
	cinode->ctime.sec = cpu_to_le64(inode->i_ctime.tv_sec);
	cinode->ctime.nsec = cpu_to_le32(inode->i_ctime.tv_nsec);
	cinode->mtime.sec = cpu_to_le64(inode->i_mtime.tv_sec);
	cinode->mtime.nsec = cpu_to_le32(inode->i_mtime.tv_nsec);

	cinode->salt = cpu_to_le32(ci->salt);
}

/*
 * Create a pinned dirty inode item so that we can later update the
 * inode item without risking failure.  We often wouldn't want to have
 * to unwind inode modifcations (perhaps by shared vfs code!) if our
 * item update failed.  This is our chance to return errors for enospc
 * for lack of space for new logged dirty inode items.
 *
 * This dirty inode item will be found by lookups in the interim so we
 * have to update it now with the current inode contents.
 *
 * Callers don't delete these dirty items on errors.  They're still
 * valid and will be merged with the current item eventually.  They can
 * be found in the dirty block to avoid future dirtying (say repeated
 * creations in a directory).
 *
 * The caller has to prevent sync between dirtying and updating the
 * inodes.
 *
 * XXX this will have to do something about variable length inodes
 */
int scoutfs_dirty_inode_item(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct scoutfs_key key;
	int ret;

	scoutfs_set_key(&key, scoutfs_ino(inode), SCOUTFS_INODE_KEY, 0);

	ret = scoutfs_btree_dirty(sb, &key);
	if (!ret)
		trace_scoutfs_dirty_inode(inode);
	return ret;
}

/*
 * Every time we modify the inode in memory we copy it to its inode
 * item.  This lets us write out blocks of items without having to track
 * down dirty vfs inodes and safely copy them into items before writing.
 *
 * The caller makes sure that the item is dirty and pinned so they don't
 * have to deal with errors and unwinding after they've modified the
 * vfs inode and get here.
 */
void scoutfs_update_inode_item(struct inode *inode)
{
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_key key;

	scoutfs_set_key(&key, scoutfs_ino(inode), SCOUTFS_INODE_KEY, 0);

	scoutfs_btree_update(sb, &key, &curs);
	store_inode(curs.val, inode);
	scoutfs_btree_release(&curs);
	trace_scoutfs_update_inode(inode);
}

/*
 * This will need to try and find a mostly idle shard.  For now we only
 * have one :).
 */
static int get_next_ino_batch(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	DECLARE_SCOUTFS_WRLOCK_HELD(held);
	int ret;

	ret = scoutfs_wrlock_lock(sb, &held, 1, 1);
	if (ret)
		return ret;

	spin_lock(&sbi->next_ino_lock);
	if (!sbi->next_ino_count) {
		sbi->next_ino = le64_to_cpu(sbi->super.next_ino);
		if (sbi->next_ino + SCOUTFS_INO_BATCH < sbi->next_ino) {
			ret = -ENOSPC;
		} else {
			le64_add_cpu(&sbi->super.next_ino, SCOUTFS_INO_BATCH);
			sbi->next_ino_count = SCOUTFS_INO_BATCH;
			ret = 0;
		}
	}
	spin_unlock(&sbi->next_ino_lock);

	scoutfs_wrlock_unlock(sb, &held);

	return ret;
}

/*
 * Inode allocation is at the core of supporting parallel creation.
 * Each mount needs to allocate from a pool of free inode numbers which
 * map to a shard that it has locked.
 */
int scoutfs_alloc_ino(struct super_block *sb, u64 *ino)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	int ret;

	do {
		/* don't really care if this is racey */
		if (!sbi->next_ino_count) {
			ret = get_next_ino_batch(sb);
			if (ret)
				break;
		}

		spin_lock(&sbi->next_ino_lock);

		if (sbi->next_ino_count) {
			*ino = sbi->next_ino++;
			sbi->next_ino_count--;
			ret = 0;
		} else {
			ret = -EAGAIN;
		}
		spin_unlock(&sbi->next_ino_lock);

	} while (ret == -EAGAIN);

	return ret;
}

/*
 * Allocate and initialize a new inode.  The caller is responsible for
 * creating links to it and updating it.  @dir can be null.
 */
struct inode *scoutfs_new_inode(struct super_block *sb, struct inode *dir,
				u64 ino, umode_t mode, dev_t rdev)
{
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	struct scoutfs_inode_info *ci;
	struct scoutfs_key key;
	struct inode *inode;
	int ret;


	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ci = SCOUTFS_I(inode);
	ci->ino = ino;
	get_random_bytes(&ci->salt, sizeof(ci->salt));

	inode->i_ino = ino; /* XXX overflow */
	inode_init_owner(inode, dir, mode);
	inode_set_bytes(inode, 0);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_rdev = rdev;
	set_inode_ops(inode);

	scoutfs_set_key(&key, scoutfs_ino(inode), SCOUTFS_INODE_KEY, 0);

	ret = scoutfs_btree_insert(inode->i_sb, &key,
				   sizeof(struct scoutfs_inode), &curs);
	if (ret) {
		iput(inode);
		return ERR_PTR(ret);
	}

	scoutfs_btree_release(&curs);
	return inode;
}

void scoutfs_inode_exit(void)
{
	if (scoutfs_inode_cachep) {
		rcu_barrier();
		kmem_cache_destroy(scoutfs_inode_cachep);
		scoutfs_inode_cachep = NULL;
	}
}

int scoutfs_inode_init(void)
{
	scoutfs_inode_cachep = kmem_cache_create("scoutfs_inode_info",
					sizeof(struct scoutfs_inode_info), 0,
					SLAB_RECLAIM_ACCOUNT,
					scoutfs_inode_ctor);
	if (!scoutfs_inode_cachep)
		return -ENOMEM;

	return 0;
}
