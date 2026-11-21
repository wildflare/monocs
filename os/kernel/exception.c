// 例外の振り分けと、同期例外の表示
// 出典: ARM DDI 0487 D13.2.37（ESR_EL1）

#include "klog.h"
#include <stddef.h>
#include <stdatomic.h>
#include "../curses/curses_os.h"
#include "exception.h"
#include "watchdog.h"
#include "lock.h"
#include "memory.h"
#include "mmu.h"
#include "pmu.h"
#include "app.h"
#include "smp.h"
#include "sysreg.h"
#include "../drivers/irq.h"
#include "../drivers/bcm2837.h"
#include "../drivers/mmio.h"
#include "../drivers/uart.h"
#include "fault.h"

uint64_t irq_counts[4];     // コアごとに受けた割り込みの数（top。os/kernel/sysstat.c）

// コア 0 が最後の割り込みのときにいた番地と x30（2026-11-17。シェルが止まったときに、sshd が知らせる。
// OS API の monocs_core0_where）
static volatile uint64_t core0_pc, core0_lr;

void core0_where(uint64_t *pc, uint64_t *lr)
{
    *pc = core0_pc;
    *lr = core0_lr;
}

// コア 0 がどこにいたかを、割り込みのたびに記録する（2026-11-16。アプリの起動が遅かったときに、何を待っていたかを
// dmesg に出すため。os/slimcc/run.c）。止まっていた番地と x30（呼び出し元）の組
// 仕事中のコア 1〜3 にもメールボックス 0 のビット 1 で頼み、そのコアで同じように記録する（smp_handle_irq）
static volatile bool stall_on;
static uint64_t stall_pc[STALL_MAX], stall_lr[STALL_MAX];
static uint8_t stall_core[STALL_MAX];
static _Atomic int stall_n;

void stall_sample_begin(void)
{
    atomic_store(&stall_n, 0);
    __asm__ volatile("dmb ish" ::: "memory");
    stall_on = true;
}

int stall_sample_end(const uint64_t **pc, const uint64_t **lr, const uint8_t **core)
{
    stall_on = false;
    __asm__ volatile("dmb ish" ::: "memory");
    *pc = stall_pc;
    *lr = stall_lr;
    *core = stall_core;
    int n = atomic_load(&stall_n);
    return n < STALL_MAX ? n : STALL_MAX;
}

void stall_record(int core, uint64_t pc, uint64_t lr)
{
    if (!stall_on)
        return;
    int i = atomic_fetch_add(&stall_n, 1);
    if (i >= STALL_MAX)
        return;
    stall_pc[i] = pc;
    stall_lr[i] = lr;
    stall_core[i] = (uint8_t)core;
}

static_assert(offsetof(struct trap_frame, elr) == 248);
static_assert(offsetof(struct trap_frame, esr) == 264);
static_assert(offsetof(struct trap_frame, fpsr) == 280);
static_assert(offsetof(struct trap_frame, q) == 304);
static_assert(sizeof(struct trap_frame) == 816);

extern char exception_vectors[];

// スタックがあふれたときに使う、コアごとの非常用のスタック（vectors.S の OVERFLOW_STACK と一致させる）
#define OVERFLOW_STACK  16384
#define EXC_OVERFLOW    16
alignas(16) uint8_t exception_overflow_stacks[4][OVERFLOW_STACK];

// ESR_EL1 の EC（例外クラス）
#define ESR_EC_SHIFT        26
#define ESR_EC_MASK         0x3F
#define EC_UNKNOWN          0x00
#define EC_SVC64            0x15
#define EC_INST_ABORT_CUR   0x21
#define EC_PC_ALIGN         0x22
#define EC_DATA_ABORT_CUR   0x25
#define EC_SP_ALIGN         0x26
#define EC_BRK64            0x3C

static const char *type_names[16] = {
    "Sync", "IRQ", "FIQ", "SError",
    "Sync (SP_EL0)", "IRQ (SP_EL0)", "FIQ (SP_EL0)", "SError (SP_EL0)",
    "Sync (lower EL64)", "IRQ (lower EL64)", "FIQ (lower EL64)", "SError (lower EL64)",
    "Sync (lower EL32)", "IRQ (lower EL32)", "FIQ (lower EL32)", "SError (lower EL32)",
};

static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case EC_UNKNOWN:        return "undefined instruction";
    case EC_SVC64:          return "SVC";
    case EC_INST_ABORT_CUR: return "instruction abort";
    case EC_PC_ALIGN:       return "PC alignment fault";
    case EC_DATA_ABORT_CUR: return "data abort";
    case EC_SP_ALIGN:       return "SP alignment fault";
    case EC_BRK64:          return "BRK";
    default:                return "?";
    }
}

static void dump_frame(uint64_t type, const struct trap_frame *f)
{
    uint32_t ec = (f->esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

    uart_puts("\n*** exception: ");
    uart_puts(type < 16 ? type_names[type] : "?");
    uart_puts(" / ");
    uart_puts(ec_name(ec));
    uart_puts("\n ESR ");
    uart_puthex(f->esr);
    uart_puts("  ELR ");
    uart_puthex(f->elr);
    uart_puts("\n FAR ");
    uart_puthex(f->far);
    uart_puts("  SPSR ");
    uart_puthex(f->spsr);
    // データアボートの原因（ESR の DFSC、D13.2.37）。読み出し専用のカーネル、ガードページ、0 番地
    if (ec == EC_DATA_ABORT_CUR || ec == EC_INST_ABORT_CUR) {
        uint32_t fsc = f->esr & 0x3F;
        bool guard = false;
        for (int c = 0; c < 4; c++)
            if (f->far - mmu_stack_guard(c) < MMU_STACK_GUARD_SIZE)
                guard = true;
        if (guard)
            uart_puts("\n (stack guard page: the stack overflowed)");
        else if (fsc >= 0x0C && fsc <= 0x0F)
            uart_puts("\n (write to read-only memory: the kernel's code or constants)");
        else if (f->far < 0x1000)
            uart_puts("\n (NULL pointer)");
    }
    uart_puts("\n SP  ");
    uart_puthex((uint64_t)f + sizeof(*f));
    for (int i = 0; i < 31; i++) {
        uart_puts(i % 2 == 0 ? "\n x" : "  x");
        if (i < 10)
            uart_putc('0');
        uart_putdec(i);
        uart_putc(' ');
        uart_puthex(f->x[i]);
    }
    uart_puts("\n");
}

static void handle_sync(uint64_t type, struct trap_frame *f)
{
    uint32_t ec = (f->esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

    // BRK は内容を表示して次の命令から再開する（デバッグ用のブレークポイント）
    if (ec == EC_BRK64 && type == EXC_SYNC) {
        dump_frame(type, f);
        f->elr += 4;
        return;
    }

    // コア 1〜3: 内容を表示し、そのコアの仕事を止めて待機に戻す
    if (cpu_id() != 0) {
        if (app_is_running())
            fault_record(f);    // スレッドの例外ならアプリ全体が止まる。その表示で関数名に直す（os/slimcc/run.c）
        klog_tee_begin();   // 例外の表示は記録にも残す（dmesg。os/kernel/klog.c）
        uart_puts("\n*** core ");
        uart_putdec(cpu_id());
        dump_frame(type, f);
        klog_tee_end();
        smp_fault(&f->elr);
        return;
    }

    // アプリの実行中なら、内容を表示してシェルへ戻す（curses の代わりの画面から戻してから表示する）
    if (app_is_running()) {
        curses_emergency_restore();
        fault_record(f);        // 止まった場所（アプリが終わった後で、関数名に直して出す。os/slimcc/run.c）
    }
    klog_tee_begin();
    dump_frame(type, f);
    klog_tee_end();
    if (app_is_running()) {
        app_abort_from_exception(&f->elr, APP_FAULTED);
        return;
    }
    kernel_halt("exception in the kernel");     // 数秒後に再起動する（os/kernel/watchdog.c）
}

// Ctrl-C で止めた場所と、フレームポインタ（x29）をたどった呼び出し元を表示する（デバッグ用）。
// アドレスは、GCC 版は nm build/kernel.elf、slimcc 版は build/kslim/kernel.map で関数に直せる
static void print_interrupted_at(const struct trap_frame *f)
{
    fault_record(f);        // 関数名に直した表示は、アプリが終わった後で（os/slimcc/run.c）
    uart_puts("\n");
    uart_puts(uart_interrupt_key_name());
    uart_puts(" at ");
    uart_puthex(f->elr);
    uart_puts(", called from");
    uint64_t fp = f->x[29];
    uint64_t lo = 0x80000;
    for (int i = 0; i < 8 && fp >= lo && fp < memory_heap_end() && !(fp & 7); i++) {
        const uint64_t *frame = (const uint64_t *)fp;
        uart_puts(" ");
        uart_puthex(frame[1]);
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    uart_puts("\n");
}

// スタックがあふれた（vectors.S が非常用のスタックに切り替えた）。元のスタックには戻れないので、
// 割り込みでも同期例外でも、アプリ（コア 1〜3 は仕事）を止める。カーネルなら止まる
static void handle_overflow(uint64_t type, struct trap_frame *f)
{
    if (cpu_id() == 0 && app_is_running())
        curses_emergency_restore();
    if (app_is_running())
        fault_record(f);
    klog_tee_begin();
    uart_puts("\n*** stack overflow");
    if (cpu_id() != 0) {
        uart_puts(" on core ");
        uart_putdec(cpu_id());
    }
    dump_frame(type, f);
    klog_tee_end();
    if (cpu_id() != 0) {
        smp_fault(&f->elr);
        return;
    }
    if (app_is_running()) {
        app_abort_from_exception(&f->elr, APP_FAULTED);
        return;
    }
    kernel_halt("stack overflow in the kernel");
}

void handle_exception(uint64_t type, struct trap_frame *frame)
{
    if (type & EXC_OVERFLOW) {
        handle_overflow(type & ~(uint64_t)EXC_OVERFLOW, frame);
        return;
    }
    switch (type) {
    case EXC_SYNC:
    case EXC_SYNC + 4:
        handle_sync(type, frame);
        return;
    case EXC_IRQ:
        irq_counts[cpu_id() & 3]++;     // コアごとの割り込みの数（top）
        if (cpu_id() != 0) {
            smp_handle_irq(&frame->elr); // コア 1〜3 はメールボックス（仕事の中断）だけ
            return;
        }
        pmu_set_irq_frame(frame->elr, frame->x[29], frame->x[30]);  // perf record（os/kernel/pmu.c）
        core0_pc = frame->elr;
        core0_lr = frame->x[30];
        if (stall_on) {
            stall_record(0, frame->elr, frame->x[30]);
            for (int c = 1; c < SMP_MAX_CORES; c++)
                if (smp_core_state(c) == 2 || smp_core_state(c) == 3)
                    mmio_write32(CORE0_MBOX0_SET + 16 * c, STALL_MBOX_BIT);
        }
        irq_dispatch();
        // ロックを持っている間（malloc や SD カードの読み書きの途中）は、次のタイマー割り込みまで待つ
        if (app_interrupt_pending() && lock_depth(0) == 0) {
            enum app_result why = app_pending_reason();     // Ctrl-C か、スレッドからの exit・例外
            curses_emergency_restore();
            if (why == APP_INTERRUPTED)
                print_interrupted_at(frame);
            app_abort_from_exception(&frame->elr, why);
        } else if (lock_depth(0) == 0 && app_timeout_due()) {
            // シェルの timeout の期限（Linux の timeout と同じく、何も表示しない。終了の値 124 は timeout が返す）
            curses_emergency_restore();
            app_abort_from_exception(&frame->elr, APP_TIMEDOUT);
        }
        return;
    default:
        klog_tee_begin();
        dump_frame(type, frame);
        kernel_halt("unexpected exception");
    }
}

void exception_init(void)
{
    // vectors.S がスタックのあふれを調べるため、このコアのガードページの先頭を TPIDR_EL1 に置く
    write_sysreg(tpidr_el1, mmu_stack_guard(cpu_id()));
    write_sysreg(vbar_el1, exception_vectors);
    isb();
}
