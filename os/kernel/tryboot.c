// 新しいカーネルを試しに起動して、だめなら前のカーネルに戻る（2026-11-16。Wi-Fi だけでつないでいるとき、起動しない
// カーネルを送っても戻れるように）。
//
// 流れ（tools/uart/nos.py の kernel が行う）:
//   1. 新しいカーネルを /boot/kernel8.trial.img に置く（/boot/kernel8.img は触らない）
//   2. /tryboot.txt を、config.txt の kernel= を kernel8.trial.img に替え、cmdline=tryboot-cmdline.txt（中身は
//      "monocs.tryboot"）を足したものにする
//   3. reboot tryboot: ファームウェアは次の 1 回だけ、config.txt の代わりに tryboot.txt を読む（Zero 2 W で効くことを
//      実機で確かめた。2 回目からは config.txt に戻る）
//   4. 新しいカーネル（ここ）は、コマンドラインに monocs.tryboot があれば試しの起動とわかる。ウォッチドッグ
//      （os/kernel/watchdog.c。2026-11-20 からいつも入れる）の餌は、TRYBOOT_LIMIT 秒たっても tryboot confirm
//      （ssh が通ったことの確認）が来なければやめる → リセット → ファームウェアは config.txt で前のカーネルを起動する。
//      固まって割り込みも止まったときは、餌が来ないので約 16 秒でリセットされる
//   5. 確認されたら、nos.py が kernel8.trial.img を kernel8.img に写す
// ウォッチドッグの扱いは Linux の drivers/watchdog/bcm2835_wdt.c（bcm2835_wdt_start・stop）による。
// コマンドラインは、メールボックスの Get command line（0x00050001。Raspberry Pi のファームウェアのメールボックスの
// プロパティの説明）で読む。

#include <stdatomic.h>
#include <string.h>
#include "tryboot.h"
#include "klog.h"
#include "../drivers/bcm2837.h"
#include "../drivers/mmio.h"
#include "../drivers/mailbox.h"
#include "../drivers/timer.h"
#include "watchdog.h"

#define TRYBOOT_LIMIT   90                  // 確認を待つ秒数（Wi-Fi がつながるまで約 10 秒、ssh まで 15 秒ほど）

static char cmdline[256];
static atomic_bool active;
static bool confirmed;
static uint64_t start_us;

void tryboot_init(void)
{
    mailbox_get_cmdline(cmdline, sizeof cmdline);
    if (!strstr(cmdline, "monocs.tryboot"))
        return;
    start_us = timer_now_us();
    atomic_store(&active, true);
    klog_printf("tryboot: trial boot; back to the previous kernel unless confirmed within %d s", TRYBOOT_LIMIT);
}

void tryboot_tick(void)
{
    if (!atomic_load(&active))
        return;
    // 期限を過ぎたら餌をやめる: 約 16 秒でリセットされ、前のカーネルに戻る
    if (timer_now_us() - start_us >= (uint64_t)TRYBOOT_LIMIT * 1000000)
        watchdog_starve();
}

bool tryboot_active(void)
{
    return atomic_load(&active);
}

bool tryboot_confirm(void)
{
    if (!atomic_exchange(&active, false))
        return false;
    confirmed = true;
    klog_printf("tryboot: confirmed");
    return true;
}

int tryboot_seconds_left(void)
{
    if (!atomic_load(&active))
        return -1;
    int64_t left = (int64_t)TRYBOOT_LIMIT - (int64_t)((timer_now_us() - start_us) / 1000000);
    return left < 0 ? 0 : (int)left;
}

bool tryboot_confirmed(void)
{
    return confirmed;
}

const char *tryboot_cmdline(void)
{
    return cmdline;
}
