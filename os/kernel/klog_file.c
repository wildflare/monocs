// カーネルの記録（dmesg）を SD カードにも残す（2026-11-16。Wi-Fi だけでつないでいると、固まって再起動した後に、何が
// 起きたかを知る手段がなかったため）。
//   /var/log/dmesg     今の起動の記録。シェルがプロンプトで待つ間に、増えた分を 5 秒に 1 回まで書き足す。アプリの
//                      終わり・reboot・poweroff の前にはすぐ書く。1MB を超えたら、今の記録（64KB）で書き直す
//   /var/log/dmesg.1   前の起動の記録（起動のたびに移す。固まってリセットされたときは、最後の 5 秒ほどは入らない）
//   /var/log/dmesg.crash  前の起動がふつうに終わらなかった（固まってウォッチドッグでリセット・カーネルが止まった）
//                      ときの、RAM に残っていた最後の記録（64KB まで。os/kernel/klog.c が再起動をまたいで残す。
//                      SD カードに書く前の、止まる直前のものも入る。2026-11-20）
// ファイルを書くのはコア 0 のシェルの文脈だけ（アプリが動いていないとき）。
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <sys/stat.h>
#include "klog.h"
#include "klog_file.h"
#include "../drivers/timer.h"

#define LOG_DIR     "/var/log"
#define LOG_PATH    "/var/log/dmesg"
#define LOG_OLD     "/var/log/dmesg.1"
#define LOG_CRASH   "/var/log/dmesg.crash"
#define LOG_MAX     (1024 * 1024)
#define SYNC_US     5000000

static bool ready;
static uint64_t pos;            // klog のどこまで書いたか
static uint64_t last_us;
static long file_size;

static bool append(void)
{
    static char buf[4096];
    FILE *f = fopen(LOG_PATH, file_size > LOG_MAX ? "w" : "a");
    if (!f)
        return false;
    if (file_size > LOG_MAX) {
        // 大きくなりすぎた: 今残っている記録（64KB まで）で書き直す
        file_size = 0;
        pos = 0;
    }
    size_t n;
    while ((n = klog_read_since(&pos, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, n, f);
        file_size += (long)n;
    }
    fclose(f);
    return true;
}

void klog_file_boot(void)
{
    mkdir("/var", 0755);
    mkdir(LOG_DIR, 0755);
    rename(LOG_PATH, LOG_OLD);      // 前の起動の記録（なければ何もしない。dmesg.1 は置き換える）
    // 前の起動の終わり方（RAM に残った記録から）
    const char *text;
    size_t len;
    int how;
    if (!klog_previous(&text, &len, &how)) {
        klog_printf("previous boot: no log left in RAM (the power was off)");
    } else if (how == KLOG_END_REBOOT || how == KLOG_END_POWEROFF) {
        klog_printf("previous boot: ended by %s", how == KLOG_END_REBOOT ? "reboot" : "poweroff");
    } else {
        FILE *f = fopen(LOG_CRASH, "w");
        if (f) {
            fwrite(text, 1, len, f);
            fclose(f);
        }
        klog_printf("previous boot: %s; its last log is in " LOG_CRASH,
                    how == KLOG_END_HALT ? "the kernel halted" : "reset without a reboot (a hang and the watchdog?)");
    }
    file_size = 0;
    pos = 0;
    ready = append();
    last_us = timer_now_us();
}

void klog_file_sync(bool force)
{
    if (!ready)
        return;
    uint64_t now = timer_now_us();
    if (!force && now - last_us < SYNC_US)
        return;
    uint64_t probe = pos;
    char c;
    if (klog_read_since(&probe, &c, 1) == 0) {
        last_us = now;
        return;                     // 増えていない
    }
    append();
    last_us = now;
}
