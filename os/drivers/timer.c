// ARM 汎用タイマー（EL1 物理タイマー CNTP）で TIMER_HZ のティックを作る
// 出典: ARM DDI 0487 D11（Generic Timer）、D13.8（CNTP_CTL_EL0 など）

#include "timer.h"
#include "../kernel/smp.h"
#include "../kernel/tryboot.h"
#include "../kernel/watchdog.h"
#include "led.h"
#include "uart.h"
#include "../kernel/sysreg.h"
#include "../libc/signal_os.h"

// DAIF の I（IRQ を止めている）。Arm ARM の PSTATE.DAIF（MRS で読むとビット 7）
#define DAIF_I (1u << 7)

// CNTP_CTL_EL0 のビット
#define CNTP_CTL_ENABLE     (1U << 0)

static volatile uint64_t ticks;
static uint64_t interval;
static bool running;            // ティックの割り込みが動いている（cpu_idle で眠っても起きられる）
static uint64_t idle_count;     // cpu_idle で眠っていた時間（CNTPCT の数）

void timer_init(void)
{
    // CNTFRQ_EL0 はファームウェア（実機）または QEMU が設定済み
    interval = read_sysreg(cntfrq_el0) / TIMER_HZ;
    write_sysreg(cntp_cval_el0, read_sysreg(cntpct_el0) + interval);
    write_sysreg(cntp_ctl_el0, CNTP_CTL_ENABLE);
    isb();
    running = true;
}

// コア 0 を次の割り込みまで眠らせる（wfi。省電力）。割り込みはすべてコア 0 に届くので（タイマーは
// 1 / TIMER_HZ 秒ごと、UART・USB の受信はすぐ）、待つ間に呼べば、遅れはせいぜい 1 ティック。
// wfi は PSTATE.I で割り込みを止めていても、割り込みが来れば起きる（ARM DDI 0487 D1.16.2）。条件を
// 調べてから眠るまでの間に割り込みが来て、次の割り込みまで寝過ごすのを避けたいときは、割り込みを
// 止めて条件を調べてから呼び、その後で割り込みを戻す（uart_wait_input）。
// コア 1〜3 には割り込みが来ないので、何もしない（呼んだ側が回り続ける）
void cpu_idle(void)
{
    if (!running || cpu_id() != 0)
        return;
    uint64_t t0 = read_sysreg(cntpct_el0);
    __asm__ volatile("dsb sy\n\twfi" ::: "memory");
    idle_count += read_sysreg(cntpct_el0) - t0;
    if (!(read_sysreg(daif) & DAIF_I))
        signal_point();     // 割り込みが入る状態で待っていたら、届いたシグナルを渡す（os/libc/signal.c）
}

// このコアのタイマーのイベントストリーム（CNTKCTL_EL1。S3_0_C14_C1_0: slimcc の内蔵アセンブラは名前を知らない）。
// 入れている間は、約 0.85ms ごと（実機 19.2MHz ÷ 2^14）に wfe が起きる（割り込みにはならない）。割り込みの来ない
// コア 1〜3 が、眠りながら時間を見るのに使う（サービスのコア、pthread の時間切れのある待ち）。
// EVNTI（ビット 7:4）= 13: カウンタのビット 13 が変わるたび、EVNTEN（ビット 2）。
// 出典: Arm Architecture Reference Manual（CNTKCTL_EL1、D11.2.4 の Event streams）
#define EVNT_BITS   (0xFUL << 4 | 1UL << 3 | 1UL << 2)
#define EVNT_ON     (13UL << 4 | 1UL << 2)

void cpu_event_stream(bool on)
{
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C14_C1_0" : "=r"(v));
    v = (v & ~EVNT_BITS) | (on ? EVNT_ON : 0);
    __asm__ volatile("msr S3_0_C14_C1_0, %0\n\tisb" : : "r"(v));
}

// wfe などで眠った時間を、そのコアの眠っていた時間に足す（top。コア 0 は cpu_idle と同じもの、コア 1〜3 は smp.c）
void cpu_idle_account(uint64_t ticks)
{
    if (cpu_id() == 0)
        idle_count += ticks;
    else
        smp_add_idle(cpu_id(), ticks);
}

uint64_t timer_idle_us(void)
{
    uint64_t freq = read_sysreg(cntfrq_el0);
    return idle_count / freq * 1000000 + idle_count % freq * 1000000 / freq;
}

void timer_handle_irq(void)
{
    // 比較値を一定間隔で進めて、割り込み処理の遅れが累積しないようにする
    uint64_t next = read_sysreg(cntp_cval_el0) + interval;
    write_sysreg(cntp_cval_el0, next);
    ticks++;
    led_tick();
    tryboot_tick();     // 試しの起動の期限（過ぎたらウォッチドッグの餌をやめる）
    watchdog_tick();    // ウォッチドッグに餌をやる（os/kernel/watchdog.c）

    // 受信した文字を拾う（実行中のアプリへの Ctrl-C もここで見つかる）
    uart_poll_rx();
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t timer_freq(void)
{
    return read_sysreg(cntfrq_el0);
}

uint64_t timer_now_us(void)
{
    uint64_t freq = read_sysreg(cntfrq_el0);
    uint64_t count = read_sysreg(cntpct_el0);
    return count / freq * 1000000 + count % freq * 1000000 / freq;
}

void timer_delay_us(uint64_t us)
{
    uint64_t end = timer_now_us() + us;
    // 1 ティックより長く残っている間は眠る（寝過ごしても次のティックで起きる）
    while (timer_now_us() + 1000000 / TIMER_HZ < end)
        cpu_idle();
    while (timer_now_us() < end)
        ;
}
