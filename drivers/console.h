/*
 * OrgitoOS - Console Driver
 *
 * Provides a split-screen VGA text mode console: two independent panes
 * (0=left, 1=right) separated by a vertical divider, each with its own
 * cursor and color. console_write()/console_put_char()/console_backspace()/
 * console_clear()/console_set_color() all target "the calling process's own
 * pane" (process_t.pane, via process_current()) automatically, so existing
 * callers throughout the kernel don't need to know panes exist at all.
 */

#ifndef ORGITOOS_DRIVERS_CONSOLE_H
#define ORGITOOS_DRIVERS_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* VGA text mode constants */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* Number of split-screen panes */
#define CONSOLE_PANE_COUNT 2
#define CONSOLE_PANE_LEFT  0
#define CONSOLE_PANE_RIGHT 1

/* Initialize console: clears both panes, draws the divider, and moves the
 * hardware cursor to the left pane's home cell. */
void console_init(void);

/* Clear the calling process's own pane only (divider and the other pane
 * are left untouched). */
void console_clear(void);

/* Put a character in the calling process's own pane. */
void console_put_char(char c);

/* Write a string to the calling process's own pane. */
void console_write(const char* s);

/* Backspace in the calling process's own pane. */
void console_backspace(void);

/* Set the calling process's own pane's text/background color.
 * fg/bg are 0-15 (standard VGA 16-color palette). */
void console_set_color(uint8_t fg, uint8_t bg);

/* Look up a VGA color name (e.g. "green", "light_grey") -> 0-15.
 * Returns -1 if the name isn't recognized. Case-insensitive. */
int console_parse_color_name(const char *name);

/* Pane-explicit variants: target a specific pane regardless of which
 * process is calling. Used by the keyboard driver, which must echo into
 * whichever pane currently has keyboard focus - not into the pane of
 * whatever process the timer happened to interrupt. */
void console_pane_put_char(int pane, char c);
void console_pane_backspace(int pane);

/* Raw cursor accessors for the given pane, used by the keyboard driver's
 * line editor (Left/Right/Home/End arrow-key cursor movement and mid-line
 * insert/delete need to read back and reposition the pane's cursor
 * directly, not just append/backspace at the end of the line). row/col are
 * in the pane's own local coordinate space (col 0 = pane's leftmost
 * column), matching console_pane_state_t. */
void console_pane_get_cursor(int pane, size_t *row, size_t *col);
void console_pane_set_cursor(int pane, size_t row, size_t col);

/* Width in columns of the given pane, for computing where an offset into
 * the current input line wraps to a new row. */
int console_pane_width(int pane);

/* Move the hardware VGA cursor to the given pane's current position and
 * remember it as the focused pane for cursor-follows-typing purposes.
 * Called by the keyboard driver whenever focus switches (Tab) and after
 * every character typed/erased in the focused pane. */
void console_focus_pane(int pane);

/* Layout: 1 = single 80-column pane (left pane takes the full screen),
 * 0 = default split screen. Clears the screen. */
void console_set_fullscreen(int full);
int console_is_fullscreen(void);

#endif /* ORGITOOS_DRIVERS_CONSOLE_H */
