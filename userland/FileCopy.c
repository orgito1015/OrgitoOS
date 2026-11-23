/*
 * FileCopy.c - kopjon permbajtjen e nje skedari ne nje skedar te ri.
 *
 * Detyre 1 (Sisteme Operative): programi perdor vetem thirrjet e sistemit
 * open(), read(), write() dhe close().
 *
 * I njejti skedar burim kompilohet:
 *   - ne Linux:    gcc -std=c99 -Wall -Wextra FileCopy.c -o FileCopy
 *   - ne OrgitoOS: nga Makefile-i, me -D__ORGITOOS__ (include/ulibc.h jep
 *                  open/read/write/close qe hyjne ne kernel me int 0x80)
 *
 * Hapat:
 *   1. Merr emrat e skedareve (burim, destinacion) dhe kontrollon qe
 *      te dy jane dhene.
 *   2. Hap burimin vetem per lexim (O_RDONLY). Nese deshton (skedari
 *      mungon, s'ka leje leximi...), programi ndalon PARA se te krijoje
 *      destinacionin.
 *   3. Krijon destinacionin me O_WRONLY | O_CREAT | O_EXCL: nese skedari
 *      ekziston, open() deshton me EEXIST dhe permbajtja e tij ruhet.
 *   4. Kopjon me nje buffer: lexon derisa read() kthen 0 (fundi i
 *      skedarit) dhe per cdo bllok shkruan aq bajte sa u lexuan, duke
 *      vazhduar me bajtet e mbetura nese write() shkruan vetem nje pjese.
 *   5. Kontrollon cdo rezultat, mbyll me close() cdo skedar te hapur dhe
 *      shfaq mesazhin e suksesit vetem pasi kopjimi dhe mbyllja kane
 *      perfunduar me sukses.
 */

#ifdef __ORGITOOS__
#include "ulibc.h"
#else
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define BUF_SIZE  4096   /* madhesia e bllokut per read()/write()   */
#define NAME_SIZE 256    /* gjatesia maksimale e emrit te skedarit  */

/* Shkruan te gjithe vargun s ne fd (trajton edhe shkrimet e pjesshme). */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;                 /* u nderpre nga sinjali: provo serish */
            }
            return -1;
        }
        if (w == 0) {                     /* asnje progres: mos rri ne cikel */
            errno = ENOSPC;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

/* Shton vargun s ne fund te dst (pa e tejkaluar madhesine). */
static size_t append(char *dst, size_t len, size_t size, const char *s)
{
    while (*s != '\0' && len < size - 1) {
        dst[len++] = *s++;
    }
    dst[len] = '\0';
    return len;
}

static void print_str(int fd, const char *s)
{
    (void)write_all(fd, s, strlen(s));
}

/* Mesazh gabimi ne stderr, i derguar me nje write():
 * "FileCopy: <mesazhi> '<emri>': <strerror(errno)>" */
static void print_error(const char *what, const char *name)
{
    int    saved = errno;
    char   msg[NAME_SIZE + 160];
    size_t len = 0;

    len = append(msg, len, sizeof(msg), "FileCopy: ");
    len = append(msg, len, sizeof(msg), what);
    if (name != NULL) {
        len = append(msg, len, sizeof(msg), " '");
        len = append(msg, len, sizeof(msg), name);
        len = append(msg, len, sizeof(msg), "'");
    }
    len = append(msg, len, sizeof(msg), ": ");
    len = append(msg, len, sizeof(msg), strerror(saved));
    len = append(msg, len, sizeof(msg), "\n");
    (void)write_all(STDERR_FILENO, msg, len);
}

/*
 * Lexon nje rresht nga hyrja standarde me read(). Perdor nje buffer te
 * perbashket, keshtu qe funksionon si me terminal (nje rresht per cdo
 * read()) ashtu edhe me hyrje te ridrejtuar (disa rreshta ne nje read()).
 * Kthen gjatesine e rreshtit (pa '\n'), ose -1 ne gabim / fund te hyrjes.
 */
static int read_line(const char *prompt, char *out, size_t size)
{
    static char   inbuf[NAME_SIZE * 2];
    static size_t inlen = 0;
    size_t i;

    print_str(STDOUT_FILENO, prompt);

    for (;;) {
        /* A kemi tashme nje rresht te plote ne buffer? */
        for (i = 0; i < inlen; i++) {
            if (inbuf[i] == '\n') {
                size_t n = (i < size - 1) ? i : size - 1;
                size_t k;

                for (k = 0; k < n; k++) {
                    out[k] = inbuf[k];
                }
                out[n] = '\0';
                if (n > 0 && out[n - 1] == '\r') {
                    out[--n] = '\0';
                }
                /* hiq rreshtin e konsumuar nga buffer-i */
                for (k = i + 1; k < inlen; k++) {
                    inbuf[k - (i + 1)] = inbuf[k];
                }
                inlen -= i + 1;
                return (int)n;
            }
        }

        if (inlen == sizeof(inbuf)) {     /* rresht shume i gjate */
            errno = EINVAL;
            return -1;
        }

        ssize_t r = read(STDIN_FILENO, inbuf + inlen, sizeof(inbuf) - inlen);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {                     /* fundi i hyrjes (Ctrl+D) */
            if (inlen == 0) {
                out[0] = '\0';
                return 0;
            }
            inbuf[inlen++] = '\n';        /* rreshti i fundit pa '\n' */
            continue;
        }
        inlen += (size_t)r;
    }
}

int main(void)
{
    char src_name[NAME_SIZE];
    char dst_name[NAME_SIZE];
    static char buf[BUF_SIZE];    /* statik: jo 4 KiB ne stack */
    long total = 0;
    int  src_fd;
    int  dst_fd;
    int  failed = 0;

    /* 1. Merr emrat e skedareve */
    if (read_line("Emri i skedarit burim: ", src_name, sizeof(src_name)) < 0) {
        print_error("gabim gjate leximit te emrit te burimit", NULL);
        return 1;
    }
    if (read_line("Emri i skedarit destinacion: ", dst_name, sizeof(dst_name)) < 0) {
        print_error("gabim gjate leximit te emrit te destinacionit", NULL);
        return 1;
    }
    if (src_name[0] == '\0' || dst_name[0] == '\0') {
        print_str(STDERR_FILENO,
                  "FileCopy: duhen dhene te dy emrat (burimi dhe destinacioni)\n");
        return 1;
    }

    /* 2. Hap burimin per lexim */
    src_fd = open(src_name, O_RDONLY);
    if (src_fd < 0) {
        print_error("nuk mund te hapet burimi", src_name);
        return 1;                         /* destinacioni NUK krijohet */
    }

    /* 3. Krijo destinacionin; deshton nese ekziston (O_EXCL) */
    dst_fd = open(dst_name, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (dst_fd < 0) {
        print_error("nuk mund te krijohet destinacioni", dst_name);
        if (close(src_fd) < 0) {
            print_error("gabim gjate mbylljes se burimit", src_name);
        }
        return 1;
    }

    /* 4. Kopjo te dhenat: read() derisa te ktheje 0 */
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_error("gabim gjate leximit nga", src_name);
            failed = 1;
            break;
        }
        if (n == 0) {
            break;                        /* fundi i skedarit burim */
        }
        /* shkruaj aq bajte sa u lexuan; vazhdo nese shkrimi eshte i pjesshem */
        if (write_all(dst_fd, buf, (size_t)n) < 0) {
            print_error("gabim gjate shkrimit ne", dst_name);
            failed = 1;
            break;
        }
        total += (long)n;
    }

    /* 5. Mbyll te dy skedaret dhe kontrollo rezultatin */
    if (close(src_fd) < 0) {
        print_error("gabim gjate mbylljes se", src_name);
        failed = 1;
    }
    if (close(dst_fd) < 0) {
        print_error("gabim gjate mbylljes se", dst_name);
        failed = 1;
    }
    if (failed) {
        return 1;
    }

    /* Mesazhi i suksesit vetem pas kopjimit dhe mbylljes se suksesshme.
     * Ndertohet i plote ne memorie qe te dergohet me nje write(). */
    {
        char msg[NAME_SIZE * 2 + 96];
        char num[24];
        size_t len = 0;
        int  pos = (int)sizeof(num) - 1;
        long v = total;

        num[pos] = '\0';
        do {
            num[--pos] = (char)('0' + (v % 10));
            v /= 10;
        } while (v > 0 && pos > 0);

        len = append(msg, len, sizeof(msg), "Kopjimi perfundoi me sukses: ");
        len = append(msg, len, sizeof(msg), &num[pos]);
        len = append(msg, len, sizeof(msg), " bajte nga '");
        len = append(msg, len, sizeof(msg), src_name);
        len = append(msg, len, sizeof(msg), "' ne '");
        len = append(msg, len, sizeof(msg), dst_name);
        len = append(msg, len, sizeof(msg), "'.\n");
        (void)write_all(STDOUT_FILENO, msg, len);
    }
    return 0;
}
