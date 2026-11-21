// mmap・munmap・msync・mprotect・madvise（sys/mman.h。2026-11-20、SQLite の WAL の共有メモリ）
//
// Monocs はアドレス空間が 1 つで、アプリごとのページ表がない。mmap は、ページの境界にそろえたメモリを確保して返す:
//   - MAP_ANONYMOUS: 0 で埋めたメモリ
//   - ファイル: 確保したメモリにファイルの中身を読む（ファイルより先は 0）。MAP_SHARED で PROT_WRITE なら、msync・
//     munmap のときにファイルへ書き戻す（Linux のように書いたそばからファイルに見えるわけではない。同じところを
//     2 回 mmap すると別々のメモリになる）
// 確保はふつうの malloc なので、アプリが終わると（munmap しなくても）アプリ用のヒープごと消える。MAP_FIXED はない
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../kernel/svc.h"
#include "../kernel/lock.h"

#define MMAP_MAX  64
#define PAGE_SIZE 4096

typedef struct {
    void *addr;
    size_t len;
    int fd;             // ファイルなら fd、MAP_ANONYMOUS なら -1
    off_t off;
    bool writeback;     // MAP_SHARED で PROT_WRITE（msync・munmap で書き戻す）
    int owner;          // svc_owner()（ファイルと同じ持ち主。持ち主が終わったら mman_close_owner で外す）
} Map;

static Map maps[MMAP_MAX];
static struct klock mman_lock;

// fd の位置を変えずに、off から読む・書く（pread・pwrite と同じ）
static ssize_t rw_at(int fd, void *buf, size_t n, off_t off, bool write_it)
{
    off_t save = lseek(fd, 0, SEEK_CUR);
    if (save < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -1;
    size_t done = 0;
    while (done < n) {
        ssize_t r = write_it ? write(fd, (char *)buf + done, n - done) : read(fd, (char *)buf + done, n - done);
        if (r < 0) {
            int e = errno;
            lseek(fd, save, SEEK_SET);
            errno = e;
            return -1;
        }
        if (r == 0)
            break;  // ファイルの終わり
        done += (size_t)r;
    }
    lseek(fd, save, SEEK_SET);
    return (ssize_t)done;
}

static Map *find_map(void *addr)
{
    for (int i = 0; i < MMAP_MAX; i++)
        if (maps[i].addr && (char *)addr >= (char *)maps[i].addr &&
            (char *)addr < (char *)maps[i].addr + maps[i].len)
            return &maps[i];
    return nullptr;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr;     // 場所の希望は使わない
    bool anon = (flags & MAP_ANONYMOUS) != 0;
    if (len == 0 || (flags & MAP_FIXED) || !(flags & (MAP_SHARED | MAP_PRIVATE)) || (off & (PAGE_SIZE - 1)) ||
        (!anon && off < 0)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    size_t size = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
    char *p = memalign(PAGE_SIZE, size);
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    memset(p, 0, size);
    if (!anon && rw_at(fd, p, len, off, false) < 0) {
        free(p);
        return MAP_FAILED;  // errno は read・lseek（EBADF など）
    }
    klock_acquire(&mman_lock);
    Map *m = nullptr;
    for (int i = 0; i < MMAP_MAX && !m; i++)
        if (!maps[i].addr)
            m = &maps[i];
    if (m)
        *m = (Map){.addr = p, .len = size, .fd = anon ? -1 : fd, .off = off,
                   .writeback = !anon && (flags & MAP_SHARED) && (prot & PROT_WRITE), .owner = svc_owner()};
    klock_release(&mman_lock);
    if (!m) {
        free(p);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return p;
}

// 書き戻す（msync・munmap。ファイルの大きさは変えない: 今の大きさより先は書かない）
static int write_back(const Map *m)
{
    if (!m->writeback)
        return 0;
    off_t end = lseek(m->fd, 0, SEEK_END);
    if (end < 0)
        return -1;
    if (end <= m->off)
        return 0;
    size_t n = (size_t)(end - m->off) < m->len ? (size_t)(end - m->off) : m->len;
    return rw_at(m->fd, m->addr, n, m->off, true) < 0 ? -1 : 0;
}

int munmap(void *addr, size_t len)
{
    (void)len;      // 一部だけ外すことはできない（mmap の全体を外す）
    klock_acquire(&mman_lock);
    Map *m = find_map(addr);
    Map copy = m ? *m : (Map){0};
    if (m)
        m->addr = nullptr;
    klock_release(&mman_lock);
    if (!m) {
        errno = EINVAL;
        return -1;
    }
    int r = write_back(&copy);
    free(copy.addr);
    return r;
}

int msync(void *addr, size_t len, int flags)
{
    (void)len;
    (void)flags;
    klock_acquire(&mman_lock);
    Map *m = find_map(addr);
    Map copy = m ? *m : (Map){0};
    klock_release(&mman_lock);
    if (!m) {
        errno = ENOMEM;
        return -1;
    }
    return write_back(&copy);
}

// 守りの種類は変えられない（読み書きできるまま）。調べるだけ
int mprotect(void *addr, size_t len, int prot)
{
    (void)len;
    (void)prot;
    if ((uintptr_t)addr & (PAGE_SIZE - 1)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int madvise(void *addr, size_t len, int advice)
{
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

int posix_madvise(void *addr, size_t len, int advice)
{
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

// 持ち主（アプリ・サービス）が終わった（os/slimcc/run.c、os/kernel/svc.c）: その mmap を外して解放する（書き戻しは
// しない: Linux は終わるときにも書き戻すが、ここでは fd がもう閉じられている。msync・munmap を使う）
void mman_close_owner(int owner)
{
    for (;;) {
        void *p = nullptr;
        klock_acquire(&mman_lock);
        for (int i = 0; i < MMAP_MAX && !p; i++)
            if (maps[i].addr && maps[i].owner == owner) {
                p = maps[i].addr;
                maps[i].addr = nullptr;
            }
        klock_release(&mman_lock);
        if (!p)
            return;
        free(p);
    }
}
