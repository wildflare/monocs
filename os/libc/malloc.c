// メモリの確保（malloc・free など）: TLSF（Two-Level Segregated Fit）で picolibc の malloc を置き換える。
// picolibc の malloc（nano-malloc）は空き領域を 1 本のリストで先頭からたどるので、空き領域が増えると遅い
// （実機で 1 回 約 23µs、picolibc のビルドの後は約 89µs。同じ起動のまま picolibc をビルドし直すと 49 → 120 秒）。
// TLSF は空き領域を大きさで 2 段に分けたリストに置き、ビットマップで O(1) で見つける。
// 出典: M. Masmano, I. Ripoll, A. Crespo, J. Real, "TLSF: a New Dynamic Memory Allocator for Real-Time
//       Systems", ECRTS 2004。大きさの区分（第 1 段は 2 の累乗、第 2 段はそれを 32 に等分）はこの論文による。
// os/libc/ldtoa.c と同じく、カーネルで定義して picolibc の同名の関数より先にリンクする（third_party は無改造）。
// picolibc の malloc の関数はすべてここで定義する（libc.a の malloc のメンバーを引き込まないように）。
//
// ブロック: 16 バイトの見出し { prev_size, size } と本体。size はブロック全体の大きさ（16 の倍数）で、
// 下位のビットに「このブロックは空き」「前のブロックは空き」を持つ。前が空きのときだけ prev_size が有効
// （空きになったブロックが、次のブロックの prev_size に自分の大きさを書く。境界タグ）。
// 空きのブロックは本体に空きリストの前後を持つ（最小 32 バイト）。領域の終わりには大きさ 0 の番兵を置く。
// ロックは picolibc の malloc と同じ __lock___libc_recursive_mutex（例外で止まったときの解放、
// Ctrl-C の後回しも同じになる。os/kernel/lock.c）。
//
// ヒープは 2 つ（2026-11-12）: カーネル用（ヒープの領域の下から上へ sbrk で広げる）と、アプリ用（上から下へ
// memory_app_grow で広げる。os/kernel/memory.c）。アプリの実行中（malloc_app_begin〜malloc_app_stop）の確保は
// アプリ用から取り、アプリが終わったら malloc_app_discard で丸ごと捨てる（free しなかったメモリも戻る）。
// 捨てた領域は 0 で塗りつぶす。カーネルが捨てたメモリの中のポインタを使うと、NULL（0 番地のページは無効。
// os/kernel/mmu.c）を参照して例外になるので気づける。0 なのは、C ライブラリの memset が 0 のときだけ
// DC ZVA（キャッシュラインをまとめて 0 にする命令）を使い、速いため（実機で 0xA5 のときの約 0.9ms/MB から）。
//   - どちらのヒープのものかは番地で決める（アプリ用の領域の中か）。解放・realloc は元のヒープで行う。
//   - アプリの実行中でも、アプリが終わった後まで残すもの（VFS のファイルのキャッシュ、アプリから呼んだ
//     コンパイラ）は malloc_kernel_begin〜malloc_kernel_end の間に確保して、カーネル用から取る（コアごと）。
//   - サービス（コア 3）はいつもカーネル用（os/kernel/svc.c の runner が malloc_kernel_begin を呼ぶ）。
//   - アプリを始めるとき、カーネル用の終わりの空きを sbrk で返す（trim_kernel）。途中の空きは返せないので、
//     アプリ用が足りなくなったら、カーネル用の空きからも取る（alloc_any。この分は捨てられない）。

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef MALLOC_TEST     // tools/picolibc/test_malloc.c（Linux）では、ロックを自分で用意する
#include <sys/lock.h>
#endif
#include <unistd.h>

void *sbrk(ptrdiff_t incr);    // os/kernel/memory.c
#ifndef MALLOC_TEST
#include "heap.h"
#include "../kernel/memory.h"
#endif


#define ALIGN           16
#define HDR             16                  // 見出しの大きさ
#define MIN_BLOCK       32                  // 見出しと、空きリストの前後
#define F_FREE          1UL
#define F_PREV_FREE     2UL
#define F_APP           4UL                 // カーネル用のヒープにある、アプリのもの（alloc_any。アプリの終わりに解放）
#define SIZE_MASK       (~(size_t)15)

#define SL_LOG2         5                   // 第 2 段の区分は 32
#define SL_COUNT        (1 << SL_LOG2)
#define FL_SHIFT        (SL_LOG2 + 4)       // これより小さい（512 バイト未満）は第 1 段 0 で 16 バイトきざみ
#define SMALL_BLOCK     (1UL << FL_SHIFT)
#define FL_COUNT        26                  // 2^(26 + 8) まで（実際の上限は RAM の大きさ）
#define GROW_MIN        (1024 * 1024)       // sbrk で一度に広げる最小の量

typedef struct block {
    size_t prev_size;
    size_t size;
    struct block *next_free, *prev_free;    // 空きのときだけ
} block_t;

typedef struct heap {
    uint32_t fl_map;
    uint32_t sl_map[FL_COUNT];
    block_t *heads[FL_COUNT][SL_COUNT];
    block_t *sentinel;                      // 今の領域の終わりの番兵（広げるときに使う）
    block_t *first;                         // 最初のブロック（カーネル用。アプリの終わりに F_APP を探す）
    size_t arena, in_use;                   // 広げた量、使用中の量（見出しを含む）
    size_t peak;                            // 使用中の量のいちばん多かったとき（mem。malloc_peak_reset で今に）
    bool app;                               // アプリ用（上から下へ広げる）
} heap_t;

static heap_t kheap, aheap = {.app = true};
static size_t peak_total;                   // カーネル用とアプリ用の使用中の合計の、いちばん多かったとき

static inline void note_use(heap_t *h)
{
    if (h->in_use > h->peak)
        h->peak = h->in_use;
    size_t t = kheap.in_use + aheap.in_use;
    if (t > peak_total)
        peak_total = t;
}

// 確保できなかった（領域を広げられなかった）ときに呼ぶ（カーネルが定義すれば。os/kernel/memory.c が dmesg に書く。
// Linux の試験 tools/picolibc/test_malloc.c では定義しない）
__attribute__((weak)) void malloc_out_of_memory(size_t n, bool app, size_t k_in_use, size_t a_in_use);
static bool app_on;                         // アプリの実行中（確保をアプリ用から取る）

// 入れ子のアプリ（アプリの中から system() で動かしたもの）: 外側のアプリのヒープと状態を退避して、子は親の
// 領域のすぐ下に自分の領域を作る（os/kernel/memory.c の memory_app_push）。子が終わったら子の領域だけを捨てて、
// 外側のものを戻す。子の間に外側のもの（番地が退避した領域の中）を free したら、そのヒープへ返す
#define APP_NEST_MAX 8
static struct {
    heap_t heap;
    uintptr_t low, top;                     // その領域 [low, top)（子の間は広がらない）
    bool app_on;
    int kernel_depth;
} saved[APP_NEST_MAX];
static int nest;
static size_t last_app_arena, last_app_leaked;  // 最後に捨てたアプリ用のヒープの大きさと、free されなかった量
static _Thread_local int kernel_depth;      // このコアの malloc_kernel_begin の深さ

// 確保に使うヒープ
static heap_t *current(void)
{
    return app_on && kernel_depth == 0 ? &aheap : &kheap;
}

// p を確保したヒープ（番地で決める）
static heap_t *heap_of(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    if (a >= memory_app_low() && a < memory_app_top())
        return &aheap;
    for (int i = nest - 1; i >= 0; i--)
        if (a >= saved[i].low && a < saved[i].top)
            return &saved[i].heap;
    return &kheap;
}

static void lock(void) { __retarget_lock_acquire_recursive(&__lock___libc_recursive_mutex); }
static void unlock(void) { __retarget_lock_release_recursive(&__lock___libc_recursive_mutex); }

static size_t bsize(const block_t *b) { return b->size & SIZE_MASK; }
static block_t *next_phys(block_t *b) { return (block_t *)((char *)b + bsize(b)); }
static block_t *prev_phys(block_t *b) { return (block_t *)((char *)b - b->prev_size); }
static void *payload(block_t *b) { return (char *)b + HDR; }
static block_t *from_payload(void *p) { return (block_t *)((char *)p - HDR); }

static int fls_size(size_t x) { return 63 - __builtin_clzl(x); }     // 最上位のビットの位置（x > 0）

static void mapping(size_t size, int *fl, int *sl)
{
    if (size < SMALL_BLOCK) {
        *fl = 0;
        *sl = (int)(size / (SMALL_BLOCK / SL_COUNT));
    } else {
        int f = fls_size(size);
        *sl = (int)((size >> (f - SL_LOG2)) ^ SL_COUNT);
        *fl = f - FL_SHIFT + 1;
    }
}

static void list_remove(heap_t *h, block_t *b, int fl, int sl)
{
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
        h->heads[fl][sl] = b->next_free;
    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
    if (!h->heads[fl][sl]) {
        h->sl_map[fl] &= ~(1U << sl);
        if (!h->sl_map[fl])
            h->fl_map &= ~(1U << fl);
    }
}

static void list_insert(heap_t *h, block_t *b)
{
    int fl, sl;
    mapping(bsize(b), &fl, &sl);
    b->prev_free = nullptr;
    b->next_free = h->heads[fl][sl];
    if (b->next_free)
        b->next_free->prev_free = b;
    h->heads[fl][sl] = b;
    h->fl_map |= 1U << fl;
    h->sl_map[fl] |= 1U << sl;
}

static void remove_free(heap_t *h, block_t *b)
{
    int fl, sl;
    mapping(bsize(b), &fl, &sl);
    list_remove(h, b, fl, sl);
}

// b を空きにする（隣の空きとつなげてから、リストに入れる）
static void make_free(heap_t *h, block_t *b)
{
    size_t size = bsize(b);
    if (b->size & F_PREV_FREE) {
        block_t *p = prev_phys(b);
        remove_free(h, p);
        size += bsize(p);
        b = p;
    }
    block_t *n = (block_t *)((char *)b + size);
    if (n->size & F_FREE) {
        remove_free(h, n);
        size += bsize(n);
    }
    b->size = size | F_FREE | (b->size & F_PREV_FREE);
    n = next_phys(b);
    n->prev_size = size;
    n->size |= F_PREV_FREE;
    list_insert(h, b);
}

// 使用中のブロック b を size に切り詰め、余りを空きにする
static void split(heap_t *h, block_t *b, size_t size)
{
    size_t total = bsize(b);
    if (total - size < MIN_BLOCK)
        return;
    b->size = size | (b->size & F_PREV_FREE);
    block_t *rest = next_phys(b);
    rest->size = (total - size);    // 使用中として作ってから make_free（前は使用中）
    next_phys(rest)->size &= ~F_PREV_FREE;
    make_free(h, rest);
}

// 区分の中のどの空きブロックでも足りるように、size を次の区分の始めまで切り上げる（論文の mapping_search）
static size_t round_class(size_t size)
{
    return size >= SMALL_BLOCK ? size + (1UL << (fls_size(size) - SL_LOG2)) - 1 : size;
}

// size（見出しを含む）以上の空きブロックを取り出す。なければ nullptr
static block_t *find_free(heap_t *h, size_t size)
{
    int fl, sl;
    mapping(round_class(size), &fl, &sl);
    if (fl < FL_COUNT) {
        uint32_t m = h->sl_map[fl] & (~0U << sl);
        if (!m) {
            uint32_t f = fl + 1 < 32 ? h->fl_map & (~0U << (fl + 1)) : 0;
            if (f) {
                fl = __builtin_ctz(f);
                m = h->sl_map[fl];
            }
        }
        if (m) {
            sl = __builtin_ctz(m);
            block_t *b = h->heads[fl][sl];
            list_remove(h, b, fl, sl);
            return b;
        }
    }
    // 切り上げた区分より上に空きがない: size と同じ区分のリストを順に探す（足りるものがあるかもしれない）
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT)
        return nullptr;
    for (block_t *b = h->heads[fl][sl]; b; b = b->next_free)
        if (bsize(b) >= size) {
            list_remove(h, b, fl, sl);
            return b;
        }
    return nullptr;
}

// 領域を少なくとも size だけ広げる
static bool grow(heap_t *h, size_t size)
{
    // 切り上げた大きさの分を広げる（size ちょうどでは、find_free が切り上げた区分で探すので見つからない。
    // 1MB を少し超える確保（slimcc のゾーンの塊）で NULL を返していた）
    size_t incr = round_class(size) + MIN_BLOCK + HDR + ALIGN;
    if (incr < GROW_MIN)
        incr = GROW_MIN;
    incr = (incr + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    if (h->app) {
        // アプリ用は上から下へ: [p, p + incr) が今までの領域のすぐ下に足される（初めは領域の上端の下）
        char *p = memory_app_grow(incr);
        if (p == (char *)-1)
            return false;
        h->arena += incr;
        block_t *b = (block_t *)p;      // 上端も incr も 16 の倍数なので、そろっている
        if (!h->sentinel) {
            b->size = (incr - HDR) & SIZE_MASK;
            h->sentinel = next_phys(b);
            h->sentinel->size = 0;
        } else {
            b->size = incr;             // すぐ上は今までのいちばん下のブロック。make_free が空きならつなげる
        }
        make_free(h, b);
        return true;
    }
    char *p = sbrk((ptrdiff_t)incr);
    if (p == (char *)-1)
        return false;
    h->arena += incr;
    block_t *b;
    char *end = p + incr;
    if (h->sentinel && (char *)h->sentinel + HDR <= p && p - ((char *)h->sentinel + HDR) < ALIGN) {
        // 続きに広がった（境界合わせのすき間は 16 バイト未満）: 今の番兵を新しいブロックの見出しにする
        b = h->sentinel;
        b->size = ((size_t)(end - (char *)b - HDR) & SIZE_MASK) | (b->size & F_PREV_FREE);
    } else {
        // 新しい領域（sbrk の始めは 16 バイト境界とは限らないので、そろえる）
        char *start = (char *)(((uintptr_t)p + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1));
        size_t usable = incr - (size_t)(start - p) - HDR;     // 番兵の分を除く
        usable &= SIZE_MASK;
        b = (block_t *)start;
        b->size = usable;
        if (!h->first)
            h->first = b;
    }
    h->sentinel = next_phys(b);
    h->sentinel->size = 0;     // 使用中で大きさ 0（ここでつながるのを止める）
    make_free(h, b);
    return true;
}

// 求められた大きさから、ブロックの大きさ（見出しを含む）へ
static bool block_size_for(size_t n, size_t *out)
{
    if (n > ((size_t)1 << 40))
        return false;
    size_t s = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    *out = s < MIN_BLOCK ? MIN_BLOCK : s;
    return true;
}

static void *alloc_locked(heap_t *h, size_t n)
{
    size_t size;
    if (!block_size_for(n, &size))
        return nullptr;
    block_t *b = find_free(h, size);
    if (!b) {
        if (!grow(h, size)) {
            if (malloc_out_of_memory)
                malloc_out_of_memory(n, h->app, kheap.in_use, aheap.in_use);
            return nullptr;
        }
        b = find_free(h, size);
        if (!b)
            return nullptr;
    }
    // 使用中にする
    b->size &= ~F_FREE;
    next_phys(b)->size &= ~F_PREV_FREE;
    split(h, b, size);
    h->in_use += bsize(b);
    note_use(h);
    return payload(b);
}

static void free_locked(heap_t *h, void *p)
{
    block_t *b = from_payload(p);
    h->in_use -= bsize(b);
    make_free(h, b);
}

// 今のヒープから取る。アプリ用が足りなければ（カーネル用とぶつかった）、カーネル用の空きから取る
// （カーネル用の途中の空きは返せないので、アプリが使える量が前より減らないように）。カーネル用から取った
// ものには F_APP を付け、アプリが終わったら malloc_app_discard が解放する
static void *alloc_any(heap_t *h, size_t n)
{
    void *p = alloc_locked(h, n);
    if (!p && h == &aheap && nest == 0) {   // 入れ子の子は取らない（F_APP は外側のアプリの終わりにまとめて解放する）
        p = alloc_locked(&kheap, n);
        if (p)
            from_payload(p)->size |= F_APP;
    }
    return p;
}

static void bad_free(void *p)
{
#ifdef MALLOC_TEST
    printf("free: invalid pointer %p\n", p);
#else
    fprintf(stderr, "free: invalid pointer %p (ignored)\n", p);
#endif
}

void *malloc(size_t n)
{
    lock();
    void *p = alloc_any(current(), n);
    unlock();
    if (!p)
        errno = ENOMEM;
    return p;
}

void free(void *p)
{
    if (!p)
        return;
    lock();
    heap_t *h = heap_of(p);
    if (h == &kheap && (uintptr_t)p >= memory_heap_brk()) {
        // どちらのヒープの中でもない（捨てたアプリ用の領域のものなど）。つなぐとカーネル用が壊れるので無視する
        unlock();
        bad_free(p);
        return;
    }
    free_locked(h, p);
    unlock();
}

void cfree(void *p)
{
    free(p);
}

void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return nullptr;
    }
    size_t n = count * size;
    void *p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (n == 0) {
        free(p);
        return nullptr;
    }
    size_t size;
    if (!block_size_for(n, &size)) {
        errno = ENOMEM;
        return nullptr;
    }
    lock();
    heap_t *h = heap_of(p);
    block_t *b = from_payload(p);
    size_t cur = bsize(b);
    size_t app_flag = b->size & F_APP;     // split などで消えるので、付け直す
    if (size <= cur) {          // 縮める: 余りを空きにする
        h->in_use -= cur;
        split(h, b, size);
        h->in_use += bsize(b);
    note_use(h);
        b->size |= app_flag;
        unlock();
        return p;
    }
    block_t *nb = next_phys(b);
    if ((nb->size & F_FREE) && cur + bsize(nb) >= size) {  // 後ろの空きを取り込んで広げる
        remove_free(h, nb);
        h->in_use -= cur;
        b->size = (cur + bsize(nb)) | (b->size & F_PREV_FREE);
        next_phys(b)->size &= ~F_PREV_FREE;
        split(h, b, size);
        h->in_use += bsize(b);
    note_use(h);
        b->size |= app_flag;
        unlock();
        return p;
    }
    void *q = alloc_any(app_flag ? &aheap : h, n);   // アプリのものは、動かしてもアプリのもの
    if (q) {
        memcpy(q, p, cur - HDR);
        free_locked(h, p);
    }
    unlock();
    if (!q)
        errno = ENOMEM;
    return q;
}

void *memalign(size_t align, size_t n)
{
    if (align <= ALIGN)
        return malloc(n);
    if (align & (align - 1)) {
        errno = EINVAL;
        return nullptr;
    }
    lock();
    // 余分に取り、境界にそろった位置から始まるブロックを切り出す（前の余りは空きにする）
    char *p = alloc_any(current(), n + align + MIN_BLOCK);
    if (!p) {
        unlock();
        errno = ENOMEM;
        return nullptr;
    }
    heap_t *h = heap_of(p);
    block_t *b = from_payload(p);
    size_t app_flag = b->size & F_APP;
    uintptr_t a = ((uintptr_t)p + MIN_BLOCK + align - 1) & ~(uintptr_t)(align - 1);
    if ((uintptr_t)p % align == 0) {
        a = (uintptr_t)p;
    }
    if (a != (uintptr_t)p) {
        block_t *nb = from_payload((void *)a);
        size_t front = (size_t)((char *)nb - (char *)b);    // MIN_BLOCK 以上、16 の倍数
        size_t total = bsize(b);
        h->in_use -= total;
        nb->size = total - front;
        b->size = front | (b->size & F_PREV_FREE);
        // 前の余りを空きにする（nb は使用中）
        make_free(h, b);
        h->in_use += bsize(nb);
    note_use(h);
        b = nb;
    }
    size_t size = MIN_BLOCK;
    block_size_for(n, &size);   // n + align が通ったので、n も通る
    h->in_use -= bsize(b);
    split(h, b, size);
    h->in_use += bsize(b);
    note_use(h);
    b->size |= app_flag;
    unlock();
    return payload(b);
}

void *aligned_alloc(size_t align, size_t n)
{
    return memalign(align, n);
}

int posix_memalign(void **out, size_t align, size_t n)
{
    if (align % sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    void *p = memalign(align, n);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *valloc(size_t n)
{
    return memalign(4096, n);
}

void *pvalloc(size_t n)
{
    return memalign(4096, (n + 4095) & ~(size_t)4095);
}

size_t malloc_usable_size(void *p)
{
    return p ? bsize(from_payload(p)) - HDR : 0;
}

// mem: 使用中の量のいちばん多かったとき（カーネル用、アプリ用、合計）。reset なら今の量に戻す
void malloc_peak(size_t *k_peak, size_t *a_peak, size_t *total, bool reset)
{
    lock();
    if (reset) {
        kheap.peak = kheap.in_use;
        aheap.peak = aheap.in_use;
        peak_total = kheap.in_use + aheap.in_use;
    }
    *k_peak = kheap.peak;
    *a_peak = aheap.peak;
    *total = peak_total;
    unlock();
}

// top（os/kernel/sysstat.c）: カーネル用・アプリ用のヒープの大きさと使っている量
void malloc_heap_info(size_t *k_arena, size_t *k_in_use, size_t *a_arena, size_t *a_in_use)
{
    lock();
    *k_arena = kheap.arena;
    *k_in_use = kheap.in_use;
    *a_arena = aheap.arena;
    *a_in_use = aheap.in_use;
    unlock();
}

struct mallinfo mallinfo(void)
{
    lock();
    struct mallinfo mi = {0};
    mi.arena = kheap.arena + aheap.arena;
    mi.uordblks = kheap.in_use + aheap.in_use;
    mi.fordblks = mi.arena - mi.uordblks;
    unlock();
    return mi;
}

// ---- アプリ用のヒープ（heap.h） ----

// カーネル用のヒープの終わりの空き（コンパイルのゾーンなど、使い終わった大きなもの）を sbrk で返し、
// アプリ用のヒープが使えるようにする。途中の空きは返せない
#define TRIM_MIN    (1024 * 1024)

static void trim_kernel(void)
{
    block_t *s = kheap.sentinel;
    if (!s || !(s->size & F_PREV_FREE))
        return;
    block_t *b = prev_phys(s);
    uintptr_t brk = memory_heap_brk();
    uintptr_t new_brk = (uintptr_t)b + HDR;     // b を新しい番兵にする
    if (brk - new_brk < TRIM_MIN)
        return;
    remove_free(&kheap, b);
    b->size = 0 | (b->size & F_PREV_FREE);      // 使用中で大きさ 0（前は空きではない。空きはつながっている）
    kheap.sentinel = b;
    kheap.arena -= brk - new_brk;
    sbrk(-(ptrdiff_t)(brk - new_brk));
}

bool malloc_app_begin(void)
{
    lock();
    if (app_on || nest > 0) {
        // 入れ子: 外側のアプリのヒープを退避して、空のヒープから始める（system() はカーネル用で確保しているので、
        // ここに来るとき kernel_depth は 0 でない）
        uintptr_t low = memory_app_low(), top = memory_app_top();     // 外側のアプリの領域
        if (nest == APP_NEST_MAX || !memory_app_push()) {
            unlock();
            return false;
        }
        saved[nest].heap = aheap;
        saved[nest].low = low;
        saved[nest].top = top;
        saved[nest].app_on = app_on;
        saved[nest].kernel_depth = kernel_depth;
        nest++;
        aheap = (heap_t){.app = true};
    } else {
        trim_kernel();
    }
    kernel_depth = 0;
    app_on = true;
    unlock();
    return true;
}

void malloc_app_stop(void)
{
    lock();
    app_on = false;
    kernel_depth = 0;
    unlock();
}

size_t malloc_app_discard(void)
{
    lock();
    // カーネル用のヒープに取ったアプリのもの（F_APP）で、まだ解放されていないもの
    size_t in_kernel = 0;
    for (block_t *b = kheap.first; b && bsize(b); ) {
        block_t *next = next_phys(b);
        if ((b->size & (F_APP | F_FREE)) == F_APP) {
            // 解放すると前後の空きとつながる。次に調べるのは、つながったブロックのすぐ後
            block_t *after = (next->size & F_FREE) ? next_phys(next) : next;
            in_kernel += bsize(b);
            free_locked(&kheap, payload(b));
            next = after;
        }
        b = next;
    }
    if (nest > 0)
        in_kernel = 0;  // 子の間は F_APP を作らない（alloc_any）。上の掃除は外側のアプリの終わりにだけ意味がある
    size_t leaked = aheap.in_use + in_kernel;
    last_app_arena = aheap.arena + in_kernel;
    last_app_leaked = leaked;
    uintptr_t low = memory_app_low();
    memset((void *)low, 0, memory_app_top() - low);  // 捨てたものを使うと気づけるように塗りつぶす（上の説明）
    aheap = (heap_t){.app = true};
    memory_app_reset();
    if (nest > 0) {     // 入れ子の子が終わった: 外側のアプリに戻す
        nest--;
        aheap = saved[nest].heap;
        app_on = saved[nest].app_on;
        kernel_depth = saved[nest].kernel_depth;
        memory_app_pop();
    }
    unlock();
    return leaked;
}

void malloc_kernel_begin(void)
{
    kernel_depth++;
}

void malloc_kernel_end(void)
{
    if (kernel_depth > 0)
        kernel_depth--;
}

void malloc_kernel_reset(void)
{
    kernel_depth = 0;
}

void malloc_last_app(size_t *arena, size_t *leaked)
{
    lock();
    *arena = last_app_arena;
    *leaked = last_app_leaked;
    unlock();
}

void malloc_stats(void)
{
    struct mallinfo mi = mallinfo();
    fprintf(stderr, "max system bytes = %10zu\n", mi.arena);
    fprintf(stderr, "system bytes     = %10zu\n", mi.arena);
    fprintf(stderr, "in use bytes     = %10zu\n", mi.uordblks);
}

int mallopt(int param, int value)
{
    (void)param;
    (void)value;
    return 0;
}
