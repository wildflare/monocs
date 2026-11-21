// select・pselect・poll（2026-11-14）。キー入力（fd 0）・ソケット・ファイルをまとめて待つ。
//   - 読めるか: fd 0 は届いた文字があるか（termios.c の term_input_ready。ICANON でも 1 文字で読めるとする）、
//     ソケットは os/libc/socket.c の socket_ready、ファイルはいつも
//   - 書けるか: fd 1・2 とファイルはいつも、ソケットは socket_ready
//   - どれも用意できていなければ、次の割り込み（受信・キー・タイマー 10ms）まで眠ってから調べ直す（電気を使わない）
//   - 待つ間にシグナルの関数が呼ばれたら -1、EINTR（Linux と同じく、SA_RESTART でも戻る）
//   - select は Linux と同じく、残りの時間を timeout に書く
// 開いていない fd は、select は EBADF、poll は POLLNVAL

#define _DEFAULT_SOURCE
#include <errno.h>
#include "../api/include/poll.h"
#include <signal.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/time.h>
#include "console.h"
#include <string.h>
#include "socket_os.h"
#include "pipe_os.h"
#include "signal_os.h"
#include "termios_os.h"
#include "../drivers/timer.h"
#include "../fs/vfs.h"

typedef struct {
    bool rd, wr, err, hup, bad;
    bool hup_only;      // pipe の終わり: select では読める、poll では POLLHUP だけ（Linux と同じ）
} Ready;

static Ready check(int fd)
{
    Ready r = {0};
    if (socket_ready(fd, &r.rd, &r.wr, &r.err, &r.hup))
        return r;
    if (pipe_ready(fd, &r.rd, &r.wr, &r.hup)) {
        r.hup_only = !r.rd && r.hup;
        r.rd = r.rd || r.hup;
        return r;
    }
    if (fd == 0) {
        r.rd = term_input_ready();
    } else if (fd == 1 || fd == 2) {
        r.wr = true;
    } else if (vfs_fd_open(fd)) {
        r.rd = r.wr = true;
    } else {
        r.bad = true;
    }
    return r;
}

typedef struct {
    uint64_t end;       // 0 = いつまでも
    bool zero;          // 待たない
    uint32_t sigs;
} Deadline;

static Deadline deadline_us(int64_t us)
{
    Deadline d = {.zero = us == 0, .sigs = signal_delivered()};
    if (us > 0)
        d.end = timer_now_us() + (uint64_t)us;
    return d;
}

// まだ待つなら 0（眠ってから戻る）。時間切れなら 1、シグナルなら -1
static int wait_step(Deadline *d)
{
    if (d->zero || (d->end && timer_now_us() >= d->end))
        return 1;
    cpu_idle();
    return signal_delivered() != d->sigs ? -1 : 0;
}

static int64_t left_us(const Deadline *d)
{
    uint64_t now = timer_now_us();
    return d->end && now < d->end ? (int64_t)(d->end - now) : 0;
}

// fd_set の n ビットを読み書きする。アプリの fd_set の大きさ（FD_SETSIZE）はアプリが決める（Monocs が渡す既定は
// Linux と同じ 1024。os/slimcc/run.c の app_defines）ので、カーネルの fd_set の型（picolibc の 64）では扱わない
#define WORD_BITS   (8 * sizeof(unsigned long))
#define SEL_MAX     4096    // 受け付ける n の上限（fd は VFS_MAX_FILES まで。それより先は空でなければ EBADF）

static bool bit_get(const void *set, int fd)
{
    return set && (((const unsigned long *)set)[fd / WORD_BITS] >> (fd % WORD_BITS)) & 1;
}

static void bits_store(void *set, const unsigned long *bits, int n)
{
    if (set)
        memcpy(set, bits, (size_t)(n + WORD_BITS - 1) / WORD_BITS * sizeof(unsigned long));
}

int select(int n, fd_set *restrict rfds, fd_set *restrict wfds, fd_set *restrict efds, struct timeval *restrict tv)
{
    if (n < 0 || n > SEL_MAX) {
        errno = EINVAL;
        return -1;
    }
    Deadline d = deadline_us(tv ? (int64_t)tv->tv_sec * 1000000 + tv->tv_usec : -1);
    // 頼まれたもの（呼んだときの中身。待つ間に書き換えない）
    static_assert(SEL_MAX % 64 == 0, "words");
    unsigned long want_r[SEL_MAX / WORD_BITS], want_w[SEL_MAX / WORD_BITS], want_e[SEL_MAX / WORD_BITS];
    size_t words = (size_t)(n + WORD_BITS - 1) / WORD_BITS;
    memset(want_r, 0, sizeof want_r);
    memset(want_w, 0, sizeof want_w);
    memset(want_e, 0, sizeof want_e);
    if (rfds)
        memcpy(want_r, rfds, words * sizeof(unsigned long));
    if (wfds)
        memcpy(want_w, wfds, words * sizeof(unsigned long));
    if (efds)
        memcpy(want_e, efds, words * sizeof(unsigned long));
    for (;;) {
        int count = 0;
        unsigned long got_r[SEL_MAX / WORD_BITS] = {0}, got_w[SEL_MAX / WORD_BITS] = {0};
        for (int fd = 0; fd < n; fd++) {
            bool r = bit_get(want_r, fd), w = bit_get(want_w, fd), e = bit_get(want_e, fd);
            if (!r && !w && !e)
                continue;
            Ready st = check(fd);
            if (st.bad) {
                errno = EBADF;
                return -1;
            }
            if (r && (st.rd || st.err)) {
                got_r[fd / WORD_BITS] |= 1UL << (fd % WORD_BITS);
                count++;
            }
            if (w && (st.wr || st.err)) {
                got_w[fd / WORD_BITS] |= 1UL << (fd % WORD_BITS);
                count++;
            }
            // 例外（帯域外のデータ）は届かない
        }
        int step = count ? 1 : wait_step(&d);
        if (step) {
            if (tv) {
                int64_t us = left_us(&d);
                tv->tv_sec = us / 1000000;
                tv->tv_usec = us % 1000000;
            }
            if (step < 0 && !count) {
                errno = EINTR;
                return -1;
            }
            static const unsigned long none[SEL_MAX / WORD_BITS];
            bits_store(rfds, got_r, n);
            bits_store(wfds, got_w, n);
            bits_store(efds, none, n);
            return count;
        }
    }
}

int pselect(int n, fd_set *restrict rfds, fd_set *restrict wfds, fd_set *restrict efds,
            const struct timespec *restrict ts, const sigset_t *restrict mask)
{
    sigset_t old;
    if (mask)
        sigprocmask(SIG_SETMASK, mask, &old);
    struct timeval tv, *tvp = nullptr;
    if (ts) {
        tv = (struct timeval){ts->tv_sec, ts->tv_nsec / 1000};
        tvp = &tv;
    }
    int r = select(n, rfds, wfds, efds, tvp);
    if (mask) {
        int e = errno;
        sigprocmask(SIG_SETMASK, &old, nullptr);
        errno = e;
    }
    return r;
}

int poll(struct pollfd fds[], nfds_t n, int timeout_ms)
{
    Deadline d = deadline_us(timeout_ms < 0 ? -1 : (int64_t)timeout_ms * 1000);
    for (;;) {
        int count = 0;
        for (nfds_t i = 0; i < n; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            Ready st = check(fds[i].fd);
            short ev = 0;
            if (st.bad) {
                ev = POLLNVAL;
            } else {
                if ((fds[i].events & (POLLIN | POLLRDNORM)) && st.rd && !st.hup_only)
                    ev |= fds[i].events & (POLLIN | POLLRDNORM);
                if ((fds[i].events & (POLLOUT | POLLWRNORM)) && st.wr)
                    ev |= fds[i].events & (POLLOUT | POLLWRNORM);
                if (st.err)
                    ev |= POLLERR;      // 頼まなくても返す（POSIX）
                if (st.hup)
                    ev |= POLLHUP;      // つないでいない・失敗した TCP（Linux と同じ）
            }
            fds[i].revents = ev;
            if (ev)
                count++;
        }
        int step = count ? 1 : wait_step(&d);
        if (step) {
            if (step < 0 && !count) {
                errno = EINTR;
                return -1;
            }
            return count;
        }
    }
}
