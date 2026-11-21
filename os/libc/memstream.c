// open_memstream を、picolibc のバッファ付きの FILE（stdio-bufio.h の FDEV_SETUP_BUFIO）で作る（2026-11-15）。
// picolibc の open_memstream は 1 文字ごとに __open_mem_put を呼び、そのたびに大きさを調べる。コンパイラは
// アセンブリの文字列を全部これに書くので、実機で時間の 1〜2 割を使っていた。バッファ付きなら、fwrite・fputs は
// memcpy（fast-bufio）、vfprintf は __bufio_put（バッファに置くだけ）になり、書き出しは 4KB ごと。
// カーネルで定義して picolibc の同じ名前の関数より先にリンクする（malloc.c と同じ）。third_party は変えない。
// 動きは picolibc・glibc と同じ: *bufp・*sizep は fflush・fclose・fseek のたびに新しくし、大きさは今の位置、
// 中身は NUL で終わる。位置を終わりより先に動かして書いたら、間は 0 で埋める。
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio-bufio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define MS_IOBUF 4096

typedef struct {
    struct __file_bufio bf;  // 先頭（FILE * と同じ番地。fclose が __BFALL で free する）
    char **bufp;
    size_t *sizep;
    char *data;              // 呼び出し元のもの（fclose の後も残す。free は呼び出し元）
    size_t cap, pos, len;
    char iobuf[MS_IOBUF];
} MemStream;

static void ms_publish(MemStream *m)
{
    *m->bufp = m->data;
    *m->sizep = m->pos;
}

// n バイト（と NUL）が入るように広げる。広げた分は 0
static int ms_reserve(MemStream *m, size_t need)
{
    if (need + 1 <= m->cap)
        return 0;
    size_t cap = m->cap ? m->cap : 64;
    while (cap < need + 1) {
        if (cap > SIZE_MAX / 2) {
            errno = ENOMEM;
            return -1;
        }
        cap *= 2;
    }
    char *p = realloc(m->data, cap);
    if (!p) {
        errno = ENOMEM;
        return -1;
    }
    memset(p + m->cap, 0, cap - m->cap);
    m->data = p;
    m->cap = cap;
    return 0;
}

static ssize_t ms_write(void *ptr, const void *buf, size_t n)
{
    MemStream *m = ptr;
    if (m->pos > SIZE_MAX - n - 1 || ms_reserve(m, m->pos + n) != 0)
        return -1;
    memcpy(m->data + m->pos, buf, n);   // 終わりより先の間は、広げたときに 0 にしてある
    m->pos += n;
    if (m->pos > m->len) {
        m->len = m->pos;
        m->data[m->len] = '\0';
    }
    ms_publish(m);
    return (ssize_t)n;
}

static ssize_t ms_read(void *ptr, void *buf, size_t n)
{
    (void)ptr;
    (void)buf;
    (void)n;
    errno = EBADF;
    return -1;
}

static off_t ms_lseek(void *ptr, off_t off, int whence)
{
    MemStream *m = ptr;
    off_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (off_t)m->pos; break;
    case SEEK_END: base = (off_t)m->len; break;
    default:
        errno = EINVAL;
        return -1;
    }
    off_t np = base + off;
    if (np < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)np > m->len && ms_reserve(m, (size_t)np) != 0)
        return -1;
    m->pos = (size_t)np;
    ms_publish(m);
    return np;
}

static int ms_close(void *ptr)
{
    ms_publish(ptr);
    return 0;
}

FILE *open_memstream(char **bufp, size_t *sizep)
{
    if (!bufp || !sizep) {
        errno = EINVAL;
        return NULL;
    }
    // 入れ物（iobuf、4KB）は 0 にしなくてよいので、その前だけを 0 にする（2026-11-16。コンパイラは関数ごとに
    // open_memstream を開くので、毎回 4KB を 0 にしていた）
    MemStream *m = malloc(sizeof *m);
    if (!m)
        return NULL;
    memset(m, 0, offsetof(MemStream, iobuf));
    m->bufp = bufp;
    m->sizep = sizep;
    if (ms_reserve(m, 0) != 0) {
        free(m);
        return NULL;
    }
    m->bf = (struct __file_bufio)FDEV_SETUP_BUFIO(0, m->iobuf, MS_IOBUF, NULL, NULL, NULL, NULL, __SWR,
                                                  __BFALL | __BFPTR);
    m->bf.ptr = m;
    m->bf.read_ptr = ms_read;
    m->bf.write_ptr = ms_write;
    m->bf.lseek_ptr = ms_lseek;
    m->bf.close_ptr = ms_close;
    ms_publish(m);
    return &m->bf.xfile.cfile.file;
}
