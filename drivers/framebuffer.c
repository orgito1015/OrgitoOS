/*
 * OrgitoOS - Linear Framebuffer Driver Implementation
 *
 * See framebuffer.h for the design overview. Two things worth flagging:
 *
 *   1. fb_ptr is a *physical* address used as a raw pointer, only safe to
 *      dereference once fb_remap_after_paging() has identity-mapped that
 *      exact physical range (kernel/kernel.c calls it right after
 *      vmm_init()). fb_init() itself runs earlier, from console_init() -
 *      the very first thing kmain() does, well before vmm_init() - so it
 *      deliberately only records geometry (fb_detected) and never touches
 *      fb_ptr. Every function that actually draws is gated on the
 *      separate fb_active flag, which stays 0 until fb_remap_after_paging()
 *      finishes, so a console_write() in that window (there are several -
 *      GDT/IDT/PIC/timer/keyboard setup all log before vmm_init() runs)
 *      just silently no-ops on screen instead of touching unmapped memory;
 *      it still reaches serial (drivers/console.c mirrors unconditionally).
 *      console_framebuffer_ready() (called right after fb_remap_after_paging())
 *      does the deferred fb_clear() and resets the text cursor once
 *      drawing is finally safe.
 *
 *   2. The 5x7 font below is a deliberately simple, self-authored bitmap
 *      font (not a copy of real VGA/VESA font ROM data) covering space,
 *      digits, uppercase letters (lowercase folds to the same glyph - a
 *      documented scope simplification, not a bug), and the punctuation
 *      this kernel's own shell output actually uses. Anything outside
 *      that set renders as a checkerboard marker, so an unsupported
 *      glyph is visibly distinct from a rendering bug rather than
 *      silently blank.
 */

#include "framebuffer.h"
#include "../include/multiboot.h"
#include "../memory/vmm.h"

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static uint8_t *fb_ptr = 0;
static uint32_t fb_phys_addr = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_mapped_size = 0;
static uint8_t  fb_red_pos = 0, fb_red_size = 0;
static uint8_t  fb_green_pos = 0, fb_green_size = 0;
static uint8_t  fb_blue_pos = 0, fb_blue_size = 0;
static int      fb_detected = 0; /* geometry known (fb_init() succeeded) */
static int      fb_active = 0;   /* mapped by fb_remap_after_paging() - safe to draw */

#define FB_GLYPH_W 5
#define FB_GLYPH_H 7
#define FB_CELL_W  8
#define FB_CELL_H  16

/* ------------------------------------------------------------------ */
/* Init / remap                                                         */
/* ------------------------------------------------------------------ */

int fb_init(struct multiboot_info *mboot) {
    fb_detected = 0;
    fb_active = 0;
    if (!mboot) return -1;
    if (!(mboot->flags & MULTIBOOT_FLAG_FRAMEBUFFER)) return -1;
    if (mboot->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB) return -1;
    if (mboot->framebuffer_bpp != 32) return -1;          /* only 32bpp supported - keeps pixel packing simple */
    if (mboot->framebuffer_addr > 0xFFFFFFFFULL) return -1; /* stay addressable in this 32-bit kernel */
    if (mboot->framebuffer_width == 0 || mboot->framebuffer_height == 0 ||
        mboot->framebuffer_pitch == 0) {
        return -1;
    }

    fb_phys_addr = (uint32_t)mboot->framebuffer_addr;
    fb_pitch     = mboot->framebuffer_pitch;
    fb_width     = mboot->framebuffer_width;
    fb_height    = mboot->framebuffer_height;
    fb_red_pos   = mboot->color_info.rgb.framebuffer_red_field_position;
    fb_red_size  = mboot->color_info.rgb.framebuffer_red_mask_size;
    fb_green_pos = mboot->color_info.rgb.framebuffer_green_field_position;
    fb_green_size= mboot->color_info.rgb.framebuffer_green_mask_size;
    fb_blue_pos  = mboot->color_info.rgb.framebuffer_blue_field_position;
    fb_blue_size = mboot->color_info.rgb.framebuffer_blue_mask_size;

    /* fb_ptr is recorded but never dereferenced here - only geometry is
     * read from `mboot`, which is why fb_detected (not fb_active) is what
     * this function sets. See the file header comment. */
    fb_ptr = (uint8_t *)(uintptr_t)fb_phys_addr;
    fb_mapped_size = fb_pitch * fb_height;
    fb_detected = 1;
    return 0;
}

void fb_remap_after_paging(void) {
    if (!fb_detected) return;
    uint32_t start = fb_phys_addr & ~((uint32_t)PAGE_SIZE - 1u);
    uint32_t end   = (fb_phys_addr + fb_mapped_size + PAGE_SIZE - 1u) & ~((uint32_t)PAGE_SIZE - 1u);
    vmm_identity_map_region(vmm_kernel_directory(), (void *)(uintptr_t)start,
                            end - start, PTE_PRESENT | PTE_WRITABLE);
    fb_active = 1;
}

int fb_is_active(void) { return fb_active; }

/* Text-grid sizing only needs the geometry fb_init() already recorded, not
 * a mapped framebuffer, so it's gated on fb_detected: drivers/console.c
 * calls these on every character even before fb_remap_after_paging() has
 * run, and returning 0 here in that window would make its col/row
 * wrap-around math underflow term_row/term_col instead of just laying out
 * text normally (which then draws for real once fb_active flips on). */
size_t fb_text_cols(void) { return fb_detected ? (size_t)(fb_width / FB_CELL_W) : 0; }
size_t fb_text_rows(void) { return fb_detected ? (size_t)(fb_height / FB_CELL_H) : 0; }

/* ------------------------------------------------------------------ */
/* Pixel access                                                         */
/* ------------------------------------------------------------------ */

/* Packs one 8-bit color channel into its field of the framebuffer's
 * native pixel format. Guards against mask_size >= 8 (a straight 8:8
 * channel) so the shift amount is never negative. */
static inline uint32_t pack_channel(uint8_t v, uint8_t pos, uint8_t size) {
    if (size == 0) return 0;
    if (size >= 8) return ((uint32_t)v) << pos;
    return ((uint32_t)(v >> (8 - size))) << pos;
}

static inline uint32_t pack_pixel(uint32_t rgb) {
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    return pack_channel(r, fb_red_pos, fb_red_size) |
           pack_channel(g, fb_green_pos, fb_green_size) |
           pack_channel(b, fb_blue_pos, fb_blue_size);
}

static inline void fb_set_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
    if (x >= fb_width || y >= fb_height) return;
    uint8_t *row = fb_ptr + (size_t)y * fb_pitch;
    ((uint32_t *)row)[x] = pack_pixel(rgb);
}

/* ------------------------------------------------------------------ */
/* 5x7 font                                                             */
/*                                                                       */
/* Each glyph is 7 bytes, one per row, only the low 5 bits of each byte  */
/* are meaningful (bit 4 = leftmost column, bit 0 = rightmost).          */
/* ------------------------------------------------------------------ */

static const uint8_t FONT_UNKNOWN[FB_GLYPH_H] = {
    0b10101, 0b01010, 0b10101, 0b01010, 0b10101, 0b01010, 0b10101
};

static const uint8_t *font_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');   /* case folds - see file header */

    switch (c) {
        case ' ': { static const uint8_t g[7] = {0,0,0,0,0,0,0}; return g; }

        /* Digits */
        case '0': { static const uint8_t g[7] = {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}; return g; }
        case '1': { static const uint8_t g[7] = {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}; return g; }
        case '2': { static const uint8_t g[7] = {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}; return g; }
        case '3': { static const uint8_t g[7] = {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}; return g; }
        case '4': { static const uint8_t g[7] = {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}; return g; }
        case '5': { static const uint8_t g[7] = {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}; return g; }
        case '6': { static const uint8_t g[7] = {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}; return g; }
        case '7': { static const uint8_t g[7] = {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}; return g; }
        case '8': { static const uint8_t g[7] = {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}; return g; }
        case '9': { static const uint8_t g[7] = {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}; return g; }

        /* Uppercase letters */
        case 'A': { static const uint8_t g[7] = {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}; return g; }
        case 'B': { static const uint8_t g[7] = {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}; return g; }
        case 'C': { static const uint8_t g[7] = {0b01111,0b10000,0b10000,0b10000,0b10000,0b10000,0b01111}; return g; }
        case 'D': { static const uint8_t g[7] = {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}; return g; }
        case 'E': { static const uint8_t g[7] = {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}; return g; }
        case 'F': { static const uint8_t g[7] = {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}; return g; }
        case 'G': { static const uint8_t g[7] = {0b01111,0b10000,0b10000,0b10011,0b10001,0b10001,0b01111}; return g; }
        case 'H': { static const uint8_t g[7] = {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}; return g; }
        case 'I': { static const uint8_t g[7] = {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}; return g; }
        case 'J': { static const uint8_t g[7] = {0b00001,0b00001,0b00001,0b00001,0b00001,0b10001,0b01110}; return g; }
        case 'K': { static const uint8_t g[7] = {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}; return g; }
        case 'L': { static const uint8_t g[7] = {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}; return g; }
        case 'M': { static const uint8_t g[7] = {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}; return g; }
        case 'N': { static const uint8_t g[7] = {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}; return g; }
        case 'O': { static const uint8_t g[7] = {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}; return g; }
        case 'P': { static const uint8_t g[7] = {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}; return g; }
        case 'Q': { static const uint8_t g[7] = {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}; return g; }
        case 'R': { static const uint8_t g[7] = {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}; return g; }
        case 'S': { static const uint8_t g[7] = {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}; return g; }
        case 'T': { static const uint8_t g[7] = {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}; return g; }
        case 'U': { static const uint8_t g[7] = {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}; return g; }
        case 'V': { static const uint8_t g[7] = {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}; return g; }
        case 'W': { static const uint8_t g[7] = {0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010}; return g; }
        case 'X': { static const uint8_t g[7] = {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}; return g; }
        case 'Y': { static const uint8_t g[7] = {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}; return g; }
        case 'Z': { static const uint8_t g[7] = {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}; return g; }

        /* Punctuation actually used by this kernel's own shell output */
        case '.': { static const uint8_t g[7] = {0,0,0,0,0,0b01100,0b01100}; return g; }
        case ',': { static const uint8_t g[7] = {0,0,0,0,0,0b01100,0b01000}; return g; }
        case ':': { static const uint8_t g[7] = {0,0b01100,0b01100,0,0b01100,0b01100,0}; return g; }
        case ';': { static const uint8_t g[7] = {0,0b01100,0b01100,0,0b01100,0b01000,0}; return g; }
        case '!': { static const uint8_t g[7] = {0b00100,0b00100,0b00100,0b00100,0b00100,0,0b00100}; return g; }
        case '?': { static const uint8_t g[7] = {0b01110,0b10001,0b00001,0b00010,0b00100,0,0b00100}; return g; }
        case '-': { static const uint8_t g[7] = {0,0,0,0b11111,0,0,0}; return g; }
        case '_': { static const uint8_t g[7] = {0,0,0,0,0,0,0b11111}; return g; }
        case '/': { static const uint8_t g[7] = {0b00001,0b00010,0b00100,0b00100,0b01000,0b10000,0b10000}; return g; }
        case '\\':{ static const uint8_t g[7] = {0b10000,0b01000,0b00100,0b00100,0b00010,0b00001,0b00001}; return g; }
        case '(': { static const uint8_t g[7] = {0b00010,0b00100,0b01000,0b01000,0b01000,0b00100,0b00010}; return g; }
        case ')': { static const uint8_t g[7] = {0b01000,0b00100,0b00010,0b00010,0b00010,0b00100,0b01000}; return g; }
        case '<': { static const uint8_t g[7] = {0b00010,0b00100,0b01000,0b10000,0b01000,0b00100,0b00010}; return g; }
        case '>': { static const uint8_t g[7] = {0b01000,0b00100,0b00010,0b00001,0b00010,0b00100,0b01000}; return g; }
        case '=': { static const uint8_t g[7] = {0,0,0b11111,0,0b11111,0,0}; return g; }
        case '+': { static const uint8_t g[7] = {0,0b00100,0b00100,0b11111,0b00100,0b00100,0}; return g; }
        case '*': { static const uint8_t g[7] = {0,0b10101,0b01110,0b11111,0b01110,0b10101,0}; return g; }
        case '\'':{ static const uint8_t g[7] = {0b00100,0b00100,0b01000,0,0,0,0}; return g; }
        case '"': { static const uint8_t g[7] = {0b01010,0b01010,0,0,0,0,0}; return g; }
        case '@': { static const uint8_t g[7] = {0b01110,0b10001,0b10111,0b10101,0b10111,0b10000,0b01111}; return g; }

        default: return FONT_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* Text rendering                                                       */
/* ------------------------------------------------------------------ */

void fb_putc_cell(size_t col, size_t row, char c, uint32_t fg_rgb, uint32_t bg_rgb) {
    if (!fb_active) return;

    uint32_t base_x = (uint32_t)col * FB_CELL_W;
    uint32_t base_y = (uint32_t)row * FB_CELL_H;
    const uint8_t *glyph = font_glyph(c);

    for (uint32_t cy = 0; cy < FB_CELL_H; cy++) {
        for (uint32_t cx = 0; cx < FB_CELL_W; cx++) {
            int on = 0;
            if (cy >= 1 && cy < 1 + FB_GLYPH_H && cx >= 1 && cx < 1 + FB_GLYPH_W) {
                uint32_t gy = cy - 1;
                uint32_t gx = cx - 1;
                on = (glyph[gy] >> (FB_GLYPH_W - 1 - gx)) & 1;
            }
            fb_set_pixel(base_x + cx, base_y + cy, on ? fg_rgb : bg_rgb);
        }
    }
}

void fb_clear(uint32_t bg_rgb) {
    if (!fb_active) return;
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_set_pixel(x, y, bg_rgb);
        }
    }
}

void fb_scroll_up_one_row(uint32_t bg_rgb) {
    if (!fb_active) return;
    if (fb_height <= FB_CELL_H) {
        fb_clear(bg_rgb);
        return;
    }

    size_t row_bytes = (size_t)fb_pitch * FB_CELL_H;
    size_t total = (size_t)fb_pitch * fb_height;
    size_t move_bytes = total - row_bytes;

    /* Forward copy is safe here: dst[i] (address i) is only ever written
     * after src[i] (address i + row_bytes) has already been read at an
     * earlier iteration (i - row_bytes), since row_bytes > 0 and the loop
     * runs in increasing order - the same safety argument that makes
     * memmove's "dst < src" case a plain forward copy. */
    for (size_t i = 0; i < move_bytes; i++) {
        fb_ptr[i] = fb_ptr[i + row_bytes];
    }

    for (uint32_t y = fb_height - FB_CELL_H; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_set_pixel(x, y, bg_rgb);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Raw blit (kernel/gui.c)                                              */
/* ------------------------------------------------------------------ */

void fb_blit_argb_buffer(const uint32_t *src, uint32_t src_w, uint32_t src_h) {
    if (!fb_active || !src) return;

    uint32_t w = (src_w < fb_width) ? src_w : fb_width;
    uint32_t h = (src_h < fb_height) ? src_h : fb_height;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            fb_set_pixel(x, y, src[(size_t)y * src_w + x]);
        }
    }
}
