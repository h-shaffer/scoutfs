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
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/uio.h>

#include "format.h"
#include "dir.h"
#include "inode.h"
#include "key.h"
#include "super.h"
#include "btree.h"
#include "wrlock.h"

/*
 * Directory entries are stored in entries with offsets calculated from
 * the hash of their entry name.
 *
 * Having a single index of items used for both lookup and readdir
 * iteration reduces the storage overhead of directories.  It also
 * avoids having to manage the allocation of readdir positions as
 * directories age and the aggregate create count inches towards the
 * small 31 bit position limit.  The downside is that dirent name
 * operations produce random item access patterns.
 *
 * Hash values are limited to 31 bits primarily to support older
 * deployed protocols that only support 31 bits of file entry offsets,
 * but also to avoid unlikely bugs in programs that store offsets in
 * signed ints.
 *
 * We have to worry about hash collisions.  We linearly probe a fixed
 * number of hash values past the natural value.  In a typical small
 * directory this search will terminate immediately because adjacent
 * items will have distant offset values.  It's only as the directory
 * gets very large that hash values will start to be this dense and
 * sweeping over items in a btree leaf is reasonably efficient.
 */

static unsigned int mode_to_type(umode_t mode)
{
#define S_SHIFT 12
	static unsigned char mode_types[S_IFMT >> S_SHIFT] = {
		[S_IFIFO >> S_SHIFT]	= SCOUTFS_DT_FIFO,
		[S_IFCHR >> S_SHIFT]	= SCOUTFS_DT_CHR,
		[S_IFDIR >> S_SHIFT]	= SCOUTFS_DT_DIR,
		[S_IFBLK >> S_SHIFT]	= SCOUTFS_DT_BLK,
		[S_IFREG >> S_SHIFT]	= SCOUTFS_DT_REG,
		[S_IFLNK >> S_SHIFT]	= SCOUTFS_DT_LNK,
		[S_IFSOCK >> S_SHIFT]	= SCOUTFS_DT_SOCK,
	};

	return mode_types[(mode & S_IFMT) >> S_SHIFT];
#undef S_SHIFT
}

static unsigned int dentry_type(unsigned int type)
{
	static unsigned char types[] = {
		[SCOUTFS_DT_FIFO]	= DT_FIFO,
		[SCOUTFS_DT_CHR]	= DT_CHR,
		[SCOUTFS_DT_DIR]	= DT_DIR,
		[SCOUTFS_DT_BLK]	= DT_BLK,
		[SCOUTFS_DT_REG]	= DT_REG,
		[SCOUTFS_DT_LNK]	= DT_LNK,
		[SCOUTFS_DT_SOCK]	= DT_SOCK,
		[SCOUTFS_DT_WHT]	= DT_WHT,
	};

	if (type < ARRAY_SIZE(types))
		return types[type];

	return DT_UNKNOWN;
}

static int names_equal(const char *name_a, int len_a, const char *name_b,
		       int len_b)
{
	return (len_a == len_b) && !memcmp(name_a, name_b, len_a);
}

/*
 * XXX This crc nonsense is a quick hack.  We'll want something a
 * lot stronger like siphash.
 */
static u32 name_hash(const char *name, unsigned int len, u32 salt)
{
	u32 h = crc32c(salt, name, len) & SCOUTFS_DIRENT_OFF_MASK;

	return max_t(u32, 2, min_t(u32, h, SCOUTFS_DIRENT_LAST_POS));
}

static unsigned int dent_bytes(unsigned int name_len)
{
	return sizeof(struct scoutfs_dirent) + name_len;
}

static unsigned int item_name_len(struct scoutfs_btree_cursor *curs)
{
	return curs->val_len - sizeof(struct scoutfs_dirent);
}
/*
 * Store the dirent item hash in the dentry so that we don't have to
 * calculate and search to remove the item. 
 */
struct dentry_info {
	u32 hash;
};

static struct kmem_cache *scoutfs_dentry_cachep;

static void scoutfs_d_release(struct dentry *dentry)
{
	struct dentry_info *di = dentry->d_fsdata;

	if (di) {
		kmem_cache_free(scoutfs_dentry_cachep, di);
		dentry->d_fsdata = NULL;
	}
}

static const struct dentry_operations scoutfs_dentry_ops = {
	.d_release = scoutfs_d_release,
};

static struct dentry_info *alloc_dentry_info(struct dentry *dentry)
{
	struct dentry_info *di;

	/* XXX read mb? */
	if (dentry->d_fsdata)
		return dentry->d_fsdata;

	di = kmem_cache_zalloc(scoutfs_dentry_cachep, GFP_NOFS);
	if (!di)
		return ERR_PTR(-ENOMEM);

	spin_lock(&dentry->d_lock);
	if (!dentry->d_fsdata) {
		dentry->d_fsdata = di;
		d_set_d_op(dentry, &scoutfs_dentry_ops);
	}

	spin_unlock(&dentry->d_lock);

	if (di != dentry->d_fsdata)
		kmem_cache_free(scoutfs_dentry_cachep, di);

	return dentry->d_fsdata;
}

static u64 last_dirent_key_offset(u32 h)
{
	return min_t(u64, (u64)h + SCOUTFS_DIRENT_COLL_NR - 1,
			  SCOUTFS_DIRENT_LAST_POS);
}

/*
 * Lookup searches for an entry for the given name amongst the entries
 * stored in the item at the name's hash. 
 */
static struct dentry *scoutfs_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(dir);
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	struct super_block *sb = dir->i_sb;
	struct scoutfs_dirent *dent;
	struct dentry_info *di;
	struct scoutfs_key first;
	struct scoutfs_key last;
	unsigned int name_len;
	struct inode *inode;
	u64 ino = 0;
	u32 h = 0;
	int ret;

	di = alloc_dentry_info(dentry);
	if (IS_ERR(di)) {
		ret = PTR_ERR(di);
		goto out;
	}

	if (dentry->d_name.len > SCOUTFS_NAME_LEN) {
		ret = -ENAMETOOLONG;
		goto out;
	}

	h = name_hash(dentry->d_name.name, dentry->d_name.len, si->salt);

	scoutfs_set_key(&first, scoutfs_ino(dir), SCOUTFS_DIRENT_KEY, h);
	scoutfs_set_key(&last, scoutfs_ino(dir), SCOUTFS_DIRENT_KEY,
			last_dirent_key_offset(h));

	while ((ret = scoutfs_btree_next(sb, &first, &last, &curs)) > 0) {

		/* XXX verify */

		dent = curs.val;
		name_len = item_name_len(&curs);
		if (names_equal(dentry->d_name.name, dentry->d_name.len,
				dent->name, name_len)) {
			ino = le64_to_cpu(dent->ino);
			di->hash = scoutfs_key_offset(curs.key);
			break;
		}
	}

	scoutfs_btree_release(&curs);

out:
	if (ret < 0)
		inode = ERR_PTR(ret);
	else if (ino == 0)
		inode = NULL;
	else
		inode = scoutfs_iget(sb, ino);

	return d_splice_alias(inode, dentry);
}

/* this exists upstream so we can just delete it in a forward port */
static int dir_emit_dots(struct file *file, void *dirent, filldir_t filldir)
{
	struct dentry *dentry = file->f_path.dentry;
	struct inode *inode = dentry->d_inode;
	struct inode *parent = dentry->d_parent->d_inode;

	if (file->f_pos == 0) {
		if (filldir(dirent, ".", 1, 1, scoutfs_ino(inode), DT_DIR))
			return 0;
		file->f_pos = 1;
	}

	if (file->f_pos == 1) {
		if (filldir(dirent, "..", 2, 1, scoutfs_ino(parent), DT_DIR))
			return 0;
		file->f_pos = 2;
	}

	return 1;
}

/*
 * readdir simply iterates over the dirent items for the dir inode and
 * uses their offset as the readdir position.
 *
 * It will need to be careful not to read past the region of the dirent
 * hash offset keys that it has access to.
 */
static int scoutfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	struct scoutfs_dirent *dent;
	struct scoutfs_key first;
	struct scoutfs_key last;
	unsigned int name_len;
	int ret;
	u32 pos;

	if (!dir_emit_dots(file, dirent, filldir))
		return 0;

	scoutfs_set_key(&first, scoutfs_ino(inode), SCOUTFS_DIRENT_KEY,
			file->f_pos);
	scoutfs_set_key(&last, scoutfs_ino(inode), SCOUTFS_DIRENT_KEY,
			SCOUTFS_DIRENT_LAST_POS);

	while ((ret = scoutfs_btree_next(sb, &first, &last, &curs)) > 0) {
		dent = curs.val;
		name_len = item_name_len(&curs);
		pos = scoutfs_key_offset(curs.key);

		if (filldir(dirent, dent->name, name_len, pos,
			    le64_to_cpu(dent->ino), dentry_type(dent->type))) {
			ret = 0;
			break;
		}

		file->f_pos = pos + 1;
	}

	scoutfs_btree_release(&curs);

	return ret;
}

static int scoutfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		       dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct scoutfs_inode_info *si = SCOUTFS_I(dir);
	DECLARE_SCOUTFS_BTREE_CURSOR(curs);
	struct inode *inode = NULL;
	struct scoutfs_dirent *dent;
	struct dentry_info *di;
	struct scoutfs_key first;
	struct scoutfs_key last;
	struct scoutfs_key key;
	DECLARE_SCOUTFS_WRLOCK_HELD(held);
	int bytes;
	u64 ino;
	int ret;
	u64 h;

	di = alloc_dentry_info(dentry);
	if (IS_ERR(di))
		return PTR_ERR(di);

	if (dentry->d_name.len > SCOUTFS_NAME_LEN)
		return -ENAMETOOLONG;

	ret = scoutfs_alloc_ino(sb, &ino);
	if (ret)
		return ret;

	ret = scoutfs_wrlock_lock(sb, &held, 2, scoutfs_ino(dir), ino);
	if (ret)
		return ret;

	ret = scoutfs_dirty_inode_item(dir);
	if (ret)
		goto out;

	inode = scoutfs_new_inode(sb, dir, ino, mode, rdev);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto out;
	}

	bytes = dent_bytes(dentry->d_name.len);
	h = name_hash(dentry->d_name.name, dentry->d_name.len, si->salt);
	scoutfs_set_key(&first, scoutfs_ino(dir), SCOUTFS_DIRENT_KEY, h);
	scoutfs_set_key(&last, scoutfs_ino(dir), SCOUTFS_DIRENT_KEY,
			last_dirent_key_offset(h));

	/* find the first unoccupied key offset after the hashed name */
	key = first;
	while ((ret = scoutfs_btree_next(sb, &first, &last, &curs)) > 0) {
		key = *curs.key;
		scoutfs_inc_key(&key);
	}
	scoutfs_btree_release(&curs);
	if (ret < 0)
		goto out;

	if (scoutfs_key_cmp(&key, &last) > 0) {
		ret = -ENOSPC;
		goto out;
	}

	ret = scoutfs_btree_insert(sb, &key, bytes, &curs);
	if (ret)
		goto out;

	dent = curs.val;
	dent->ino = cpu_to_le64(scoutfs_ino(inode));
	dent->type = mode_to_type(inode->i_mode);
	memcpy(dent->name, dentry->d_name.name, dentry->d_name.len);
	di->hash = scoutfs_key_offset(&key);

	scoutfs_btree_release(&curs);

	i_size_write(dir, i_size_read(dir) + dentry->d_name.len);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	inode->i_mtime = inode->i_atime = inode->i_ctime = dir->i_mtime;

	if (S_ISDIR(mode)) {
		inc_nlink(inode);
		inc_nlink(dir);
	}

	scoutfs_update_inode_item(inode);
	scoutfs_update_inode_item(dir);

	insert_inode_hash(inode);
	d_instantiate(dentry, inode);
out:
	/* XXX delete the inode item here */
	if (ret && !IS_ERR_OR_NULL(inode))
		iput(inode);
	scoutfs_wrlock_unlock(sb, &held);
	return ret;
}

/* XXX hmm, do something with excl? */
static int scoutfs_create(struct inode *dir, struct dentry *dentry,
			  umode_t mode, bool excl)
{
	return scoutfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int scoutfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	return scoutfs_mknod(dir, dentry, mode | S_IFDIR, 0);
}

/*
 * Unlink removes the entry from its item and removes the item if ours
 * was the only remaining entry.
 */
static int scoutfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = dentry->d_inode;
	struct timespec ts = current_kernel_time();
	DECLARE_SCOUTFS_WRLOCK_HELD(held);
	struct dentry_info *di;
	struct scoutfs_key key;
	int ret = 0;

	if (WARN_ON_ONCE(!dentry->d_fsdata))
		return -EINVAL;
	di = dentry->d_fsdata;

	if (S_ISDIR(inode->i_mode) && i_size_read(inode))
		return -ENOTEMPTY;

	ret = scoutfs_wrlock_lock(sb, &held, 2, scoutfs_ino(dir),
				  scoutfs_ino(inode));
	if (ret)
		return ret;

	ret = scoutfs_dirty_inode_item(dir) ?:
	      scoutfs_dirty_inode_item(inode);
	if (ret)
		goto out;

	scoutfs_set_key(&key, scoutfs_ino(dir), SCOUTFS_DIRENT_KEY, di->hash);

	ret = scoutfs_btree_delete(sb, &key);
	if (ret)
		goto out;

	dir->i_ctime = ts;
	dir->i_mtime = ts;
	i_size_write(dir, i_size_read(dir) - dentry->d_name.len);

	inode->i_ctime = ts;
	drop_nlink(inode);
	if (S_ISDIR(inode->i_mode)) {
		drop_nlink(dir);
		drop_nlink(inode);
	}
	scoutfs_update_inode_item(inode);
	scoutfs_update_inode_item(dir);

out:
	scoutfs_wrlock_unlock(sb, &held);
	return ret;
}

const struct file_operations scoutfs_dir_fops = {
	.readdir	= scoutfs_readdir,
};

const struct inode_operations scoutfs_dir_iops = {
	.lookup		= scoutfs_lookup,
	.mknod		= scoutfs_mknod,
	.create		= scoutfs_create,
	.mkdir		= scoutfs_mkdir,
	.unlink		= scoutfs_unlink,
	.rmdir		= scoutfs_unlink,
};

void scoutfs_dir_exit(void)
{
	if (scoutfs_dentry_cachep) {
		kmem_cache_destroy(scoutfs_dentry_cachep);
		scoutfs_dentry_cachep = NULL;
	}
}

int scoutfs_dir_init(void)
{
	scoutfs_dentry_cachep = kmem_cache_create("scoutfs_dentry_info",
						  sizeof(struct dentry_info), 0,
						  SLAB_RECLAIM_ACCOUNT, NULL);
	if (!scoutfs_dentry_cachep)
		return -ENOMEM;

	return 0;
}
