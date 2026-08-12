/*
 * OrgitoOS - OrgitoFS: a small custom on-disk filesystem
 *
 * Persists a directory tree to the ATA disk (drivers/ata.h) and mounts
 * it into the VFS at /mnt, so files created there survive a reboot -
 * unlike the in-memory ramfs mounted at /, which is rebuilt from
 * scratch every boot.
 */

#ifndef ORGITOOS_FS_DISKFS_H
#define ORGITOOS_FS_DISKFS_H

/*
 * Detects a disk via drivers/ata.h, mounts its OrgitoFS superblock at
 * /mnt (formatting a fresh one first if no valid superblock is found),
 * and rebuilds the in-memory VFS node tree from the on-disk inode
 * table. No-op (leaves /mnt absent) if ata_init() found no disk -
 * mirrors net_init()'s "no NIC" fallback.
 */
void diskfs_init(void);

#endif /* ORGITOOS_FS_DISKFS_H */
