// 割り込みの振り分け
// BCM2837 には GIC がなく、コアごとの割り込み要因は ARM ローカル周辺で読む。
// 出典: BCM2836 ARM-local peripherals（QA7_rev3.4）4.6 節、4.10 節

#include "irq.h"
#include "bcm2837.h"
#include "mmio.h"
#include "timer.h"
#include "uart.h"
#include "usb.h"
#include "wifi.h"
#include "../net/net.h"
#include "../kernel/pmu.h"

// CORE0_TIMER_IRQCNTL / CORE0_IRQ_SOURCE のビット
// EL2 から EL1 に降りて非セキュアで動くため、物理タイマーは CNTPNSIRQ を使う
#define TIMER_CNTPNSIRQ     (1U << 1)

void irq_init(void)
{
    // 非セキュア物理タイマーの割り込みをコア 0 の IRQ に配送する
    mmio_write32(CORE0_TIMER_IRQCNTL, TIMER_CNTPNSIRQ);
    pmu_init();     // 性能カウンタのあふれを、各コアの IRQ に
}

void irq_dispatch(void)
{
    uint32_t source = mmio_read32(CORE0_IRQ_SOURCE);

    if (source & IRQ_SOURCE_PMU) {
        pmu_handle_irq();   // 性能カウンタのあふれ（シェルの perf の間。os/kernel/pmu.c）
        if (!(source & (TIMER_CNTPNSIRQ | IRQ_SOURCE_GPU)))
            return;
    }
    if (source & (TIMER_CNTPNSIRQ | IRQ_SOURCE_GPU)) {
        if (source & TIMER_CNTPNSIRQ) {
            timer_handle_irq();
            net_timer();    // lwIP のタイマー（TCP の再送など）
        }
        if ((source & IRQ_SOURCE_GPU) && (mmio_read32(IRQ_PENDING_2) & IRQ_UART))
            uart_handle_irq();
        if ((source & IRQ_SOURCE_GPU) && (mmio_read32(IRQ_PENDING_1) & IRQ_USB))
            usb_handle_irq();
        if ((source & IRQ_SOURCE_GPU) && (mmio_read32(IRQ_PENDING_2) & IRQ_EMMC))
            wifi_handle_irq();  // 内蔵 Wi-Fi のチップが「読むものがある」と知らせた
        // サービスのコア（os/kernel/svc.c）は wfe で眠っている。タイマーやネットワークの受信のたびに起こす
        __asm__ volatile("dsb sy\n\tsev" ::: "memory");
        return;
    }

    uart_puts("\n*** unknown IRQ source ");
    uart_puthex(source);
    uart_puts("\n");
}
