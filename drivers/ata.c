/*
 * OrgitoOS - ATA/IDE PIO disk driver
 *
 * Drives the legacy ATA (PATA) primary-bus master drive through the
 * classic ISA I/O ports (0x1F0-0x1F7 command block, 0x3F6 control
 * block) using polled PIO - the same registers real PC BIOSes and every
 * OSDev "ATA PIO Mode" tutorial use. Deliberately minimal: no IRQ-driven
 * PIO, no bus-master DMA, no ATAPI/slave-drive support. This is the
 * whole point of "PIO" in the roadmap's "Disk driver (ATA/IDE PIO)" -
 * just enough to give fs/diskfs.c a real, persistent block device.
 *
 * QEMU users: attach a disk with `-hda disk.img` (already wired into
 * tools/run-qemu.sh / make run / make run-iso / make debug). Booting
 * without one is fine - ata_init() just reports no drive and
 * fs/diskfs.c falls back to "no persistent storage" the same way
 * net_init() falls back to "no NIC".
 */

#include "ata.h"
#include "console.h"
#include "../arch/x86/ports.h"
#include "../kernel/string.h"

/* Primary ATA bus - I/O port layout */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CONTROL 0x3F6

#define ATA_REG_DATA        (ATA_PRIMARY_IO + 0)
#define ATA_REG_SECCOUNT0   (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA0        (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA1        (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA2        (ATA_PRIMARY_IO + 5)
#define ATA_REG_HDDEVSEL    (ATA_PRIMARY_IO + 6)
#define ATA_REG_COMMAND     (ATA_PRIMARY_IO + 7)
#define ATA_REG_STATUS      (ATA_PRIMARY_IO + 7)
#define ATA_REG_ALTSTATUS   ATA_PRIMARY_CONTROL

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_BSY  0x80

/* Generous polling bound - a real/emulated drive answers in well under
 * this; a genuinely absent/wedged one should time out rather than hang
 * boot forever. Not tied to the timer (ata_init() runs before the timer
 * IRQ is unmasked), so this is a plain busy-wait iteration count. */
#define ATA_POLL_ITERATIONS 100000u

static int disk_present = 0;
static uint32_t disk_sectors = 0;

/* Reading the alternate status register costs ~100ns on real hardware
 * and has no other side effect - four of them is the standard "400ns
 * delay" OSDev recommends after writing the drive-select register,
 * before that drive's status register is trustworthy. */
static void ata_400ns_delay(void) {
    for (int i = 0; i < 4; i++) {
        inb(ATA_REG_ALTSTATUS);
    }
}

/* Poll until BSY clears. Returns 0 once BSY=0, -1 on timeout. */
static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < ATA_POLL_ITERATIONS; i++) {
        if (!(inb(ATA_REG_STATUS) & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

/* Poll until DRQ (data ready) is set, or ERR/DF signals failure. */
static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < ATA_POLL_ITERATIONS; i++) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (status & ATA_SR_DRQ) {
            return 0;
        }
    }
    return -1;
}

int ata_init(void) {
    disk_present = 0;
    disk_sectors = 0;

    /* Select primary master, LBA mode, all-zero LBA for IDENTIFY. */
    outb(ATA_REG_HDDEVSEL, 0xA0);
    ata_400ns_delay();

    outb(ATA_REG_SECCOUNT0, 0);
    outb(ATA_REG_LBA0, 0);
    outb(ATA_REG_LBA1, 0);
    outb(ATA_REG_LBA2, 0);
    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_REG_STATUS) == 0) {
        console_write("ATA: no primary master drive present\n");
        return -1;
    }

    if (ata_wait_not_busy() != 0) {
        console_write("ATA: IDENTIFY timed out (BSY never cleared)\n");
        return -1;
    }

    /* A non-zero LBA1/LBA2 here means an ATAPI (or other non-ATA) device
     * answered the drive-select instead of a plain PATA hard disk -
     * out of scope for this driver. */
    if (inb(ATA_REG_LBA1) != 0 || inb(ATA_REG_LBA2) != 0) {
        console_write("ATA: primary master is not a PATA hard disk (ATAPI?)\n");
        return -1;
    }

    if (ata_wait_drq() != 0) {
        console_write("ATA: IDENTIFY failed (ERR/DF set)\n");
        return -1;
    }

    uint16_t identify[256];
    insw(ATA_REG_DATA, identify, 256);

    /* IDENTIFY words 60-61: total addressable sectors, LBA28 mode. */
    disk_sectors = ((uint32_t)identify[61] << 16) | identify[60];
    disk_present = 1;

    console_write("ATA: primary master detected, ");
    {
        char buf[12];
        itoa((int)disk_sectors, buf, 10);
        console_write(buf);
    }
    console_write(" sectors (~");
    {
        char buf[12];
        itoa((int)(disk_sectors / 2048), buf, 10);   /* 512B sectors -> MiB */
        console_write(buf);
    }
    console_write(" MiB)\n");

    return 0;
}

int ata_is_present(void) {
    return disk_present;
}

uint32_t ata_sector_count(void) {
    return disk_sectors;
}

static int ata_select_lba(uint32_t lba, uint8_t sector_count) {
    if (!disk_present) return -1;
    if (lba > 0x0FFFFFFFu) return -1;   /* LBA28 range only */

    outb(ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_400ns_delay();
    outb(ATA_REG_SECCOUNT0, sector_count);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    return 0;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    if (!disk_present || !buffer || count == 0) return -1;
    if ((uint64_t)lba + count > disk_sectors) return -1;

    if (ata_select_lba(lba, count) != 0) return -1;
    outb(ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    uint16_t *dst = (uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_not_busy() != 0 || ata_wait_drq() != 0) {
            console_write("ATA: read timed out/error\n");
            return -1;
        }
        insw(ATA_REG_DATA, dst, 256);
        dst += 256;
    }
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    if (!disk_present || !buffer || count == 0) return -1;
    if ((uint64_t)lba + count > disk_sectors) return -1;

    if (ata_select_lba(lba, count) != 0) return -1;
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    const uint16_t *src = (const uint16_t *)buffer;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_not_busy() != 0 || ata_wait_drq() != 0) {
            console_write("ATA: write timed out/error\n");
            return -1;
        }
        outsw(ATA_REG_DATA, src, 256);
        src += 256;
    }

    /* Flush the drive's write cache and wait for completion before
     * returning - without this, a reset/power loss right after this
     * call returns could lose data still sitting in the drive's cache
     * rather than on the medium. */
    outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_not_busy();

    return 0;
}
