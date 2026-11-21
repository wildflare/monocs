// os/libc/malloc.c（TLSF）を Linux で確かめる。偽の sbrk とロックで動かし、乱数で確保・解放・realloc・memalign を
// 繰り返して、中身が壊れないこと・重ならないこと・境界がそろうこと・空きがつながることを調べる。
// アプリ用のヒープ（上から下へ広げる）でも同じことをし、捨てた後にカーネル用のものが無事なことを調べる。
//   gcc -std=c23 -O2 -o test_malloc tools/picolibc/test_malloc.c && ./test_malloc
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// malloc.c が使うものを用意し、関数名を変えて取り込む（Linux の malloc とぶつからないように）
struct __lock { int dummy; };
struct __lock __lock___libc_recursive_mutex;
static void __retarget_lock_acquire_recursive(struct __lock *l) { (void)l; }
static void __retarget_lock_release_recursive(struct __lock *l) { (void)l; }
alignas(4096) static char heap[1024 << 20];
static size_t brk_off = 8;     // 16 バイト境界でない始まりも試す
static size_t app_off = sizeof(heap);   // アプリ用の下端（上から下へ）
static size_t app_top_off = sizeof(heap);   // アプリ用の上端（入れ子の子の間は、親の下端）
static size_t saved_top[8];
static int nest_depth;
void *t_sbrk(ptrdiff_t incr)
{
    if (brk_off + incr > app_off)
        return (void *)-1;
    void *p = heap + brk_off;
    brk_off += incr;
    return p;
}
// os/kernel/memory.c の代わり
uintptr_t memory_heap_brk(void) { return (uintptr_t)heap + brk_off; }
uintptr_t memory_app_low(void) { return (uintptr_t)heap + app_off; }
uintptr_t memory_app_top(void) { return (uintptr_t)heap + app_top_off; }
void memory_app_reset(void) { app_off = app_top_off; }
bool memory_app_push(void)
{
    if (nest_depth == 8) return false;
    saved_top[nest_depth++] = app_top_off;
    app_top_off = app_off;
    return true;
}
void memory_app_pop(void) { if (nest_depth > 0) app_top_off = saved_top[--nest_depth]; }
void *memory_app_grow(size_t incr)
{
    if (incr > app_off - brk_off)
        return (void *)-1;
    app_off -= incr;
    return heap + app_off;
}
#define sbrk t_sbrk
#define malloc t_malloc
#define free t_free
#define cfree t_cfree
#define calloc t_calloc
#define realloc t_realloc
#define memalign t_memalign
#define aligned_alloc t_aligned_alloc
#define posix_memalign t_posix_memalign
#define valloc t_valloc
#define pvalloc t_pvalloc
#define malloc_usable_size t_malloc_usable_size
#define mallinfo t_mallinfo
#define malloc_stats t_malloc_stats
#define mallopt t_mallopt
#define MALLOC_TEST     // picolibc の sys/lock.h の代わりに、上で用意したロックを使う
#include "../../os/libc/malloc.c"
#undef malloc
#undef free
#undef realloc

#define N 5000
static unsigned char *ptr[N];
static size_t len[N];
static unsigned char tag[N];
static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void fill(int i) { memset(ptr[i], tag[i], len[i]); }
static void check(int i)
{
    for (size_t k = 0; k < len[i]; k++)
        if (ptr[i][k] != tag[i]) {
            printf("corrupted block %d at %zu\n", i, k);
            exit(1);
        }
}

static unsigned char kscope[N];     // アプリの段階で、malloc_kernel_begin の中で確保したもの

static bool in_app(const void *p) { return (uintptr_t)p >= memory_app_low() && (uintptr_t)p < memory_app_top(); }

// 乱数で確保・解放を rounds 回。app なら、ときどき malloc_kernel_begin の中で確保する
static long stress(int rounds, bool app)
{
    long ops = 0;
    for (int round = 0; round < rounds; round++) {
        int i = (int)(rnd() % N);
        if (ptr[i]) {
            check(i);
            if (rnd() % 3 == 0) {   // realloc
                size_t n = rnd() % 4 == 0 ? rnd() % 100000 : rnd() % 600;
                unsigned char *q = t_realloc(ptr[i], n ? n : 1);
                if (!q) { printf("realloc failed\n"); exit(1); }
                if (in_app(q) != in_app(ptr[i])) { printf("realloc moved to the other heap\n"); exit(1); }
                size_t keep = len[i] < (n ? n : 1) ? len[i] : (n ? n : 1);
                for (size_t k = 0; k < keep; k++)
                    if (q[k] != tag[i]) { printf("realloc lost data\n"); exit(1); }
                ptr[i] = q; len[i] = n ? n : 1; tag[i]++; fill(i);
            } else {
                t_free(ptr[i]);
                ptr[i] = NULL;
            }
        } else {
            // ほとんどは小さく、ときどき中くらい（〜200KB）、まれに大きく（1〜8MB。区分の幅が広い大きさ）
            size_t n = rnd() % 8 == 0 ? rnd() % 200000 : rnd() % 300;
            if (rnd() % 500 == 0)
                n = (1 << 20) + rnd() % (7 << 20);
            int kind = (int)(rnd() % 10);
            kscope[i] = app && rnd() % 4 == 0;
            if (kscope[i])
                malloc_kernel_begin();
            if (kind == 0) {
                size_t a = (size_t)16 << (rnd() % 9);   // 16〜4096
                ptr[i] = t_memalign(a, n);
                if ((uintptr_t)ptr[i] % a) { printf("misaligned %zu\n", a); exit(1); }
            } else if (kind == 1) {
                ptr[i] = t_calloc(1, n);
                for (size_t k = 0; k < n; k++) if (ptr[i][k]) { printf("calloc not zero\n"); exit(1); }
            } else {
                ptr[i] = t_malloc(n);
            }
            if (kscope[i])
                malloc_kernel_end();
            if (!ptr[i]) { printf("alloc failed (%zu)\n", n); exit(1); }
            if ((uintptr_t)ptr[i] % 16) { printf("not 16-aligned\n"); exit(1); }
            if (t_malloc_usable_size(ptr[i]) < n) { printf("usable size too small\n"); exit(1); }
            if (in_app(ptr[i]) != (app && !kscope[i])) { printf("allocated from the wrong heap\n"); exit(1); }
            len[i] = n; tag[i] = (unsigned char)rnd(); fill(i);
        }
        ops++;
    }
    return ops;
}

static int count_free(heap_t *h)
{
    int nfree = 0;
    for (int f = 0; f < FL_COUNT; f++)
        for (int s = 0; s < SL_COUNT; s++)
            for (block_t *b = h->heads[f][s]; b; b = b->next_free) nfree++;
    return nfree;
}

int main(void)
{
    // カーネル用だけ
    long ops = stress(3000000, false);
    for (int i = 0; i < N; i++) if (ptr[i]) { check(i); t_free(ptr[i]); ptr[i] = NULL; }
    struct mallinfo mi = t_mallinfo();
    // すべて解放したら、空きは 1 つの大きなブロックにつながっているはず
    int nfree = count_free(&kheap);
    printf("kernel: %ld ops, arena %zu KB, in use %zu, free blocks %d\n", ops, mi.arena / 1024, mi.uordblks, nfree);
    if (mi.uordblks != 0 || nfree != 1)
        return 1;

    // カーネル用に残しておくもの
    unsigned char *keep[64];
    for (int k = 0; k < 64; k++) { keep[k] = t_malloc(1000 + k); memset(keep[k], k, 1000 + k); }

    // アプリ用（2 回。捨てた後も使えること）
    for (int pass = 0; pass < 2; pass++) {
        malloc_app_begin();
        // カーネル用の終わりの空きは返している（残すもの 64 個と、前の回の小さな確保だけ）
        if (kheap.arena > 1024 * 1024) { printf("kernel heap not trimmed (%zu KB)\n", kheap.arena / 1024); return 1; }
        ops = stress(1000000, true);
        malloc_app_stop();
        // アプリが終わった後: カーネル用のものは自分で解放、アプリ用のものは半分だけ解放して残りは捨てる
        for (int i = 0; i < N; i++) {
            if (!ptr[i]) continue;
            check(i);
            if (!in_app(ptr[i]) || rnd() % 2) t_free(ptr[i]);
            ptr[i] = NULL;
        }
        if (in_app(t_malloc(10))) { printf("allocated from the app heap after stop\n"); return 1; }
        int afree = count_free(&aheap);
        size_t arena = aheap.arena;
        size_t leaked = malloc_app_discard();
        printf("app %d: %ld ops, arena %zu KB, leaked %zu KB, free blocks %d\n", pass, ops, arena / 1024, leaked / 1024, afree);
        if (aheap.arena || aheap.in_use || memory_app_low() != memory_app_top()) { printf("app heap not reset\n"); return 1; }
    }
    // 入れ子（system() の子のアプリ）: 親のヒープを残したまま、子のヒープを作って捨てる
    {
        if (!malloc_app_begin()) { printf("app begin failed\n"); return 1; }
        unsigned char *pa[200];
        for (int k = 0; k < 200; k++) { pa[k] = t_malloc(100 + k * 37); memset(pa[k], k, 100 + k * 37); }
        for (int k = 0; k < 200; k += 3) { t_free(pa[k]); pa[k] = NULL; }
        uintptr_t parent_low = memory_app_low(), parent_top = memory_app_top();
        size_t parent_use = aheap.in_use;
        malloc_kernel_begin();          // shell_system と同じ
        for (int depth = 0; depth < 2; depth++)
            if (!malloc_app_begin()) { printf("nested begin failed\n"); return 1; }
        if (memory_app_top() > parent_low) { printf("child region overlaps the parent\n"); return 1; }
        unsigned char *ca[300];
        for (int k = 0; k < 300; k++) { ca[k] = t_malloc(50 + k * 101); memset(ca[k], 0xEE, 50 + k * 101); }
        for (int k = 0; k < 300; k++)
            if ((uintptr_t)ca[k] >= parent_low && (uintptr_t)ca[k] < parent_top) { printf("child allocated in the parent region\n"); return 1; }
        for (int k = 0; k < 300; k += 2) t_free(ca[k]);
        malloc_app_stop();
        malloc_app_discard();           // 孫
        malloc_app_stop();
        malloc_app_discard();           // 子
        malloc_kernel_end();
        if (memory_app_low() != parent_low || memory_app_top() != parent_top || aheap.in_use != parent_use) {
            printf("parent heap not restored\n"); return 1;
        }
        for (int k = 0; k < 200; k++)
            if (pa[k]) for (int j = 0; j < 100 + k * 37; j++) if (pa[k][j] != k) { printf("parent block corrupted\n"); return 1; }
        for (int k = 0; k < 200; k++) if (pa[k]) { pa[k] = t_realloc(pa[k], 5000); if (!in_app(pa[k])) { printf("parent realloc left the app heap\n"); return 1; } }
        for (int k = 0; k < 200; k++) t_free(pa[k]);
        if (aheap.in_use != 0) { printf("parent heap leaked\n"); return 1; }
        malloc_app_stop();
        malloc_app_discard();
        if (memory_app_low() != memory_app_top() || memory_app_top() != (uintptr_t)heap + sizeof(heap)) { printf("app heap not reset after nesting\n"); return 1; }
        printf("nested: ok\n");
    }
    // カーネル用の途中に大きな空き（返せない）があるとき、アプリ用が足りなくなったらそこからも取る
    unsigned char *mid = t_malloc(100 << 20), *top = t_malloc(100);
    t_free(mid);
    size_t kernel_before = kheap.in_use;
    malloc_app_begin();
    static unsigned char *big[2048];
    int nbig = 0, nkernel = 0;
    while (nbig < 2048 && (big[nbig] = t_malloc(1 << 20))) {
        memset(big[nbig], 0x5A, 1 << 20);
        if (!in_app(big[nbig])) nkernel++;
        nbig++;
    }
    // いっぱいのときの memalign と、カーネル用から取ったものの realloc（縮める。印が残ること）
    unsigned char *al = t_memalign(4096, 1000);
    if (!al || (uintptr_t)al % 4096) { printf("memalign failed\n"); return 1; }
    for (int i = 1; i < nbig; i += 2)
        if (!in_app(big[i])) { big[i] = t_realloc(big[i], 1000); break; }
    malloc_app_stop();
    printf("fill: %d MB (%d MB from the kernel heap)\n", nbig, nkernel);
    if (nkernel < 90 || nbig < 900) { printf("did not use the kernel heap's free space\n"); return 1; }
    // カーネル用から取ったもの（半分はアプリが解放、残りは捨てるときに解放される）
    for (int i = 0; i < nbig; i += 2) if (!in_app(big[i])) t_free(big[i]);
    malloc_app_discard();
    if (kheap.in_use != kernel_before) { printf("app blocks in the kernel heap not freed (%zu -> %zu)\n", kernel_before, kheap.in_use); return 1; }
    t_free(top);
    for (int k = 0; k < 64; k++) {
        for (int j = 0; j < 1000 + k; j++) if (keep[k][j] != k) { printf("kernel block corrupted\n"); return 1; }
        t_free(keep[k]);
    }
    // カーネル用は小さな 2 つ（各回の stop の後の確保、32 バイトずつ）を除いて、空きにつながる
    nfree = count_free(&kheap);
    printf("kernel after apps: in use %zu, free blocks %d\n", kheap.in_use, nfree);
    return kheap.in_use == 64 && nfree <= 3 ? 0 : 1;
}
