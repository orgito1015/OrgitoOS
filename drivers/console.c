/*
 * OrgitoOS - Console Driver Implementation
 * 
 * VGA text mode console for kernel output.
 */

#include "console.h"
#include "serial.h"
#include "framebuffer.h"
#include "../include/multiboot.h"
#include <stdint.h>
#include <stddef.h>

/* VGA memory address */
#define VGA_MEMORY ((uint16_t*)0xB8000)

static uint16_t* const vga_buf = VGA_MEMORY;
static size_t term_row = 0;
static size_t term_col = 0;
static uint8_t term_color = 0x0F; /* white on black */

/* Set once in console_init() and never again: true if a real linear
 * framebuffer (drivers/framebuffer.c) is active, in which case every
 * function below draws through it instead of the VGA text buffer. */
static int use_framebuffer = 0;

/* 16-color VGA palette, so term_color's fg/bg indices (console_set_color())
 * mean the same colors on the framebuffer backend as they always have in
 * VGA text mode. */
static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};
static inline uint32_t term_fg_rgb(void) { return vga_palette[term_color & 0x0F]; }
static inline uint32_t term_bg_rgb(void) { return vga_palette[(term_color >> 4) & 0x0F]; }

/* Output capture state - see console_set_capture() in console.h */
static char* capture_buf = 0;
static size_t capture_cap = 0;
static size_t capture_len = 0;

/* Create a VGA entry with character and color */
static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Scroll the terminal up by one line */
static void terminal_scroll(void) {
    /* Scroll up by moving all lines up by one */
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            vga_buf[y * VGA_WIDTH + x] = vga_buf[(y + 1) * VGA_WIDTH + x];
        }
    }
    /* Clear the last line */
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
    }
}

/* Initialize console */
void console_init(struct multiboot_info *mboot) {
    /* Bring up COM1 so all console output is mirrored to the serial port
     * (harmless no-op if no UART is present). */
    serial_init();

    /* fb_init() is conservative: it only activates for a 32bpp RGB
     * framebuffer and leaves use_framebuffer false for anything else (no
     * framebuffer info at all, an EGA-text fallback, or an unsupported
     * pixel format) - console_clear() below then falls back to the exact
     * same VGA text buffer path this function always used. */
    use_framebuffer = (fb_init(mboot) == 0);
    console_clear();
}

/* Clear console */
void console_clear(void) {
    if (use_framebuffer) {
        fb_clear(term_bg_rgb());
    } else {
        for (size_t y = 0; y < VGA_HEIGHT; y++) {
            for (size_t x = 0; x < VGA_WIDTH; x++) {
                vga_buf[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
            }
        }
    }
    term_row = 0;
    term_col = 0;
}

/* Backspace operation */
void console_backspace(void) {
    size_t cols = use_framebuffer ? fb_text_cols() : VGA_WIDTH;

    if (term_col > 0) {
        term_col--;
    } else if (term_row > 0) {
        term_row--;
        term_col = cols - 1;
    }

    if (use_framebuffer) {
        fb_putc_cell(term_col, term_row, ' ', term_fg_rgb(), term_bg_rgb());
    } else {
        vga_buf[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
    }
}

/* Put a character on the console */
void console_put_char(char c) {
    /* While capturing, redirect entirely into the capture buffer - no
     * screen draw, no serial mirror - so piped output stays clean. */
    if (capture_buf != 0) {
        if (capture_len < capture_cap) {
            capture_buf[capture_len++] = c;
        }
        return;
    }

    /* Mirror every character to the serial port for headless logging,
     * expanding '\n' to CR/LF so terminals render lines correctly. */
    if (c == '\n') {
        serial_write_char('\r');
    }
    serial_write_char(c);

    size_t cols = use_framebuffer ? fb_text_cols() : VGA_WIDTH;
    size_t rows = use_framebuffer ? fb_text_rows() : VGA_HEIGHT;

    if (c == '\n') {
        term_col = 0;
        term_row++;
        if (term_row >= rows) {
            if (use_framebuffer) fb_scroll_up_one_row(term_bg_rgb()); else terminal_scroll();
            term_row = rows - 1;
        }
        return;
    }

    if (use_framebuffer) {
        fb_putc_cell(term_col, term_row, c, term_fg_rgb(), term_bg_rgb());
    } else {
        vga_buf[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);
    }

    term_col++;
    if (term_col >= cols) {
        term_col = 0;
        term_row++;
        if (term_row >= rows) {
            if (use_framebuffer) fb_scroll_up_one_row(term_bg_rgb()); else terminal_scroll();
            term_row = rows - 1;
        }
    }
}

/* Write a string to the console */
void console_write(const char* s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        console_put_char(s[i]);
    }
}

/* Set console color */
void console_set_color(uint8_t fg, uint8_t bg) {
    term_color = fg | (bg << 4);
}

/* Begin capturing console output into buf (up to cap bytes, no NUL added -
 * caller records the returned length separately). */
void console_set_capture(char* buf, size_t cap) {
    capture_buf = buf;
    capture_cap = cap;
    capture_len = 0;
}

/* End capture and return the number of bytes written into the buffer. */
size_t console_end_capture(void) {
    size_t len = capture_len;
    capture_buf = 0;
    capture_cap = 0;
    capture_len = 0;
    return len;
}

/* Legacy function aliases for backwards compatibility */
void terminal_put_char(char c) {
    console_put_char(c);
}

void terminal_backspace(void) {
    console_backspace();
}
