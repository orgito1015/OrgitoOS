/*
 * OrgitoOS - Multiboot Specification Structures
 * 
 * Defines structures for parsing multiboot information passed by bootloader.
 * Based on Multiboot Specification version 0.6.96
 */

#ifndef ORGITOOS_INCLUDE_MULTIBOOT_H
#define ORGITOOS_INCLUDE_MULTIBOOT_H

#include <stdint.h>

/* Multiboot header magic number */
#define MULTIBOOT_HEADER_MAGIC  0x1BADB002

/* Multiboot bootloader magic number */
#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002

/* Multiboot flags */
#define MULTIBOOT_FLAG_MEM          (1 << 0)  /* Memory info available */
#define MULTIBOOT_FLAG_DEVICE       (1 << 1)  /* Boot device info available */
#define MULTIBOOT_FLAG_CMDLINE      (1 << 2)  /* Command line available */
#define MULTIBOOT_FLAG_MODS         (1 << 3)  /* Modules available */
#define MULTIBOOT_FLAG_MMAP         (1 << 6)  /* Memory map available */
#define MULTIBOOT_FLAG_FRAMEBUFFER  (1 << 12) /* Framebuffer info available */

/* multiboot_info.framebuffer_type values */
#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT 2

/* Multiboot memory map entry */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

/* Multiboot memory types */
#define MULTIBOOT_MEMORY_AVAILABLE        1
#define MULTIBOOT_MEMORY_RESERVED         2
#define MULTIBOOT_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MEMORY_NVS              4
#define MULTIBOOT_MEMORY_BADRAM           5

/*
 * Multiboot info structure - full canonical field set through the
 * framebuffer fields (the previous version of this struct truncated right
 * after mmap_addr, which was fine while nothing past that point was ever
 * read, but means framebuffer_addr/pitch/width/height/bpp/type below MUST
 * be at these exact offsets to line up with what GRUB/QEMU actually wrote,
 * matching the standard Multiboot 1 specification layout).
 */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    union {
        struct {
            uint32_t framebuffer_palette_addr;
            uint16_t framebuffer_palette_num_colors;
        } palette;
        struct {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        } rgb;
    } color_info;
} __attribute__((packed));

#endif /* ORGITOOS_INCLUDE_MULTIBOOT_H */
