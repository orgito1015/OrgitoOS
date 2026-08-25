/*
 * OrgitoOS - Console Driver Implementation
 *
 * Split-screen VGA text mode console: two independent panes side by side,
 * separated by a one-column vertical divider. Each pane has its own
 * cursor position and color; console_write()/console_put_char()/
 * console_backspace()/console_clear()/console_set_color() resolve which
 * pane to target via process_current()->pane, so the ~470 existing call
 * sites elsewhere in the kernel need no changes at all - before
 * process_init() has run (early boot), process_current() is NULL and
 * everything defaults to the left pane.
 */

#include "console.h"
#include "serial.h"
#include "../arch/x86/ports.h"
#include "../process/process.h"
#include <stdint.h>
#include <stddef.h>

/* VGA memory address */
#define VGA_MEMORY ((uint16_t*)0xB8000)

/* VGA CRTC ports, for moving the hardware (blinking) text cursor */
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5
#define VGA_CRTC_CURSOR_HIGH 0x0E
#define VGA_CRTC_CURSOR_LOW  0x0F

/* Divider column and each pane's column range. Both panes span the full
 * VGA_HEIGHT rows - no title bar, a plain left/right split. */
#define DIVIDER_COL   40
#define DIVIDER_COLOR 0x07 /* light grey on black */

typedef struct {
    int     col_start;
    int     width;
    size_t  row;
    size_t  col;
    uint8_t color;
} console_pane_state_t;

static uint16_t* const vga_buf = VGA_MEMORY;

static console_pane_state_t panes[CONSOLE_PANE_COUNT] = {
    { 0,               DIVIDER_COL,               0, 0, 0x0F }, /* left  */
    { DIVIDER_COL + 1, VGA_WIDTH - DIVIDER_COL - 1, 0, 0, 0x0F }, /* right */
};

/* Which pane the hardware cursor currently tracks (i.e. has keyboard
 * focus). Writes into this pane keep the visible cursor in sync. */
static int hw_cursor_pane = CONSOLE_PANE_LEFT;

/* Create a VGA entry with character and color */
static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Resolve which pane the calling process's console_* calls should target.
 * NULL current_process (pre-process_init() boot code, or interrupt/panic
 * context with no clean process) defaults to the left pane. */
static int current_pane(void) {
    process_t *p = process_current();
    return p ? p->pane : CONSOLE_PANE_LEFT;
}

static void move_hw_cursor(int pane) {
    console_pane_state_t *p = &panes[pane];
    uint16_t pos = (uint16_t)(p->row * VGA_WIDTH + (size_t)p->col_start + p->col);
    outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_HIGH);
    outb(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
    outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LOW);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
}

void console_focus_pane(int pane) {
    hw_cursor_pane = pane;
    move_hw_cursor(pane);
}

/* Scroll one pane up by one line, without touching the divider or the
 * other pane's columns. */
static void pane_scroll(int pane) {
    console_pane_state_t *p = &panes[pane];
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < p->width; x++) {
            size_t col = (size_t)p->col_start + (size_t)x;
            vga_buf[y * VGA_WIDTH + col] = vga_buf[(y + 1) * VGA_WIDTH + col];
        }
    }
    for (int x = 0; x < p->width; x++) {
        size_t col = (size_t)p->col_start + (size_t)x;
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = vga_entry(' ', p->color);
    }
}

static void pane_clear(int pane) {
    console_pane_state_t *p = &panes[pane];
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < p->width; x++) {
            size_t col = (size_t)p->col_start + (size_t)x;
            vga_buf[y * VGA_WIDTH + col] = vga_entry(' ', p->color);
        }
    }
    p->row = 0;
    p->col = 0;
}

static void draw_divider(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        vga_buf[y * VGA_WIDTH + DIVIDER_COL] = vga_entry((char)0xB3, DIVIDER_COLOR);
    }
}

static void pane_backspace(int pane) {
    console_pane_state_t *p = &panes[pane];
    if (p->col > 0) {
        p->col--;
    } else if (p->row > 0) {
        p->row--;
        p->col = (size_t)p->width - 1;
    }
    vga_buf[p->row * VGA_WIDTH + (size_t)p->col_start + p->col] = vga_entry(' ', p->color);
    if (pane == hw_cursor_pane) move_hw_cursor(pane);
}

static void pane_put_char(int pane, char c) {
    /* Mirror every character to the serial port for headless logging,
     * expanding '\n' to CR/LF so terminals render lines correctly. Shared
     * across both panes - it's just a debug log, not part of the split. */
    if (c == '\n') {
        serial_write_char('\r');
    }
    serial_write_char(c);

    console_pane_state_t *p = &panes[pane];

    if (c == '\n') {
        p->col = 0;
        p->row++;
        if (p->row >= VGA_HEIGHT) {
            pane_scroll(pane);
            p->row = VGA_HEIGHT - 1;
        }
        if (pane == hw_cursor_pane) move_hw_cursor(pane);
        return;
    }

    vga_buf[p->row * VGA_WIDTH + (size_t)p->col_start + p->col] = vga_entry(c, p->color);
    p->col++;
    if ((int)p->col >= p->width) {
        p->col = 0;
        p->row++;
        if (p->row >= VGA_HEIGHT) {
            pane_scroll(pane);
            p->row = VGA_HEIGHT - 1;
        }
    }
    if (pane == hw_cursor_pane) move_hw_cursor(pane);
}

/* Initialize console */
void console_init(void) {
    /* Bring up COM1 so all console output is mirrored to the serial port
     * (harmless no-op if no UART is present). */
    serial_init();
    pane_clear(CONSOLE_PANE_LEFT);
    pane_clear(CONSOLE_PANE_RIGHT);
    draw_divider();
    console_focus_pane(CONSOLE_PANE_LEFT);
}

/* Clear the calling process's own pane */
void console_clear(void) {
    pane_clear(current_pane());
}

/* Backspace in the calling process's own pane */
void console_backspace(void) {
    pane_backspace(current_pane());
}

/* Put a character in the calling process's own pane */
void console_put_char(char c) {
    pane_put_char(current_pane(), c);
}

/* Write a string to the calling process's own pane */
void console_write(const char* s) {
    int pane = current_pane();
    for (size_t i = 0; s[i] != '\0'; i++) {
        pane_put_char(pane, s[i]);
    }
}

/* Set the calling process's own pane's color */
void console_set_color(uint8_t fg, uint8_t bg) {
    panes[current_pane()].color = (uint8_t)(fg | (bg << 4));
}

/* Pane-explicit variants, for the keyboard driver's echo */
void console_pane_put_char(int pane, char c) {
    pane_put_char(pane, c);
}

void console_pane_backspace(int pane) {
    pane_backspace(pane);
}

void console_pane_get_cursor(int pane, size_t *row, size_t *col) {
    *row = panes[pane].row;
    *col = panes[pane].col;
}

void console_pane_set_cursor(int pane, size_t row, size_t col) {
    panes[pane].row = row;
    panes[pane].col = col;
    if (pane == hw_cursor_pane) move_hw_cursor(pane);
}

int console_pane_width(int pane) {
    return panes[pane].width;
}

typedef struct {
    const char *name;
    int         value;
} color_name_t;

static const color_name_t color_names[] = {
    { "black",          0 },
    { "blue",           1 },
    { "green",          2 },
    { "cyan",           3 },
    { "red",            4 },
    { "magenta",        5 },
    { "brown",          6 },
    { "light_grey",     7 },
    { "light_gray",     7 },
    { "dark_grey",      8 },
    { "dark_gray",      8 },
    { "light_blue",     9 },
    { "light_green",   10 },
    { "light_cyan",    11 },
    { "light_red",     12 },
    { "light_magenta", 13 },
    { "yellow",        14 },
    { "white",         15 },
};

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

int console_parse_color_name(const char *name) {
    if (!name) return -1;
    size_t n = sizeof(color_names) / sizeof(color_names[0]);
    for (size_t i = 0; i < n; i++) {
        if (str_eq_ci(name, color_names[i].name)) {
            return color_names[i].value;
        }
    }
    return -1;
}

/* Legacy function aliases for backwards compatibility */
void terminal_put_char(char c) {
    console_put_char(c);
}

void terminal_backspace(void) {
    console_backspace();
}
