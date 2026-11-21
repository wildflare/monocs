// pipe・pipe2（2026-11-20。QuickJS が poll で待つ間に自分を起こすのに使う self-pipe など、同じプログラムの中の
// pipe）。Monocs にはプロセスがないので、ほかのプログラムへの pipe ではない（シェルの | は一時ファイル）。
// 同じアプリのスレッド（pthread）の間・自分自身で使う。
//   - 輪のバッファ（PIPE_SIZE）に、読む口と書く口の 2 つの fd（ファイル・ソケットと同じ番号の並び。
//     vfs_fd_take_socket で取る）
//   - 待つ（既定）・待たない（O_NONBLOCK: EAGAIN）。書く口がすべて閉じたら読む側は 0（終わり）、読む口がすべて
//     閉じたら書く側は EPIPE と SIGPIPE（Linux と同じ）。PIPE_BUF（4096）以下の書き込みは分かれない
//   - poll・select（os/libc/select.c）、fstat（S_IFIFO）、fcntl（O_NONBLOCK）、ioctl（FIONREAD）
//   - アプリ・サービスが終わったら、その持ち主のものを閉じる（pipe_close_owner）
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pipe_os.h"
#include "../fs/vfs.h"
#include "signal_os.h"
#include "../drivers/timer.h"
#include "../kernel/svc.h"
#include "../kernel/lock.h"
#include "heap.h"

#ifndef PIPE_BUF
#define PIPE_BUF    4096        // Linux と同じ
#endif
#define PIPE_SIZE   65536       // Linux の既定と同じ
#define PIPE_MAX    128         // 同時に開ける pipe（QuickJS の test_rw_handler は 100 個）

typedef struct {
    bool used;
    int owner;
    unsigned char *buf;         // PIPE_SIZE（カーネル用のヒープ）
    size_t head, len;           // 読む位置、たまっている量
    int readers, writers;       // 開いている口の数
} Pipe;

typedef struct {
    int pipe;                   // pipes の番号 + 1（0 = pipe でない）
    bool write_end;
    bool nonblock;
} PipeFd;

static Pipe pipes[PIPE_MAX];
static PipeFd fds[VFS_MAX_FILES];
static struct klock lock = KLOCK_INIT;

static PipeFd *get(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !fds[fd].pipe)
        return nullptr;
    return &fds[fd];
}

bool pipe_is_fd(int fd)
{
    return get(fd) != nullptr;
}

int pipe2(int out[2], int flags)
{
    // O_CLOEXEC などは受け付けるだけ（exec がない）。使うのは O_NONBLOCK だけ
    malloc_kernel_begin();      // アプリの終わりまで持つ（pipe_close_owner が解放する）
    unsigned char *buf = malloc(PIPE_SIZE);
    malloc_kernel_end();
    if (!buf) {
        errno = ENFILE;
        return -1;
    }
    int rfd = vfs_fd_take_socket();
    int wfd = rfd >= 0 ? vfs_fd_take_socket() : -1;
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0)
            vfs_fd_put_socket(rfd);
        free(buf);
        errno = EMFILE;
        return -1;
    }
    klock_acquire(&lock);
    int p = -1;
    for (int i = 0; i < PIPE_MAX && p < 0; i++)
        if (!pipes[i].used)
            p = i;
    if (p < 0) {
        klock_release(&lock);
        vfs_fd_put_socket(rfd);
        vfs_fd_put_socket(wfd);
        free(buf);
        errno = ENFILE;
        return -1;
    }
    pipes[p] = (Pipe){.used = true, .owner = svc_owner(), .buf = buf, .readers = 1, .writers = 1};
    bool nb = (flags & O_NONBLOCK) != 0;
    fds[rfd] = (PipeFd){p + 1, false, nb};
    fds[wfd] = (PipeFd){p + 1, true, nb};
    klock_release(&lock);
    out[0] = rfd;
    out[1] = wfd;
    return 0;
}

int pipe(int out[2])
{
    return pipe2(out, 0);
}

// lock の中で: 口を閉じ、両方とも閉じたら pipe を捨てる（解放するバッファを返す）
static unsigned char *close_locked(int fd)
{
    PipeFd *f = &fds[fd];
    Pipe *p = &pipes[f->pipe - 1];
    if (f->write_end)
        p->writers--;
    else
        p->readers--;
    *f = (PipeFd){0};
    if (p->readers == 0 && p->writers == 0) {
        unsigned char *buf = p->buf;
        *p = (Pipe){0};
        return buf;
    }
    return nullptr;
}

int pipe_close(int fd)
{
    klock_acquire(&lock);
    if (!get(fd)) {
        klock_release(&lock);
        errno = EBADF;
        return -1;
    }
    unsigned char *buf = close_locked(fd);
    klock_release(&lock);
    free(buf);
    vfs_fd_put_socket(fd);
    return 0;
}

// 待つ（次の割り込みまで眠る）。SA_RESTART のないシグナルの関数が呼ばれたら EINTR
static int wait_more(uint32_t sigs)
{
    cpu_idle();
    if (signal_delivered_norestart() != sigs)
        return EINTR;
    return 0;
}

ssize_t pipe_read(int fd, void *buf, size_t len)
{
    uint32_t sigs = signal_delivered_norestart();
    for (;;) {
        klock_acquire(&lock);
        PipeFd *f = get(fd);
        if (!f || f->write_end) {
            klock_release(&lock);
            errno = EBADF;
            return -1;
        }
        Pipe *p = &pipes[f->pipe - 1];
        if (p->len > 0 || len == 0) {
            size_t n = len < p->len ? len : p->len;
            for (size_t done = 0; done < n;) {
                size_t chunk = PIPE_SIZE - p->head < n - done ? PIPE_SIZE - p->head : n - done;
                memcpy((char *)buf + done, p->buf + p->head, chunk);
                p->head = (p->head + chunk) % PIPE_SIZE;
                done += chunk;
            }
            p->len -= n;
            klock_release(&lock);
            return (ssize_t)n;
        }
        bool eof = p->writers == 0, nb = f->nonblock;
        klock_release(&lock);
        if (eof)
            return 0;
        if (nb) {
            errno = EAGAIN;
            return -1;
        }
        int e = wait_more(sigs);
        if (e) {
            errno = e;
            return -1;
        }
    }
}

ssize_t pipe_write(int fd, const void *buf, size_t len)
{
    uint32_t sigs = signal_delivered_norestart();
    size_t done = 0;
    for (;;) {
        klock_acquire(&lock);
        PipeFd *f = get(fd);
        if (!f || !f->write_end) {
            klock_release(&lock);
            errno = EBADF;
            return -1;
        }
        Pipe *p = &pipes[f->pipe - 1];
        if (p->readers == 0) {
            klock_release(&lock);
            raise(SIGPIPE);
            errno = EPIPE;
            return done ? (ssize_t)done : -1;
        }
        size_t room = PIPE_SIZE - p->len;
        // PIPE_BUF 以下は分けずに（入りきらなければ待つ）、それより大きいものは入る分ずつ
        size_t want = len - done;
        size_t n = want <= PIPE_BUF ? (room >= want ? want : 0) : (room < want ? room : want);
        if (n > 0) {
            size_t tail = (p->head + p->len) % PIPE_SIZE;
            for (size_t k = 0; k < n;) {
                size_t chunk = PIPE_SIZE - tail < n - k ? PIPE_SIZE - tail : n - k;
                memcpy(p->buf + tail, (const char *)buf + done + k, chunk);
                tail = (tail + chunk) % PIPE_SIZE;
                k += chunk;
            }
            p->len += n;
            done += n;
        }
        bool nb = f->nonblock;
        klock_release(&lock);
        if (done == len)
            return (ssize_t)done;
        if (nb) {
            if (done)
                return (ssize_t)done;
            errno = EAGAIN;
            return -1;
        }
        int e = wait_more(sigs);
        if (e) {
            if (done)
                return (ssize_t)done;
            errno = e;
            return -1;
        }
    }
}

bool pipe_ready(int fd, bool *readable, bool *writable, bool *hangup)
{
    klock_acquire(&lock);
    PipeFd *f = get(fd);
    if (!f) {
        klock_release(&lock);
        return false;
    }
    Pipe *p = &pipes[f->pipe - 1];
    if (f->write_end) {
        *readable = false;
        *writable = p->readers == 0 || p->len < PIPE_SIZE;
        *hangup = p->readers == 0;
    } else {
        *readable = p->len > 0;     // 空で書く口がないときは hangup だけ（poll は POLLHUP だけ。Linux と同じ）
        *writable = false;
        *hangup = p->writers == 0;
    }
    klock_release(&lock);
    return true;
}

int pipe_get_flags(int fd)
{
    PipeFd *f = get(fd);
    return (f->write_end ? O_WRONLY : O_RDONLY) | (f->nonblock ? O_NONBLOCK : 0);
}

int pipe_set_flags(int fd, int flags)
{
    PipeFd *f = get(fd);
    f->nonblock = (flags & O_NONBLOCK) != 0;
    return 0;
}

size_t pipe_available(int fd)
{
    klock_acquire(&lock);
    PipeFd *f = get(fd);
    size_t n = f && !f->write_end ? pipes[f->pipe - 1].len : 0;
    klock_release(&lock);
    return n;
}

void pipe_fstat(int fd, struct stat *st)
{
    (void)fd;
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFIFO | 0600;
    st->st_blksize = PIPE_BUF;
}

void pipe_close_owner(int owner)
{
    for (int fd = 0; fd < VFS_MAX_FILES; fd++) {
        klock_acquire(&lock);
        bool mine = fds[fd].pipe && pipes[fds[fd].pipe - 1].owner == owner;
        klock_release(&lock);
        if (mine)
            pipe_close(fd);
    }
}
