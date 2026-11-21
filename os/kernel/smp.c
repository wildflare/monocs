// マルチコア: コア 1〜3 を起こし、関数を動かす
//
// 起こし方: Raspberry Pi のファームウェアは、コア 1〜3 をスピンテーブルで待たせている。
// コア n の飛び先は 0xD8 + 8n 番地に書く（デバイスツリーの cpu-release-addr。bcm2710-rpi-zero-2-w.dtb で
// 0xE0、0xE8、0xF0 を確認）。書いた後、キャッシュから書き戻して sev で起こす。QEMU の raspi3b も同じ番地。
//
// コア 1〜3 は secondary_entry（boot.S）から EL1 に降り、secondary_main で MMU を有効にして仕事を待つ。
// 仕事の中断（アプリが終わったとき）は、ARM ローカル周辺のメールボックス割り込みで知らせる
// （出典: BCM2836 ARM-local peripherals（QA7_rev3.4）4.7、4.8 節）。
// コア間のキャッシュの一貫性は、ページ表の Inner Shareable（mmu.c）と、ファームウェアが立てる
// CPUECTLR_EL1.SMPEN に頼る。

#include <setjmp.h>
#include "klog.h"
#include "smp.h"
#include "pmu.h"
#include "../libc/heap.h"
#include "exception.h"
#include "lock.h"
#include "app.h"
#include "mmu.h"
#include "sysreg.h"
#include "tls.h"
#include "../drivers/bcm2837.h"
#include "../drivers/mmio.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"

#define SPIN_TABLE 0xD8

enum { CORE_OFFLINE, CORE_IDLE, CORE_RUNNING, CORE_DONE, CORE_FAILED };

static struct {
    alignas(CACHE_LINE) volatile int state;     // コアごとに別のキャッシュラインから（偽の共有を避ける）
    volatile bool abort;
    volatile bool reserved;     // サービス用（os/kernel/svc.c）。アプリには見せず、アプリの終わりでも止めない
    smp_fn fn;
    void *arg;
    int level;                  // 仕事を始めたアプリの入れ子の深さ（app_depth。smp_abort_level で使う）
    bool thread;                // pthread のスレッド（例外でアプリ全体を止める）
    jmp_buf idle;  // 仕事を中断したときに戻る場所（そのコアの secondary_main）
} cores[SMP_MAX_CORES];

extern char secondary_entry[];

static void sev(void) { __asm__ volatile("dsb sy\n\tsev" ::: "memory"); }
static void wfe(void) { __asm__ volatile("wfe" ::: "memory"); }

// コア 1〜3 が眠っていた（仕事を待っていた）時間（カウント。top。コア 0 は timer.c の cpu_idle が数える）
static _Atomic uint64_t idle_ticks[SMP_MAX_CORES];
static _Atomic uint64_t idle_since[SMP_MAX_CORES];     // 今待っているなら、待ち始めた時刻（0 = 待っていない）

static inline uint64_t ticks_now(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void smp_add_idle(int core, uint64_t ticks)
{
    if (core >= 1 && core < SMP_MAX_CORES)
        idle_ticks[core] += ticks;
}

uint64_t smp_idle_ticks(int core)
{
    if (core < 1 || core >= SMP_MAX_CORES)
        return 0;
    uint64_t since = idle_since[core], t = idle_ticks[core];
    return since ? t + (ticks_now() - since) : t;   // 待っている途中の分も
}

// 0 = 起きていない、1 = 待っている、2 = 仕事、3 = スレッド、4 = サービスのために取ってある
int smp_core_state(int core)
{
    if (core < 1 || core >= SMP_MAX_CORES || cores[core].state == CORE_OFFLINE)
        return 0;
    if (cores[core].reserved)
        return 4;
    if (cores[core].state != CORE_RUNNING)
        return 1;
    return cores[core].thread ? 3 : 2;
}
static void dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }

// コア 1〜3 の C の入口（boot.S の secondary_entry から）
[[noreturn]] void secondary_main(uint64_t core)
{
    exception_init();
    mmu_enable();
    tls_init(core);
    // このコアのメールボックス 0 の割り込みを許可する（中断の知らせ）
    mmio_write32(CORE0_MBOX_IRQCNTL + 4 * core, 1);
    cores[core].state = CORE_IDLE;
    sev();

    setjmp(cores[core].idle);
    irq_disable();
    for (;;) {
        cpu_event_stream(false);    // 仕事の途中で止められたとき（pthread の時間切れのある待ちなど）に残さない
        uint64_t t0 = ticks_now();
        idle_since[core] = t0;
        while (cores[core].state != CORE_RUNNING)
            wfe();
        idle_since[core] = 0;
        idle_ticks[core] += ticks_now() - t0;
        dmb();
        irq_enable(); // 仕事中だけ、中断の割り込みを受ける
        malloc_kernel_reset();  // 前の仕事が malloc_kernel_begin の中で止められていても戻す
        pmu_job_begin();
        cores[core].fn(cores[core].arg);
        irq_disable();
        pmu_job_end();
        dmb();
        cores[core].state = CORE_DONE;
        sev();
    }
}

void smp_init(void)
{
    // スピンテーブルに飛び先を書き、キャッシュから書き戻す（待っているコアはキャッシュを使っていない）
    mmu_map_page0(true);
    for (int core = 1; core < SMP_MAX_CORES; core++) {
        // 小さな固定の番地なので、コンパイラの範囲検査を避けて str で書く
        uintptr_t slot = SPIN_TABLE + 8 * core;
        __asm__ volatile("str %0, [%1]\n\tdc civac, %1"
                         :: "r"((uintptr_t)secondary_entry), "r"(slot) : "memory");
    }
    dsb_sy();
    mmu_map_page0(false);
    sev();

    // 起きるのを待つ（最大 1 秒）
    uint64_t start = timer_now_us();
    while (timer_now_us() - start < 1000000) {
        int n = 0;
        for (int core = 1; core < SMP_MAX_CORES; core++)
            n += cores[core].state != CORE_OFFLINE;
        if (n == SMP_MAX_CORES - 1)
            break;
    }
}

int smp_cores(void)
{
    int n = 1;
    for (int core = 1; core < SMP_MAX_CORES; core++)
        n += cores[core].state != CORE_OFFLINE && !cores[core].reserved;
    return n;
}

void smp_reserve(int core, bool reserved)
{
    if (core >= 1 && core < SMP_MAX_CORES)
        cores[core].reserved = reserved;
}

bool smp_busy(int core)
{
    return core >= 1 && core < SMP_MAX_CORES && cores[core].state == CORE_RUNNING;
}

static bool run_on(int core, smp_fn fn, void *arg, bool thread);

bool smp_run(int core, smp_fn fn, void *arg)
{
    if (core < 1 || core >= SMP_MAX_CORES || cores[core].reserved)
        return false;
    return run_on(core, fn, arg, false);
}

bool smp_run_thread(int core, smp_fn fn, void *arg)
{
    if (core < 1 || core >= SMP_MAX_CORES || cores[core].reserved)
        return false;
    return run_on(core, fn, arg, true);
}

bool smp_run_reserved(int core, smp_fn fn, void *arg)
{
    if (core < 1 || core >= SMP_MAX_CORES || !cores[core].reserved)
        return false;
    return run_on(core, fn, arg, false);
}

static bool run_on(int core, smp_fn fn, void *arg, bool thread)
{
    int st = cores[core].state;
    if (st == CORE_OFFLINE || st == CORE_RUNNING)
        return false;
    cores[core].fn = fn;
    cores[core].arg = arg;
    cores[core].level = app_depth();
    cores[core].thread = thread;
    cores[core].abort = false;
    dmb();
    cores[core].state = CORE_RUNNING;
    sev();
    return true;
}

bool smp_wait(int core)
{
    if (core < 1 || core >= SMP_MAX_CORES || cores[core].state == CORE_OFFLINE)
        return false;
    while (cores[core].state == CORE_RUNNING)
        wfe();
    dmb();
    bool ok = cores[core].state == CORE_DONE;
    cores[core].state = CORE_IDLE;
    return ok;
}

// 入れ子の深さが min_level 以上のアプリが始めた仕事を止める（外側のアプリの仕事は続ける）
static bool mine(int core, int min_level)
{
    return !cores[core].reserved && cores[core].level >= min_level;
}

void smp_abort_level(int min_level)
{
    for (int core = 1; core < SMP_MAX_CORES; core++) {
        if (cores[core].state != CORE_RUNNING || !mine(core, min_level))
            continue;
        cores[core].abort = true;
        dmb();
        mmio_write32(CORE0_MBOX0_SET + 16 * core, 1);
    }
    for (int core = 1; core < SMP_MAX_CORES; core++) {
        if (!mine(core, min_level))
            continue;
        uint64_t start = timer_now_us();
        while (cores[core].state == CORE_RUNNING) {
            // 中断を後回しにしている間（ロック、アプリから呼んだコンパイル）は長く待つ。待たずに先へ進むと、
            // アプリ用のヒープを捨てた後も、そのコアがアプリのものを使い続ける
            uint64_t limit = lock_depth(core) > 0 ? 60000000 : 5000000;
            if (timer_now_us() - start > limit) {
                klog_tee_begin();
                uart_puts("\n*** smp: core ");
                uart_putdec(core);
                uart_puts(" does not stop\n");
                klog_tee_end();
                break;
            }
        }
        if (cores[core].state != CORE_OFFLINE && cores[core].state != CORE_RUNNING)
            cores[core].state = CORE_IDLE;
    }
}

// 中断・例外のとき、例外から戻る先。そのコアの待機に戻る
static int abort_result[SMP_MAX_CORES];

[[noreturn]] static void abort_return(void)
{
    int core = cpu_id();
    irq_disable();
    lock_release_all(core); // 同期例外のときは、ロックを持ったまま止まっていることがある
    pmu_job_end();          // 数えていたら、止められるまでの分を足す（os/kernel/pmu.c）
    dmb();
    cores[core].state = abort_result[core];
    sev();
    longjmp(cores[core].idle, 1);
}

void smp_handle_irq(uint64_t *elr)
{
    int core = cpu_id();
    uint32_t src = mmio_read32(CORE0_IRQ_SOURCE + 4 * core);
    if (src & IRQ_SOURCE_PMU) {
        // elr は例外のフレームの中を指す（exception.c の smp_handle_irq(&frame->elr)）
        const struct trap_frame *f = (const struct trap_frame *)((const char *)elr - offsetof(struct trap_frame, elr));
        pmu_set_irq_frame(*elr, f->x[29], f->x[30]);
        pmu_handle_irq();
    }
    if (src & IRQ_SOURCE_MBOX0) {
        uint32_t bits = mmio_read32(CORE0_MBOX0_CLR + 16 * core);
        mmio_write32(CORE0_MBOX0_CLR + 16 * core, 0xFFFFFFFF);
        if (bits & STALL_MBOX_BIT) {
            const struct trap_frame *f = (const struct trap_frame *)((const char *)elr - offsetof(struct trap_frame, elr));
            stall_record(core, *elr, f->x[30]);
        }
        // ロックを持っている間は後回しにし、最後のロックを外したとき（smp_check_abort）に止める
        if (cores[core].abort && cores[core].state == CORE_RUNNING && lock_depth(core) == 0) {
            abort_result[core] = CORE_FAILED;
            *elr = (uint64_t)abort_return;
        }
    }
}

void smp_exit_job(void)
{
    int core = cpu_id();
    abort_result[core] = CORE_FAILED;
    abort_return();
}

void smp_check_abort(void)
{
    int core = cpu_id();
    if (core != 0 && cores[core].abort && cores[core].state == CORE_RUNNING) {
        abort_result[core] = CORE_FAILED;
        abort_return();
    }
}

void smp_fault(uint64_t *elr)
{
    int core = cpu_id();
    if (cores[core].state == CORE_RUNNING) {
        abort_result[core] = CORE_FAILED;
        *elr = (uint64_t)abort_return;
        if (cores[core].thread)
            app_request_abort(APP_FAULTED, 128 + 11, false);    // スレッドの例外: アプリ全体を止める（139）
        return;
    }
    klog_tee_begin();
    uart_puts("smp: fault outside of a job, core halted\n");
    uart_flush(); // 止まる前に、送信バッファの表示を送り切る
    for (;;)
        wfe();
}
