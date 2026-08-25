/*
 * OrgitoCompiler - freestanding-kernel platform shim implementation
 * See oc_platform.h for why each of these exists.
 */

#include "oc_platform.h"

void *g_oc_jmp_buf[5];

void *oc_realloc(void *ptr, size_t old_size, size_t new_size) {
    if (new_size == 0) {
        oc_free(ptr);
        return NULL;
    }
    void *n = oc_malloc(new_size);
    if (!n) return NULL;
    if (ptr) {
        size_t copy = old_size < new_size ? old_size : new_size;
        memcpy(n, ptr, copy);
        oc_free(ptr);
    }
    return n;
}

/* Print an unsigned 64-bit value in decimal. */
static void print_u64(unsigned long long v) {
    char buf[21]; /* max 20 digits for 2^64-1 + NUL */
    int i = 20;
    buf[i] = '\0';
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v > 0 && i > 0) {
            buf[--i] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    console_write(&buf[i]);
}

static void print_i64(long long v) {
    if (v < 0) {
        console_put_char('-');
        /* Avoid negating LLONG_MIN (UB): widen via unsigned two's complement. */
        print_u64((unsigned long long)(-(v + 1)) + 1ULL);
    } else {
        print_u64((unsigned long long)v);
    }
}

void oc_eprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            console_put_char(fmt[i]);
            continue;
        }
        i++;
        if (fmt[i] == '\0') break;

        /* Only length modifier actually used: "ll" before 'd'. */
        int is_long_long = 0;
        if (fmt[i] == 'l' && fmt[i + 1] == 'l') {
            is_long_long = 1;
            i += 2;
        }

        switch (fmt[i]) {
            case 'd': {
                if (is_long_long) {
                    print_i64((long long)va_arg(ap, long long));
                } else {
                    print_i64((long long)va_arg(ap, int));
                }
                break;
            }
            case 'z': {
                /* %zu */
                if (fmt[i + 1] == 'u') i++;
                print_u64((unsigned long long)va_arg(ap, size_t));
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                console_write(s ? s : "(null)");
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                console_put_char(c);
                break;
            }
            case '%':
                console_put_char('%');
                break;
            default:
                console_put_char('%');
                console_put_char(fmt[i]);
                break;
        }
    }

    va_end(ap);
}
