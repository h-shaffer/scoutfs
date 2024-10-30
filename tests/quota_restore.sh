#!/usr/bin/env bash

T_EX_META_DEV=/dev/vdd
T_EX_DATA_DEV=/dev/vde
TMP_DIR="/media/sfs_test_dir"
SCR="$TMP_DIR/sfs_test_mnt"

scratch_mkfs() {
	scoutfs mkfs $@ \
		-A -f -Q 0,127.0.0.1,53000 $T_EX_META_DEV $T_EX_DATA_DEV
}

scratch_mount() {
	mount -t scoutfs -o metadev_path=$T_EX_META_DEV,quorum_slot_nr=0 $T_EX_DATA_DEV $SCR
}

echo "== Quota test no restore"
scratch_mkfs -V 2 -m 10G -d 60G > $TMP_DIR/mkfs.out 2>&1
scratch_mount
scoutfs quota-add -p "$SCR" -r "7 13,L,- 15,L,- 17,L,- I 33 -"
scoutfs quota-list -p "$SCR"
umount "$SCR"
sleep 1
scratch_mount
scoutfs print $T_EX_META_DEV > $TMP_DIR/quota_no_restore.out
umount "$SCR"

echo "== Restore quota test"
scratch_mkfs -V 2 -m 10G -d 60G > $TMP_DIR/mkfs.out 2>&1
parallel_restore -m $T_EX_META_DEV -n 16 > /dev/null
scratch_mount
scoutfs quota-list -p "$SCR"	
umount "$SCR"
sleep 1
scratch_mount
scoutfs print $T_EX_META_DEV > $TMP_DIR/quota_restore.out
umount "$SCR"
