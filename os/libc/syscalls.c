// picolibc が OS に求める関数（doc/os.md）
// stdin/stdout/stderr は UART につなぐ。ファイル系（open など）はステップ 5 で用意する。

// POSIX の宣言（PATH_MAX など）も見えるようにする
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include "../kernel/smp.h"
#include "../kernel/memory.h"
#include "../kernel/clock.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../kernel/sysreg.h"
#include "../kernel/app.h"
#include "../kernel/svc.h"
#include "../kernel/rexec.h"
#include "../kernel/lock.h"
#include "console.h"
#include "termios_os.h"
#include "../fs/vfs.h"
#include "../kernel/watchdog.h"
#include <fcntl.h>
#include <stdatomic.h>

// ---- コンソール（stdin・stdout・stderr）とリダイレクト ----
// stdout・stdin はシェルのリダイレクト・パイプ（>、>>、<、|。os/shell/shell.c）の間、ファイルに向けられる
// （console_redirect_*）。stderr はいつも画面（Linux と同じく、エラーはリダイレクトしても見える）。
// サービス（コア 3。svc_owner が 0 でない）の出力は向けない。ファイルへはまとめて書く（out_buf）

#define REDIR_BUF   4096

static int out_fd = -1, in_fd = -1, err_fd = -1;
static bool out_null, in_null, err_null, err_to_out;   // /dev/null（捨てる・すぐ終わり）、2>&1
static bool out_rexec, in_rexec, err_rexec;             // ssh の exec の輪（os/kernel/rexec.c。行の土台だけが立てる）
static char out_buf[REDIR_BUF];
static int out_len;
static char in_buf[REDIR_BUF];
static int in_len, in_pos;
// アプリは複数のコアから printf することがある。カーネルのロック（持っている間は Ctrl-C・timeout で止めるのを後回しに
// する。os/kernel/lock.c）にする（2026-11-19。前はただのフラグで、ssh の exec の出力の輪がいっぱいで待つ間に
// timeout・Ctrl-C でアプリが止められると、持ったまま残り、シェルが次の出力で止まった）
static struct klock out_lock = KLOCK_INIT;
#define OUT_LOCK()   klock_acquire(&out_lock)
#define OUT_UNLOCK() klock_release(&out_lock)

static void out_flush_locked(void)
{
    if (out_len > 0 && out_fd >= 0)
        write(out_fd, out_buf, (size_t)out_len);
    out_len = 0;
}

static bool redirected_here(int fd)
{
    return fd >= 0 && svc_owner() <= 0;
}

static int screen_putc(char c)
{
    // Ctrl-S で止められていれば、Ctrl-Q（か Ctrl-C）まで待つ（os/drivers/uart.c）。割り込みを止めている所・
    // サービス（コア 3）では待たない（固まるため）。コア 0 は眠って待つので、Ctrl-C で止められる
    if (uart_output_paused() && svc_owner() <= 0 && !(read_sysreg(daif) & DAIF_I)) {
        while (uart_output_paused())
            cpu_idle();
    }
    if (c == '\n' && term_onlcr())    // termios の OPOST・ONLCR（既定はあり。os/libc/termios.c）
        uart_putc('\r');
    uart_putc(c);
    return (unsigned char)c;
}

static int stdout_putc(char c, FILE *file)
{
    (void)file;
    if (out_null && svc_owner() <= 0)
        return (unsigned char)c;
    if (!redirected_here(out_fd)) {
        if (out_rexec && svc_owner() <= 0) {
            rexec_putc_locked(c, &out_lock);
            return (unsigned char)c;
        }
        return screen_putc(c);
    }
    OUT_LOCK();
    out_buf[out_len++] = c;
    if (out_len == REDIR_BUF)
        out_flush_locked();
    OUT_UNLOCK();
    return (unsigned char)c;
}

static int stderr_putc(char c, FILE *file)
{
    (void)file;
    if (svc_owner() <= 0) {
        if (err_to_out)
            return stdout_putc(c, file);
        if (err_null)
            return (unsigned char)c;
        if (err_fd >= 0) {
            write(err_fd, &c, 1);   // エラーは少ないので、まとめずに書く
            return (unsigned char)c;
        }
        if (err_rexec) {
            rexec_putc_locked(c, &out_lock);
            return (unsigned char)c;
        }
    }
    return screen_putc(c);
}

// 終わりは _FDEV_EOF（picolibc の stdio。EOF（-1）は _FDEV_ERR と同じ値で、読み出しの誤りになる: feof が偽で
// ferror が真になり、Lua の「lua - < FILE」が「cannot read stdin」で止まった。2026-11-20）
static int stdin_getc(FILE *file)
{
    (void)file;
    if (in_null && svc_owner() <= 0)
        return _FDEV_EOF;
    if (!redirected_here(in_fd)) {
        if (in_rexec && svc_owner() <= 0) {
            int c = rexec_getc();
            return c < 0 ? _FDEV_EOF : c;
        }
        int c = term_getc();    // 端末: termios の行の扱い（既定は 1 行ずつ、エコーあり。os/libc/termios.c）
        return c < 0 ? _FDEV_EOF : c;
    }
    if (in_pos == in_len) {
        ssize_t n = read(in_fd, in_buf, sizeof(in_buf));
        if (n < 0)
            return _FDEV_ERR;
        if (n == 0)
            return _FDEV_EOF;
        in_len = (int)n;
        in_pos = 0;
    }
    return (unsigned char)in_buf[in_pos++];
}

int console_write(int fd, const void *buf, size_t count)
{
    const char *p = buf;
    for (size_t i = 0; i < count; i++)
        (fd == 1 ? stdout_putc : stderr_putc)(p[i], nullptr);
    return (int)count;
}

int console_read(int fd, void *buf, size_t count)
{
    (void)fd;
    if (count == 0)
        return 0;
    if (!(in_null && svc_owner() <= 0) && !redirected_here(in_fd) && in_rexec && svc_owner() <= 0) {
        int c = rexec_getc();       // 届くまで待ち、届いている分をまとめて
        if (c < 0)
            return 0;
        char *p = buf;
        p[0] = (char)c;
        return 1 + rexec_read_in(p + 1, (int)(count - 1 < 65535 ? count - 1 : 65535));
    }
    if (!(in_null && svc_owner() <= 0) && !redirected_here(in_fd))
        return term_read(buf, (int)(count < 65536 ? count : 65536));   // 端末: 1 行（ICANON）まとめて
    int c = stdin_getc(nullptr);
    if (c == _FDEV_ERR)
        return -1;
    if (c < 0)
        return 0;
    *(char *)buf = (char)c;
    return 1;
}

static bool is_dev_null(const char *path)
{
    return !strcmp(path, "/dev/null");
}

bool console_redirect_out(const char *path, bool append)
{
    fflush(stdout);
    if (is_dev_null(path)) {
        out_null = true;
        return true;
    }
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0)
        return false;
    vfs_keep_open(fd);  // 動かしたアプリの終わりに閉じない（まとめて書いた分を最後に書き出す）
    out_len = 0;
    out_fd = fd;
    out_null = false;   // 外側（土台）の > /dev/null より、このリダイレクトが勝つ
    return true;
}

bool console_redirect_in(const char *path)
{
    if (is_dev_null(path)) {
        in_null = true;
        return true;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    vfs_keep_open(fd);
    clearerr(stdin);    // 前のプログラムが stdin を終わりまで読んだ印を消す
    in_len = in_pos = 0;
    in_fd = fd;
    in_null = false;
    return true;
}

bool console_redirect_err(const char *path, bool append)
{
    // 外側（土台。system() を呼んだアプリの 2>&1 など）より、このリダイレクトが勝つ（bash と同じく内側が勝つ）。
    // stderr_putc は err_to_out・err_null・err_fd の順に見るので、前のものを外す（2026-11-20、Lua の main.lua:
    // 2>&1 で動かした lua の中の os.execute の「2> FILE」が、外の 2>&1 に負けて FILE が空だった）
    if (!path) {
        err_to_out = true;  // 2>&1
        err_null = false;
        return true;
    }
    if (is_dev_null(path)) {
        err_null = true;
        err_to_out = false;
        return true;
    }
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0)
        return false;
    vfs_keep_open(fd);
    err_fd = fd;
    err_to_out = false;
    err_null = false;
    return true;
}

// 入れ子（アプリの中の system()）: 外側のリダイレクトを「土台」として覚える。子のコマンドはそれを引き継ぎ
// （Linux で、子のプロセスが親のファイルを引き継ぐのと同じ）、console_redirect_end は土台に戻す
// （土台のファイルは閉じない）。console_redirect_pop で外側に戻る
typedef struct {
    int out_fd, in_fd, err_fd;
    bool out_null, in_null, err_null, err_to_out;
    bool out_rexec, in_rexec, err_rexec;
    char in_buf[REDIR_BUF];     // 外側が読み込んで、まだ渡していない分
    int in_len, in_pos;
} Redirect;

#define REDIR_NEST_MAX 8
static Redirect bases[REDIR_NEST_MAX];
static int nbases;

static const Redirect *base(void)
{
    static const Redirect none = {.out_fd = -1, .in_fd = -1, .err_fd = -1};
    return nbases ? &bases[nbases - 1] : &none;
}

bool console_redirect_push(void)
{
    if (nbases == REDIR_NEST_MAX)
        return false;
    fflush(stdout);
    OUT_LOCK();
    out_flush_locked();     // 外側がまとめていた出力を先に書く（子の出力と順が入れ替わらないように）
    OUT_UNLOCK();
    Redirect *b = &bases[nbases++];
    *b = (Redirect){out_fd, in_fd, err_fd, out_null, in_null, err_null, err_to_out, out_rexec, in_rexec, err_rexec, {},
                    in_len, in_pos};
    memcpy(b->in_buf, in_buf, (size_t)in_len);
    return true;
}

void console_redirect_end(void)
{
    fflush(stdout);
    const Redirect *b = base();
    out_null = b->out_null;
    in_null = b->in_null;
    err_null = b->err_null;
    err_to_out = b->err_to_out;
    out_rexec = b->out_rexec;
    in_rexec = b->in_rexec;
    err_rexec = b->err_rexec;
    if (err_fd != b->err_fd) {
        close(err_fd);
        err_fd = b->err_fd;
    }
    if (out_fd != b->out_fd) {
        out_flush_locked();
        close(out_fd);
        out_fd = b->out_fd;
    }
    if (in_fd != b->in_fd) {
        close(in_fd);
        in_fd = b->in_fd;
        in_len = b->in_len;
        in_pos = b->in_pos;
        memcpy(in_buf, b->in_buf, (size_t)in_len);
    }
    clearerr(stdin);
}

void console_redirect_pop(void)
{
    console_redirect_end();
    if (nbases > 0)
        nbases--;
}

bool console_out_is_file(void)
{
    return out_fd >= 0 || out_null || out_rexec;
}

bool console_in_is_file(void)
{
    return in_fd >= 0 || in_null || in_rexec;
}

bool console_err_is_file(void)
{
    if (err_to_out)
        return console_out_is_file();
    return err_fd >= 0 || err_null || err_rexec;
}

// ssh の exec の行: 標準出力・標準エラーを出力の輪に、標準入力を入力の輪に向ける（os/kernel/rexec.c）。
// 行の土台にする（この後で console_redirect_push）。行の中のリダイレクトのほうが勝つ
void console_redirect_rexec(void)
{
    fflush(stdout);
    out_rexec = in_rexec = err_rexec = true;
    clearerr(stdin);
}

// コンソールにはロックを付けない（getchar がキー入力を待つ間、ロックを持ち続けないように。
// コアどうしの出力は文字の単位で混ざりうる）。1 は picolibc の __LOCK_NONE（libc/stdio/local-stdio.h）
#ifdef __STDIO_LOCKING
#define CONSOLE_LOCK .lock = (_LOCK_RECURSIVE_T)(uintptr_t)1,
#else
#define CONSOLE_LOCK
#endif
static FILE console_in = {.flags = __SRD, .get = stdin_getc, CONSOLE_LOCK};
static FILE console_out = {.flags = __SWR, .put = stdout_putc, CONSOLE_LOCK};
static FILE console_err = {.flags = __SWR, .put = stderr_putc, CONSOLE_LOCK};

FILE *const stdin = &console_in;
FILE *const stdout = &console_out;
FILE *const stderr = &console_err;

// exit/abort の行き先。アプリの実行中ならシェルへ戻る
void _exit(int status)
{
    app_exit(status);
    uart_puts("\n*** _exit(");
    uart_putdec((unsigned)status);
    uart_puts(")");
    kernel_halt("");    // 数秒後に再起動する（os/kernel/watchdog.c）
}

// 環境変数（getenv が参照する）。HOME は /root（Linux の root ユーザーと同じ。アプリが ~/.local などに設定を置く）。
// TZ はタイムゾーン（os/kernel/clock.c。date -z や nos.py が設定する。localtime が使う）
// PATH はアプリを探す場所（os/shell/shell.c。/root/apps は自分のアプリ、/usr/bin は Monocs に付いてくるもの）。
// LANG は、アプリの setlocale(LC_ALL, "") が選ぶロケール（Monocs の文字はいつも UTF-8。os/libc/posix.c）。
// TERM は端末の型（2026-11-20、vim）。Monocs の端末はどれも xterm と同じに扱う（os/curses/curses.c、os/libc/termcap.c）。
// Linux では ssh・端末ソフトが渡すもの
// export で変えると picolibc の setenv が environ を作り直す
static char *default_environ[] = { "HOME=/root", "PATH=/root/apps:/usr/bin", "LANG=C.UTF-8", "TERM=xterm-256color",
                                   clock_tz_string, nullptr };
char **environ = default_environ;


// 日時は os/kernel/clock.c（設定されるまでは、起動を 2026-01-01 00:00:00 UTC として数える）
int gettimeofday(struct timeval *restrict tv, void *restrict tz)
{
    (void)tz;
    int64_t us = clock_realtime_us();
    tv->tv_sec = us / 1000000;
    tv->tv_usec = us % 1000000;
    return 0;
}

// clock() から呼ばれる。picolibc の CLOCKS_PER_SEC は 1000000 なので、マイクロ秒で返す
static_assert(CLOCKS_PER_SEC == 1000000);

clock_t times(struct tms *buf)
{
    clock_t t = (clock_t)timer_now_us();
    buf->tms_utime = t;
    buf->tms_stime = 0;
    buf->tms_cutime = 0;
    buf->tms_cstime = 0;
    return t;
}

// _SC_NPROCESSORS_CONF・_SC_NPROCESSORS_ONLN・_SC_PHYS_PAGES・_SC_AVPHYS_PAGES は picolibc にないので、アプリには
// os/slimcc/run.c が定義を渡す（値は picolibc の _SC_* と重ならない 1000 番台）
#define MONOCS_SC_NPROCESSORS_CONF 1000
#define MONOCS_SC_NPROCESSORS_ONLN 1001
#define MONOCS_SC_PHYS_PAGES       1002
#define MONOCS_SC_AVPHYS_PAGES     1003

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:  return 4096;
    case _SC_CLK_TCK:   return 1000000;     // times() はマイクロ秒
    case _SC_OPEN_MAX:  return VFS_MAX_FILES;   // fd 0〜255（ファイル・ソケット・pipe で分け合う。os/fs/vfs.h）
    case MONOCS_SC_NPROCESSORS_CONF: return SMP_MAX_CORES;
    case MONOCS_SC_NPROCESSORS_ONLN: return smp_cores();  // アプリが使えるコア（コア 0 を含む。サービスのコアを除く）
    case MONOCS_SC_PHYS_PAGES:  return (long)((memory_heap_end() - memory_heap_start()) / 4096);
    case MONOCS_SC_AVPHYS_PAGES: return (long)((memory_heap_end() - memory_heap_start()) / 4096);
    default:            errno = EINVAL; return -1;
    }
}

// realpath は os/libc/posix.c
