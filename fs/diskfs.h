/*
 * OrgitoOS - OrgitoFS: a small custom on-disk filesystem
 *
 * Persists a directory tree to the ATA disk (drivers/ata.h) and mounts
 * it into the VFS at /mnt, so files created there survive a reboot -
 * unlike the in-memory ramfs mounted at /, which is rebuilt from
 * scratch every boot. See fs/diskfs.c for the on-disk layout.
 */

#ifndef ORGITOOS_FS_DISKFS_H
#define ORGITOOS_FS_DISKFS_H

#include <stdint.h>

/*
 * Mount the disk's OrgitoFS at /mnt, formatting a fresh one first if no
 * valid superblock is found. Call after ata_init() succeeded. No-op
 * (leaves /mnt absent) if there is no disk.
 */
void diskfs_init(void);

/* 1 if /mnt is mounted. */
int diskfs_is_mounted(void);

/* Write all metadata (inode table incl. permission bits, block bitmap)
 * back to disk. Data is always written through; this is only needed for
 * changes made directly to nodes, e.g. chmod. */
void diskfs_sync(void);

/* Usage numbers for the `df` command (all 0 if not mounted). Blocks are
 * 512 bytes. */
void diskfs_stats(uint32_t *total_blocks, uint32_t *free_blocks,
                  uint32_t *total_inodes, uint32_t *free_inodes);

/* Largest file OrgitoFS can store, in bytes. */
uint32_t diskfs_max_file_size(void);

#endif /* ORGITOOS_FS_DISKFS_H */
