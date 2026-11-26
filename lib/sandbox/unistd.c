// サンドボックスの中の read・write・端末の設定（termios の ICANON・ECHO・VMIN・VTIME だけ）・画面の大きさ・poll。
// カーネルは生のバイトを返すだけなので、行の編集（打った文字の表示、Backspace、Ctrl-U、Enter、行の頭の Ctrl-D）はここで。
// Ctrl-C はいつもサンドボックスを止める（キーとしては届かない）
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "sandbox_sys.h"

static struct termios tio = { .c_lflag = ICANON | ECHO, .c_cc = { [VMIN] = 1, [VTIME] = 0 } };
static int pending = -1;            // poll が先に読んだ 1 バイト
static char line[512];              // ICANON のときの、渡し終えていない 1 行
static size_t line_len, line_pos;

ssize_t write(int fd, const void *buf, size_t n)
{
    return sb_sys(SB_SYS_WRITE, fd, (long)buf, (long)n);
}

static void echo(const char *s, size_t n)
{
    if (tio.c_lflag & ECHO)
        write(1, s, n);
}

// 生のバイトを 1 つ（timeout_ms 負ならいつまでも）。時間切れは -1
static int raw_byte(long timeout_ms)
{
    if (pending >= 0) {
        int c = pending;
        pending = -1;
        return c;
    }
    unsigned char c;
    return sb_sys4(SB_SYS_READ, 0, (long)&c, 1, timeout_ms) == 1 ? c : -1;
}

ssize_t read(int fd, void *buf, size_t n)
{
    if (fd != 0 || n == 0)
        return fd == 0 ? 0 : -1;
    char *out = buf;
    if (!(tio.c_lflag & ICANON)) {      // 1 バイトずつ（VMIN 0 なら VTIME × 0.1 秒まで待つ）
        long t = tio.c_cc[VMIN] ? -1 : (long)tio.c_cc[VTIME] * 100;
        int c = raw_byte(t);
        if (c < 0)
            return 0;
        out[0] = (char)c;
        size_t k = 1;
        while (k < n && (c = raw_byte(0)) >= 0)     // 続けて届いているものもまとめて
            out[k++] = (char)c;
        echo(out, k);
        return (ssize_t)k;
    }
    while (line_pos == line_len) {      // 1 行を作る
        line_len = line_pos = 0;
        for (;;) {
            int c = raw_byte(-1);
            if (c == '\r')
                c = '\n';
            if (c == 4 && line_len == 0)    // 行の頭の Ctrl-D: 終わり
                return 0;
            if (c == 127 || c == 8) {
                if (line_len) {
                    line_len--;
                    echo("\b \b", 3);
                }
                continue;
            }
            if (c == 21) {                  // Ctrl-U
                while (line_len) {
                    line_len--;
                    echo("\b \b", 3);
                }
                continue;
            }
            if (c < 0 || (c < 32 && c != '\n' && c != '\t'))
                continue;
            if (line_len < sizeof line - 1 || c == '\n')
                line[line_len++] = (char)c;
            echo((char *)&(char){(char)c}, 1);
            if (c == '\n')
                break;
        }
    }
    size_t k = line_len - line_pos < n ? line_len - line_pos : n;
    memcpy(out, line + line_pos, k);
    line_pos += k;
    return (ssize_t)k;
}

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}

int tcgetattr(int fd, struct termios *t)
{
    (void)fd;
    *t = tio;
    return 0;
}

int tcsetattr(int fd, int act, const struct termios *t)
{
    (void)fd, (void)act;
    tio = *t;
    return 0;
}

int ioctl(int fd, unsigned long op, ...)
{
    (void)fd;
    if (op != TIOCGWINSZ)
        return -1;
    va_list ap;
    va_start(ap, op);
    struct winsize *ws = va_arg(ap, struct winsize *);
    va_end(ap);
    unsigned long v = (unsigned long)sb_sys(SB_SYS_TERMSIZE, 0, 0, 0);
    ws->ws_row = (unsigned short)(v >> 32);
    ws->ws_col = (unsigned short)v;
    ws->ws_xpixel = ws->ws_ypixel = 0;
    return 0;
}

// 標準入力だけ（ほかの fd は「何もない」）
int poll(struct pollfd fds[], nfds_t n, int timeout_ms)
{
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd == 0 && (fds[i].events & POLLIN)) {
            if (pending < 0 && line_pos == line_len) {
                int c = raw_byte(timeout_ms < 0 ? -1 : timeout_ms);
                if (c >= 0)
                    pending = c;
            }
            if (pending >= 0 || line_pos < line_len) {
                fds[i].revents = POLLIN;
                ready++;
            }
        }
    }
    return ready;
}
