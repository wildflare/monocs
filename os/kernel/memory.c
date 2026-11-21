// メモリ管理
// カーネルの後ろから、ARM が使える RAM の終わりまでをヒープとする。カーネル用は下から上へ sbrk で、
// アプリ用は上から下へ memory_app_grow で切り出す（ぶつかったら、どちらも広げられない）。
// 割り当て・解放は os/libc/malloc.c（TLSF）。

#include <errno.h>
#include <unistd.h>
#include "memory.h"
#include "mmu.h"
#include "../drivers/mailbox.h"
#include "klog.h"
#include "../drivers/timer.h"
#include "../libc/heap.h"
#include "../drivers/uart.h"

// linker.ld で定義
extern char __heap_start[];

static uintptr_t heap_end;
static uintptr_t heap_brk;
static uintptr_t app_top, app_low;  // アプリ用のヒープ [app_low, app_top)

void memory_init(void)
{
    uint32_t base, size;

    if (!mailbox_get_arm_memory(&base, &size)) {
        uart_puts("memory: mailbox failed, system halted\n");
        uart_flush(); // 止まる前に、送信バッファの表示を送り切る
        for (;;)
            __asm__ volatile("wfe");
    }
    // QEMU は RAM が 1GB あるので、MMU でマップした範囲に収める
    heap_end = (uintptr_t)base + size;
    if (heap_end > MMU_RAM_END)
        heap_end = MMU_RAM_END;
    heap_brk = (uintptr_t)__heap_start;
    app_top = app_low = heap_end & ~(uintptr_t)0xFFF;
}

uintptr_t memory_heap_start(void) { return (uintptr_t)__heap_start; }
uintptr_t memory_heap_end(void) { return heap_end; }
size_t memory_heap_used(void) { return heap_brk - (uintptr_t)__heap_start; }
uintptr_t memory_heap_brk(void) { return heap_brk; }
uintptr_t memory_app_low(void) { return app_low; }
uintptr_t memory_app_top(void) { return app_top; }
void memory_app_reset(void) { app_low = app_top; }

// 入れ子のアプリ（system()）: 子のアプリ用のヒープは、親の領域のすぐ下から始める。終わったら（memory_app_reset
// の後に）親の領域に戻す。子の間、親の領域は広がらない（親は system() から戻るのを待っている）
#define APP_NEST_MAX 8
static uintptr_t saved_top[APP_NEST_MAX];
static int nest;

bool memory_app_push(void)
{
    if (nest == APP_NEST_MAX)
        return false;
    saved_top[nest++] = app_top;
    app_top = app_low;
    return true;
}

void memory_app_pop(void)
{
    if (nest > 0)
        app_top = saved_top[--nest];
}

// ヒープ（カーネル用とアプリ用の間）の残りが LOW_MB を切ったら dmesg に 1 回書く（2026-11-20。止まる前に気づけるように。
// LOW_MB の 2 倍より空いたら、また書けるように戻す）
#define LOW_MB 32
static bool low_warned;

static void check_low(void)
{
    size_t left = app_low - heap_brk;
    if (!low_warned && left < (size_t)LOW_MB << 20) {
        low_warned = true;
        klog_printf("memory: the heap is nearly full: %zu MB left (kernel heap %zu MB, app heap %zu MB)", left >> 20,
                    (heap_brk - (uintptr_t)__heap_start) >> 20, (app_top - app_low) >> 20);
    } else if (low_warned && left > (size_t)(2 * LOW_MB) << 20) {
        low_warned = false;
    }
}

// 確保できなかった（os/libc/malloc.c から。malloc のロックの中）。dmesg に書く（続けて失敗するので 1 秒に 1 回まで）
void malloc_out_of_memory(size_t n, bool app, size_t k_in_use, size_t a_in_use)
{
    static uint64_t last_us;
    static unsigned skipped;
    uint64_t now = timer_now_us();
    if (last_us && now - last_us < 1000000) {
        skipped++;
        return;
    }
    last_us = now;
    klog_printf("memory: cannot allocate %zu KB from the %s heap (in use: kernel %zu MB, app %zu MB; "
                "%u more failures not shown)", n >> 10, app ? "app" : "kernel", k_in_use >> 20, a_in_use >> 20,
                skipped);
    skipped = 0;
}

// アプリ用のヒープを下へ incr（16 の倍数）だけ広げ、新しい下端を返す。カーネル用とぶつかるなら (void *)-1
void *memory_app_grow(size_t incr)
{
    if (incr > app_low - heap_brk)
        return (void *)-1;
    app_low -= incr;
    check_low();
    return (void *)app_low;
}

// picolibc の malloc から呼ばれる
void *sbrk(ptrdiff_t incr)
{
    uintptr_t old = heap_brk;

    if ((incr > 0 && (uintptr_t)incr > app_low - heap_brk) ||
        (incr < 0 && (uintptr_t)-incr > heap_brk - (uintptr_t)__heap_start)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_brk += incr;
    if (incr > 0)
        check_low();
    return (void *)old;
}
