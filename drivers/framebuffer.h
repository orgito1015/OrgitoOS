/*
 * OrgitoOS - Linear Framebuffer Driver
 *
 * A real, displayed pixel surface obtained via the Multiboot-requested
 * video mode (arch/x86/boot.S, include/multiboot.h) - no runtime BIOS/VM86
 * calls, since the bootloader sets the mode up before jumping to the
 * kernel. Provides:
 *
 *   - A small 5x7 bitmap-font text renderer (fb_putc_cell()/fb_clear()/
 *     fb_scroll_up_one_row()), so drivers/console.c can keep its exact
 *     existing public API and callers while swapping its backend from the
 *     VGA text buffer to real pixels when a framebuffer is available.
 *   - A raw blit entry point (fb_blit_argb_buffer()) so kernel/gui.c's
 *     simulated in-memory framebuffer can actually be shown on screen.
 *
 * fb_init() is deliberately conservative: it only activates for a 32bpp
 * RGB framebuffer (multiboot_info->framebuffer_type == 1, bpp == 32) and
 * leaves fb_is_active() false for anything else (no framebuffer info at
 * all, an EGA-text fallback, or an unsupported pixel format) - callers
 * must always be prepared for it to be inactive, and drivers/console.c's
 * whole reason for existing this way is to fall back to the legacy VGA
 * text buffer whenever that happens.
 */

#ifndef ORGITOOS_DRIVERS_FRAMEBUFFER_H
#define ORGITOOS_DRIVERS_FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

struct multiboot_info;

/* Inspect `mboot` for a usable 32bpp RGB linear framebuffer and, if found,
 * record its geometry. Safe to call even with no framebuffer info present
 * (fb_is_active() simply stays false). Must be called before paging is
 * enabled - stores the framebuffer's raw physical address as a directly
 * dereferenceable pointer, which only works pre-paging (or once
 * fb_remap_after_paging() has identity-mapped it). Returns 0 if active,
 * -1 otherwise. */
int fb_init(struct multiboot_info *mboot);

/* Identity-maps the framebuffer's physical MMIO range into the kernel's
 * page directory so it stays reachable once paging is enabled. Call
 * exactly once, immediately after vmm_init() and before any further
 * console output. No-op if fb_init() never activated. */
void fb_remap_after_paging(void);

/* True if fb_init() found and activated a usable framebuffer. */
int fb_is_active(void);

/* Text-console grid dimensions (in whole FB_CELL_W x FB_CELL_H cells).
 * Both are 0 if !fb_is_active(). */
size_t fb_text_cols(void);
size_t fb_text_rows(void);

/* Draw one character cell at text-grid position (col, row). Colors are
 * 0xRRGGBB (or 0xAARRGGBB - the alpha byte, if any, is ignored). No-op if
 * !fb_is_active() or the position is out of range. */
void fb_putc_cell(size_t col, size_t row, char c, uint32_t fg_rgb, uint32_t bg_rgb);

/* Fill the whole framebuffer with `bg_rgb`. No-op if !fb_is_active(). */
void fb_clear(uint32_t bg_rgb);

/* Shift the framebuffer's pixel content up by one text row (FB_CELL_H
 * pixel rows) and fill the newly exposed bottom row with `bg_rgb`. No-op
 * if !fb_is_active(). */
void fb_scroll_up_one_row(uint32_t bg_rgb);

/* Blit a `src_w`x`src_h` 0xAARRGGBB pixel buffer to the top-left corner of
 * the real framebuffer, clipped to whichever of the two is smaller in
 * each dimension - no scaling. No-op if !fb_is_active() or src is NULL. */
void fb_blit_argb_buffer(const uint32_t *src, uint32_t src_w, uint32_t src_h);

#endif /* ORGITOOS_DRIVERS_FRAMEBUFFER_H */
