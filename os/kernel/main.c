// カーネルのエントリポイント

#include <stdint.h>
#include <stdio.h>
#include "app.h"
#include "exception.h"
#include "memory.h"
#include "mmu.h"
#include "smp.h"
#include "sysreg.h"
#include "tls.h"
#include "../fs/vfs.h"
#include "../shell/shell.h"
#include "../drivers/irq.h"
#include "../drivers/led.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../drivers/usb.h"
#include "../net/net.h"
#include "tryboot.h"
#include "watchdog.h"

// boot_el: boot.S が渡す、起動時の例外レベル
void kernel_main(uint64_t boot_el)
{
    // MMU を有効にするまでは picolibc を呼ばない（memset などが Device メモリで例外になる）
    uart_init();
    led_init();
    led_set(true);  // 起動直後は点灯。タイマー割り込みが動き出すと点滅に変わる
    uart_puts("\nMonocs: hello from Raspberry Pi Zero 2 W\n");
    exception_init();
    mmu_init();
    tls_init(0);    // _Thread_local の変数（picolibc の errno など）は、ここから使える
    memory_init();

    printf("boot EL %llu, SCTLR_EL1 %#llx\n",
           (unsigned long long)boot_el, (unsigned long long)read_sysreg(sctlr_el1));
    printf("heap %#lx-%#lx (%lu MB)\n",
           (unsigned long)memory_heap_start(), (unsigned long)memory_heap_end(),
           (unsigned long)((memory_heap_end() - memory_heap_start()) >> 20));

    // 実行中のアプリは Ctrl-C で止める
    uart_set_ctrl_c_handler(app_request_interrupt);

    irq_init();
    uart_enable_irq();      // ここから、UART の送受信は割り込みとバッファを通す
    timer_init();
    tryboot_init();         // 試しの起動か（os/kernel/tryboot.c。期限を過ぎたらウォッチドッグの餌をやめる）
    watchdog_init();        // 固まったら約 16 秒で再起動（os/kernel/watchdog.c）
    // 動作中の目印として ACT LED を 1 秒周期で点滅させる（UART なしで実機の起動を確認できる）
    led_set_blink(TIMER_HZ / 2);
    irq_enable();
    printf("timer: CNTFRQ %llu Hz, tick %d Hz\n",
           (unsigned long long)timer_freq(), TIMER_HZ);

    if (usb_init()) {
        net_init();
        printf("usb: console (CDC-ACM) and ethernet (CDC-ECM, 192.168.7.2)\n");
    } else {
        printf("usb: not available\n");
    }

    smp_init();
    printf("smp: %d cores\n", smp_cores());

    if (vfs_init()) {
        printf("vfs: SD card mounted\n");
    } else {
        printf("vfs: mount failed\n");
        led_set_blink(TIMER_HZ / 10);   // SD カードが使えないことを、速い点滅で知らせる
    }

    shell_main();
}
