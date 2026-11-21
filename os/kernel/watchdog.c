// ウォッチドッグ（2026-11-20。Wi-Fi だけでつないでいるとき、固まった・止まった実機を電源の入れ直しなしで戻す）
//
//   - 起動したらすぐ入れ、コア 0 のタイマー割り込み（10ms ごと）が餌をやる（watchdog_tick）。割り込みが止まって
//     固まると、餌が来ないので約 16 秒（20 ビットの最大）でリセットされる。
//   - カーネルが止まるとき（例外など）は kernel_halt: 表示を送り切り、記録に終わり方を残して（os/kernel/klog.c。
//     再起動をまたいで残る）、HALT_DELAY_S 秒後に再起動する。
//   - 試しの起動（os/kernel/tryboot.c）の期限を過ぎたら餌をやめる（watchdog_starve）→ リセット → 前のカーネル。
//   - シェルの `watchdog off` で止められる（固まった様子を UART・デバッガで見たいとき。次の起動でまた入る）。
// ウォッチドッグの扱いは Linux の drivers/watchdog/bcm2835_wdt.c（bcm2835_wdt_start・stop）による。
#include <stdatomic.h>
#include "watchdog.h"
#include "klog.h"
#include "sysreg.h"
#include "../drivers/bcm2837.h"
#include "../drivers/mmio.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"

#define WDOG_TICKS      0xFFFFFU            // 1/65536 秒単位。20 ビットの最大 = 約 16 秒
#define PM_RSTC_RESET   0x00000102U         // ウォッチドッグを止める（Linux の PM_RSTC_RESET）
#define HALT_DELAY_S    5                   // 止まってから再起動するまで（UART の表示を読む間）

static atomic_bool enabled;
static atomic_bool starved;

static void wdog_start(void)
{
    mmio_write32(PM_WDOG, PM_PASSWORD | (WDOG_TICKS & 0xFFFFF));
    mmio_write32(PM_RSTC, (mmio_read32(PM_RSTC) & PM_RSTC_WRCFG_CLR) | PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET);
}

void watchdog_init(void)
{
    // QEMU（raspi3b）の電源の管理の真似は、ウォッチドッグを入れた（PM_RSTC に FULL_RESET を書いた）瞬間に
    // リセットする（時間を数えない）。実機の BCM2837 のタイマーは 19.2MHz、QEMU は 62.5MHz なので、それで見分けて
    // 実機でないときは使わない（止まったら止まったまま。QEMU の試験は表示を読める）
    if (read_sysreg(cntfrq_el0) != 19200000) {
        klog_printf("watchdog: not used (not the BCM2837's 19.2 MHz timer; QEMU?)");
        return;
    }
    wdog_start();
    atomic_store(&enabled, true);
}

void watchdog_tick(void)
{
    if (atomic_load(&enabled) && !atomic_load(&starved))
        mmio_write32(PM_WDOG, PM_PASSWORD | (WDOG_TICKS & 0xFFFFF));    // 餌をやる
}

void watchdog_starve(void)
{
    atomic_store(&starved, true);
}

void watchdog_set(bool on)
{
    if (on && read_sysreg(cntfrq_el0) != 19200000)
        return;     // 実機でない（watchdog_init）
    if (on) {
        atomic_store(&starved, false);
        wdog_start();
    } else {
        mmio_write32(PM_RSTC, PM_PASSWORD | PM_RSTC_RESET);
    }
    atomic_store(&enabled, on);
}

bool watchdog_enabled(void)
{
    return atomic_load(&enabled);
}

[[noreturn]] void kernel_halt(const char *why)
{
    irq_disable();
    klog_set_end(KLOG_END_HALT);
    klog_tee_begin();       // 止まるときの表示も記録に（次の起動の /var/log/dmesg.crash で読める）
    uart_puts(why);
    if (atomic_load(&enabled)) {
        uart_puts(", system halted; restarting in ");
        uart_putdec(HALT_DELAY_S);
        uart_puts(" s (watchdog off: stays halted)\n");
    } else {
        uart_puts(", system halted\n");
    }
    klog_tee_end();
    klog_persist_flush();
    uart_flush();   // 止まる前に、送信バッファの表示を送り切る
    if (atomic_load(&enabled)) {
        uint64_t end = timer_now_us() + (uint64_t)HALT_DELAY_S * 1000000;
        while (timer_now_us() < end)
            ;
        mmio_write32(PM_WDOG, PM_PASSWORD | 10);   // 10 / 65536 秒後にリセット
        mmio_write32(PM_RSTC, (mmio_read32(PM_RSTC) & PM_RSTC_WRCFG_CLR) | PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET);
    }
    for (;;)
        __asm__ volatile("wfe");
}
