/*
 * OrgitoOS - Keyboard Driver
 */

#ifndef ORGITOOS_DRIVERS_KEYBOARD_H
#define ORGITOOS_DRIVERS_KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

/* Keyboard I/O port */
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Initialize keyboard */
void keyboard_init(void);

/* Keyboard interrupt handler (called from ISR) */
void keyboard_handler(void);

/* Get a line of input (blocking). Targets the calling process's own pane
 * (process_t.pane): if that pane doesn't currently have keyboard focus,
 * this simply keeps blocking until the user switches focus to it (Tab)
 * and types a line - see drivers/keyboard.c. */
void keyboard_get_line(char* buffer, size_t max_len);

#endif /* ORGITOOS_DRIVERS_KEYBOARD_H */
