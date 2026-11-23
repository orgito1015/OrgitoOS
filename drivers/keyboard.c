/*
 * OrgitoOS - Keyboard Driver Implementation
 */

#include "keyboard.h"
#include "console.h"
#include "../arch/x86/pic.h"
#include "../arch/x86/ports.h"
#include "../process/process.h"
#include <stdint.h>
#include <stddef.h>

/* Tab (scan code set 1) toggles which split-screen pane has keyboard
 * focus, instead of being typed as a character - see keyboard_handler(). */
#define SCANCODE_TAB 0x0F

/* Dedicated arrow/navigation keys (as opposed to the numpad digits that
 * share these same code values) are sent as a 0xE0 prefix byte followed by
 * one of these "make" codes - see the extended_prefix handling below. */
#define SCANCODE_EXTENDED 0xE0
#define SCANCODE_HOME     0x47
#define SCANCODE_UP       0x48
#define SCANCODE_LEFT     0x4B
#define SCANCODE_END      0x4F
#define SCANCODE_RIGHT    0x4D
#define SCANCODE_DOWN     0x50
#define SCANCODE_DELETE   0x53

/* US QWERTY scan code to ASCII translation table (Set 1) */
static const char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, /* Left Ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, /* Left Shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 
    0, /* Right Shift */
    '*',
    0, /* Alt */
    ' ', /* Space */
    0, /* Caps Lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F10 */
    0, /* Num Lock */
    0, /* Scroll Lock */
    0, /* Home */
    0, /* Up Arrow */
    0, /* Page Up */
    '-',
    0, /* Left Arrow */
    0,
    0, /* Right Arrow */
    '+',
    0, /* End */
    0, /* Down Arrow */
    0, /* Page Down */
    0, /* Insert */
    0, /* Delete */
    0, 0, 0,
    0, 0, /* F11, F12 */
    0, /* Rest are undefined */
};

/* Shifted characters */
static const char scancode_to_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, /* Left Ctrl */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, /* Left Shift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 
    0, /* Right Shift */
    '*',
    0, /* Alt */
    ' ', /* Space */
    0, /* Caps Lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F10 */
    0, /* Num Lock */
    0, /* Scroll Lock */
    0, /* Home */
    0, /* Up Arrow */
    0, /* Page Up */
    '-',
    0, /* Left Arrow */
    0,
    0, /* Right Arrow */
    '+',
    0, /* End */
    0, /* Down Arrow */
    0, /* Page Down */
    0, /* Insert */
    0, /* Delete */
    0, 0, 0,
    0, 0, /* F11, F12 */
    0, /* Rest are undefined */
};

/* Keyboard state */
static uint8_t shift_pressed = 0;
static uint8_t caps_lock = 0;

/* Which pane currently receives typed characters. Toggled by Tab. */
static int focused_pane = CONSOLE_PANE_LEFT;

/* One input line buffer per pane, so each split-screen shell has its own
 * independent line in progress; only the focused pane's buffer is ever
 * appended to by keyboard_handler(). input_buffer_len is the line's total
 * length (previously the only "position" tracked, when the cursor could
 * only ever be at the end); input_cursor is where edits happen, which may
 * now be anywhere in [0, input_buffer_len] thanks to Left/Right/Home/End.
 * line_start_row/col is the pane cursor position where this line began
 * (captured once in keyboard_get_line(), right after the prompt was
 * printed), used to compute where cursor offset N lands on screen. */
#define INPUT_BUFFER_SIZE 256
static char input_buffer[CONSOLE_PANE_COUNT][INPUT_BUFFER_SIZE];
static volatile size_t input_buffer_len[CONSOLE_PANE_COUNT] = { 0 };
static volatile size_t input_cursor[CONSOLE_PANE_COUNT] = { 0 };
static size_t line_start_row[CONSOLE_PANE_COUNT] = { 0 };
static size_t line_start_col[CONSOLE_PANE_COUNT] = { 0 };
static volatile uint8_t line_ready[CONSOLE_PANE_COUNT] = { 0 };

/* Per-pane command history, browsed with Up/Down like a normal shell.
 * history_count is how many entries are stored (capped at HISTORY_SIZE,
 * oldest dropped first once full); history_pos is where Up/Down browsing
 * currently sits, in [0, history_count] - history_pos == history_count
 * means "not browsing", i.e. back at the live, currently-typed line. */
#define HISTORY_SIZE 16
static char history[CONSOLE_PANE_COUNT][HISTORY_SIZE][INPUT_BUFFER_SIZE];
static int history_count[CONSOLE_PANE_COUNT] = { 0 };
static int history_pos[CONSOLE_PANE_COUNT] = { 0 };

/* Move pane's on-screen cursor to reflect input_cursor[pane] characters
 * past line_start_row/col[pane], wrapping at the pane's width exactly like
 * pane_put_char() does. Doesn't handle the line wrapping past the bottom
 * of the screen (which would require scrolling line_start itself) - a
 * pre-existing limitation shared with plain backspace-at-row-0, not
 * something this line editor introduces. */
static void sync_cursor_display(int pane) {
    int width = console_pane_width(pane);
    size_t total = line_start_col[pane] + input_cursor[pane];
    size_t row = line_start_row[pane] + total / (size_t)width;
    size_t col = total % (size_t)width;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    console_pane_set_cursor(pane, row, col);
}

/* Redraw input_buffer[pane][from .. input_buffer_len-1] starting at the
 * screen position for offset `from`, then erase `extra_clear` trailing
 * cells (used when the line just got shorter, to wipe the old tail), and
 * finally restore the screen cursor to input_cursor[pane]. Used after any
 * mid-line insert/delete, since those shift every character after the
 * edit point. */
static void redraw_tail(int pane, size_t from, size_t extra_clear) {
    int width = console_pane_width(pane);
    size_t total = line_start_col[pane] + from;
    size_t row = line_start_row[pane] + total / (size_t)width;
    size_t col = total % (size_t)width;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    console_pane_set_cursor(pane, row, col);

    for (size_t i = from; i < input_buffer_len[pane]; i++) {
        console_pane_put_char(pane, input_buffer[pane][i]);
    }
    for (size_t i = 0; i < extra_clear; i++) {
        console_pane_put_char(pane, ' ');
    }

    sync_cursor_display(pane);
}

/* Append a just-submitted line to pane's history (called from the Enter
 * handler, before the buffer is reset for the next line). Empty lines and
 * exact repeats of the most recent entry aren't stored, matching typical
 * shell history behavior. Once HISTORY_SIZE is reached, the oldest entry
 * is dropped to make room. Always leaves history_pos[pane] at
 * history_count[pane] (the "not browsing" position), so the next Up press
 * starts from the most recent command. */
static void history_push(int pane, const char *line, size_t len) {
    if (len == 0) {
        history_pos[pane] = history_count[pane];
        return;
    }

    if (history_count[pane] > 0) {
        const char *last = history[pane][history_count[pane] - 1];
        size_t i = 0;
        while (i < len && last[i] == line[i]) i++;
        if (i == len && last[len] == '\0') {
            history_pos[pane] = history_count[pane];
            return;
        }
    }

    int slot;
    if (history_count[pane] < HISTORY_SIZE) {
        slot = history_count[pane];
        history_count[pane]++;
    } else {
        for (int i = 1; i < HISTORY_SIZE; i++) {
            for (size_t j = 0; j < INPUT_BUFFER_SIZE; j++) {
                history[pane][i - 1][j] = history[pane][i][j];
            }
        }
        slot = HISTORY_SIZE - 1;
    }

    size_t i;
    for (i = 0; i < len && i < INPUT_BUFFER_SIZE - 1; i++) {
        history[pane][slot][i] = line[i];
    }
    history[pane][slot][i] = '\0';

    history_pos[pane] = history_count[pane];
}

/* Replace the in-progress input line with pane's history entry `idx` and
 * redraw it in place, reusing redraw_tail()'s "from offset 0" redraw plus
 * however many trailing cells need clearing if the new line is shorter
 * than what was on screen before. */
static void history_load(int pane, int idx) {
    size_t old_len = input_buffer_len[pane];
    const char *src = history[pane][idx];

    size_t len = 0;
    while (src[len] != '\0' && len < INPUT_BUFFER_SIZE - 1) len++;
    for (size_t i = 0; i < len; i++) input_buffer[pane][i] = src[i];

    input_buffer_len[pane] = len;
    input_cursor[pane] = len;

    size_t extra_clear = (old_len > len) ? (old_len - len) : 0;
    redraw_tail(pane, 0, extra_clear);
}

/* Clear the input line back to empty (used when Down browses past the
 * newest history entry, back to the live line). */
static void history_clear_line(int pane) {
    size_t old_len = input_buffer_len[pane];
    input_buffer_len[pane] = 0;
    input_cursor[pane] = 0;
    redraw_tail(pane, 0, old_len);
}

/* Initialize keyboard */
void keyboard_init(void) {
    /* Enable keyboard interrupt (IRQ1) */
    uint8_t mask = inb(PIC1_DATA);
    mask &= ~(1 << 1);  /* Clear bit 1 to enable IRQ1 */
    outb(PIC1_DATA, mask);
}

/* Dedicated arrow/Home/End/Delete keys send 0xE0 as a prefix byte,
 * followed by the actual code - normally as two separate IRQ1 interrupts.
 * Some hypervisors' virtual keyboard controllers deliver those two bytes
 * close enough together that the second IRQ1 edge can be missed (the PIC
 * line doesn't have time to re-arm before this ISR returns), silently
 * dropping the follow-up byte - this reportedly happens in VirtualBox
 * even though QEMU's timing doesn't trigger it. Polling the status port
 * for the second byte within the *same* interrupt sidesteps needing a
 * second IRQ1 edge at all, so it works regardless of hypervisor timing. */
static uint8_t keyboard_read_next_byte(void) {
    for (int spins = 0; spins < 100000; spins++) {
        if (inb(KEYBOARD_STATUS_PORT) & 0x01) {
            return inb(KEYBOARD_DATA_PORT);
        }
    }
    return 0; /* Second byte never showed up - treat as a no-op key. */
}

/* Keyboard interrupt handler */
void keyboard_handler(void) {
    /* Read scan code from keyboard */
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode == SCANCODE_EXTENDED) {
        scancode = keyboard_read_next_byte();

        /* Only the make (press) codes matter for these keys - ignore the
         * paired 0xE0 <code>|0x80 release byte that follows a press. */
        if (!(scancode & 0x80)) {
            int pane = focused_pane;
            switch (scancode) {
                case SCANCODE_LEFT:
                    if (input_cursor[pane] > 0) { input_cursor[pane]--; sync_cursor_display(pane); }
                    break;
                case SCANCODE_RIGHT:
                    if (input_cursor[pane] < input_buffer_len[pane]) { input_cursor[pane]++; sync_cursor_display(pane); }
                    break;
                case SCANCODE_UP:
                    /* Step to an older history entry, if one exists. */
                    if (history_pos[pane] > 0) {
                        history_pos[pane]--;
                        history_load(pane, history_pos[pane]);
                    }
                    break;
                case SCANCODE_DOWN:
                    /* Step to a newer history entry, or back to the live
                     * (empty) line once past the newest one. */
                    if (history_pos[pane] < history_count[pane]) {
                        history_pos[pane]++;
                        if (history_pos[pane] == history_count[pane]) {
                            history_clear_line(pane);
                        } else {
                            history_load(pane, history_pos[pane]);
                        }
                    }
                    break;
                case SCANCODE_HOME:
                    input_cursor[pane] = 0;
                    sync_cursor_display(pane);
                    break;
                case SCANCODE_END:
                    input_cursor[pane] = input_buffer_len[pane];
                    sync_cursor_display(pane);
                    break;
                case SCANCODE_DELETE: {
                    /* Delete-forward: remove the character at the cursor
                     * (not before it, like Backspace), shifting the tail
                     * left and redrawing it. */
                    size_t at = input_cursor[pane];
                    if (at < input_buffer_len[pane]) {
                        for (size_t i = at; i + 1 < input_buffer_len[pane]; i++) {
                            input_buffer[pane][i] = input_buffer[pane][i + 1];
                        }
                        input_buffer_len[pane]--;
                        redraw_tail(pane, at, 1);
                    }
                    break;
                }
                default:
                    /* Unhandled extended key (e.g. numpad-adjacent codes,
                     * PageUp/PageDown) - ignore. */
                    break;
            }
        }
        pic_send_eoi(1);
        return;
    }

    /* Check if it's a break code (key release) */
    if (scancode & 0x80) {
        /* Key released */
        scancode &= 0x7F;  /* Remove break bit */

        /* Check for shift key release */
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = 0;
        }
    } else {
        /* Key pressed */

        /* Check for shift key press */
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = 1;
        }
        /* Check for caps lock */
        else if (scancode == 0x3A) {
            caps_lock = !caps_lock;
        }
        /* Tab switches which split-screen pane has keyboard focus,
         * instead of being typed - the unfocused pane's shell just keeps
         * blocking in keyboard_get_line() until focus comes back to it. */
        else if (scancode == SCANCODE_TAB) {
            /* In full-width layout there is only one visible pane. */
            if (!console_is_fullscreen()) {
                focused_pane = (focused_pane == CONSOLE_PANE_LEFT)
                              ? CONSOLE_PANE_RIGHT : CONSOLE_PANE_LEFT;
                console_focus_pane(focused_pane);
            }
        }
        /* Regular key press */
        else {
            char ascii = 0;

            /* Validate scancode to prevent out-of-bounds access */
            if (scancode >= 128) {
                /* Invalid scancode, ignore */
                pic_send_eoi(1);
                return;
            }

            /* Get ASCII character */
            if (shift_pressed) {
                ascii = scancode_to_ascii_shift[scancode];
            } else {
                ascii = scancode_to_ascii[scancode];
            }

            /* Apply caps lock for letters (only when shift is not pressed) */
            if (caps_lock && !shift_pressed) {
                if (ascii >= 'a' && ascii <= 'z') {
                    ascii -= 32;  /* Convert to uppercase */
                }
            }

            /* Handle special keys - all state/echo below targets whichever
             * pane currently has focus. */
            if (ascii == '\b') {
                /* Backspace: delete the character *before* the cursor, not
                 * necessarily the last character in the line - shift the
                 * tail left one slot and redraw it. */
                int pane = focused_pane;
                if (input_cursor[pane] > 0) {
                    for (size_t i = input_cursor[pane] - 1; i + 1 < input_buffer_len[pane]; i++) {
                        input_buffer[pane][i] = input_buffer[pane][i + 1];
                    }
                    input_buffer_len[pane]--;
                    input_cursor[pane]--;
                    redraw_tail(pane, input_cursor[pane], 1);
                }
            } else if (ascii == '\n') {
                /* Enter submits the whole line regardless of where the
                 * cursor was left - move the screen cursor to the end of
                 * the line first so the newline lands after all of it. */
                int pane = focused_pane;
                input_cursor[pane] = input_buffer_len[pane];
                sync_cursor_display(pane);
                console_pane_put_char(pane, '\n');
                input_buffer[pane][input_buffer_len[pane]] = '\0';
                history_push(pane, input_buffer[pane], input_buffer_len[pane]);
                line_ready[pane] = 1;
            } else if (ascii != 0) {
                /* Regular character: insert at the cursor, shifting
                 * anything after it right, then redraw the (now one
                 * longer) tail. */
                int pane = focused_pane;
                if (input_buffer_len[pane] < INPUT_BUFFER_SIZE - 1) {
                    for (size_t i = input_buffer_len[pane]; i > input_cursor[pane]; i--) {
                        input_buffer[pane][i] = input_buffer[pane][i - 1];
                    }
                    input_buffer[pane][input_cursor[pane]] = ascii;
                    input_buffer_len[pane]++;
                    input_cursor[pane]++;
                    redraw_tail(pane, input_cursor[pane] - 1, 0);
                }
            }
        }
    }

    /* Send EOI to PIC */
    pic_send_eoi(1);
}

/* Get a line of input (blocking), targeting the calling process's own pane */
void keyboard_get_line(char* buffer, size_t max_len) {
    /* Validate parameters */
    if (buffer == NULL || max_len == 0) {
        return;
    }

    process_t *p = process_current();
    int pane = p ? p->pane : CONSOLE_PANE_LEFT;

    /* Reset this pane's buffer - disable interrupts to prevent race
     * condition with keyboard_handler(). Also capture where this line
     * starts on screen (right after whatever prompt the caller already
     * printed), so the line editor can compute cursor positions for
     * Left/Right/Home/End relative to it. */
    __asm__ __volatile__("cli");
    input_buffer_len[pane] = 0;
    input_cursor[pane] = 0;
    line_ready[pane] = 0;
    console_pane_get_cursor(pane, &line_start_row[pane], &line_start_col[pane]);
    __asm__ __volatile__("sti");

    /* Wait for this pane's line to be ready (interrupts must be enabled).
     * If `pane` doesn't currently have keyboard focus, keyboard_handler()
     * never sets line_ready[pane], so this simply blocks until the user
     * switches focus here (Tab) and types a line - by design. */
    while (!line_ready[pane]) {
        __asm__ __volatile__("hlt");
    }

    /* Copy to output buffer */
    size_t i;
    for (i = 0; i < input_buffer_len[pane] && i < max_len - 1; i++) {
        buffer[i] = input_buffer[pane][i];
    }
    buffer[i] = '\0';
}
