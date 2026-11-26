// サンドボックスの中の malloc（データの領域の中だけ。空きを 1 本の一覧に持つ簡単な形）と、exit など
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sandbox_sys.h"

struct blk {
    size_t size;            // 頭を除いた大きさ（16 の倍数）
    struct blk *next;       // 空きの一覧（使っている間は使わない）
};
#define HDR 16
static uintptr_t heap_cur, heap_end;
static struct blk *free_list;

void __sandbox_heap_init(uint64_t start, uint64_t end)
{
    heap_cur = (uintptr_t)((start + 15) & ~15ULL);
    heap_end = (uintptr_t)end;
    free_list = nullptr;
}

void *malloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!n)
        n = 16;
    for (struct blk **pp = &free_list; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= n) {
            struct blk *b = *pp;
            *pp = b->next;
            return (char *)b + HDR;
        }
    if (heap_end - heap_cur < n + HDR)
        return nullptr;
    struct blk *b = (struct blk *)heap_cur;
    heap_cur += n + HDR;
    b->size = n;
    return (char *)b + HDR;
}

void free(void *p)
{
    if (!p)
        return;
    struct blk *b = (struct blk *)((char *)p - HDR);
    b->next = free_list;
    free_list = b;
}

void *calloc(size_t a, size_t b)
{
    if (b && a > (size_t)-1 / b)
        return nullptr;
    void *p = malloc(a * b);
    if (p)
        memset(p, 0, a * b);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    struct blk *b = (struct blk *)((char *)p - HDR);
    if (b->size >= n)
        return p;
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, b->size);
        free(p);
    }
    return q;
}

[[noreturn]] void exit(int status)
{
    fflush(stdout);
    fflush(stderr);
    for (;;)
        sb_sys(SB_SYS_EXIT, status, 0, 0);
}

[[noreturn]] void abort(void)
{
    exit(134);
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

int atoi(const char *s)
{
    int neg = 0, v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
