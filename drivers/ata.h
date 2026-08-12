/*
 * OrgitoOS - ATA/IDE PIO disk driver
 *
 * Minimal polled-PIO driver for the primary ATA bus's master drive
 * (legacy ports 0x1F0-0x1F7 + 0x3F6) - no IRQ-driven PIO, no bus-master
 * DMA, no ATAPI/slave-drive support. Gives fs/diskfs.c a real,
 * persistent 512-byte-sector block device.
 */

#ifndef ORGITOOS_DRIVERS_ATA_H
#define ORGITOOS_DRIVERS_ATA_H

#include <stdint.h>
#include <stddef.h>

#define ATA_SECTOR_SIZE 512

/* Detect and IDENTIFY the primary master ATA disk. Returns 0 if a usable
 * PATA hard disk is present, -1 otherwise (no drive, or an ATAPI/other
 * non-ATA device answered instead). Safe to call even with no disk
 * attached (e.g. QEMU without -hda) - just returns -1. */
int ata_init(void);

/* True if ata_init() found a usable disk. */
int ata_is_present(void);

/* Total addressable 512-byte sectors (0 if no disk). */
uint32_t ata_sector_count(void);

/* Read/write `count` consecutive 512-byte sectors starting at 28-bit LBA
 * `lba` into/from `buffer` (must be at least `count * ATA_SECTOR_SIZE`
 * bytes). Returns 0 on success, -1 on error, no disk, or an
 * out-of-range/oversized request. */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

#endif /* ORGITOOS_DRIVERS_ATA_H */
