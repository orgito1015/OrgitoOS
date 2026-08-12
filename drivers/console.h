/*
 * OrgitoOS - Console Driver
 * 
 * Provides VGA text mode console interface for kernel output.
 */

#ifndef ORGITOOS_DRIVERS_CONSOLE_H
#define ORGITOOS_DRIVERS_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* VGA text mode constants */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

struct multiboot_info;   /* see include/multiboot.h */

/*
 * Initialize console. `mboot` is inspected (via drivers/framebuffer.c) for
 * a usable Multiboot-provided linear framebuffer; if found, console
 * output renders as real pixels through a small bitmap font instead of
 * the legacy VGA text buffer. Every function below keeps its exact
 * existing behavior and signature either way - the backend choice is
 * entirely internal to console.c. Pass NULL to force the VGA text
 * backend (matches the pre-framebuffer behavior exactly).
 */
void console_init(struct multiboot_info *mboot);

/* Clear console */
void console_clear(void);

/* Put a character on the console */
void console_put_char(char c);

/* Write a string to the console */
void console_write(const char* s);

/* Backspace operation */
void console_backspace(void);

/* Set console color */
void console_set_color(uint8_t fg, uint8_t bg);

/*
 * Output capture (used by shell pipe redirection, kernel/shell.c).
 * While active, console_put_char()/console_write() append into `buf`
 * instead of drawing to the VGA buffer or mirroring to serial, so a
 * command's stdout can be collected and fed as another command's stdin.
 * Not reentrant - only one capture may be active at a time, which matches
 * the shell's sequential (non-nested) pipeline execution.
 */
void console_set_capture(char* buf, size_t cap);
size_t console_end_capture(void);

#endif /* ORGITOOS_DRIVERS_CONSOLE_H */
