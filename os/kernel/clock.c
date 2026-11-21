// 日時（壁の時計）とタイムゾーン
//
// Zero 2 W には電源を切っても動く時計（RTC）がない。設定されるまでは、起動を 2026-01-01 00:00:00 UTC と
// して数える（今までと同じ。QEMU の検証の結果も変わらない）。シェルの date -s か、nos.py がつなぐたびに
// 送る Pi 5 の日時で設定する（tools/uart/nos.py）。設定した後は、汎用タイマー（timer_now_us）で進める。
// タイムゾーンは UTC からの分（JST は +540）。環境変数 TZ に入れて、picolibc の localtime が使う。
// FatFs の日時（get_fattime、os/fs/vfs.c）も、設定されるまでは固定（2026-01-01）。

// setenv（POSIX）の宣言のため
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "clock.h"
#include "../libc/heap.h"
#include "../drivers/timer.h"

#define BOOT_EPOCH_US   (1767225600LL * 1000000)   // 2026-01-01 00:00:00 UTC

static volatile int64_t offset_us = BOOT_EPOCH_US;  // 日時（UNIX 時間のマイクロ秒）= offset_us + timer_now_us()
static volatile bool known;
static volatile bool estimated;     // SD カードに覚えた時刻から（clock_restore。ネットワークで合わせるまで）
static int tz_minutes;
char clock_tz_string[32] = "TZ=UTC0";   // 環境変数の TZ（os/libc/syscalls.c の environ が指す）

int64_t clock_realtime_us(void)
{
    return offset_us + (int64_t)timer_now_us();
}

bool clock_known(void)
{
    return known;
}

void clock_set(int64_t unix_us)
{
    offset_us = unix_us - (int64_t)timer_now_us();
    known = true;
}

bool clock_approx(void)
{
    return known || estimated;
}

// ---- 時刻を SD カードに覚えておく（2026-11-17。Raspberry Pi OS の fake-hwclock と同じ考え。RTC がないので、
// 起動してネットワークで合わせるまで（約 10 秒）時刻がわからず、make が日時を信じられなかった）。
// /etc/fake-hwclock.data に「YYYY-MM-DD HH:MM:SS」（UTC。fake-hwclock と同じ形）。起動したらすぐ読んで時計を
// 進めておき（推定。clock_approx は真、clock_known は偽のまま: HTTPS の証明書の確かめには使わない）、
// シェルが待つ間に 60 秒に 1 回まで・reboot・poweroff の前に書く。電源を切っていた間の分だけ遅れるが、前に
// 書いたファイルより新しい日時から始まる（make の判断にはそれで足りる） ----

#define HWCLOCK_FILE "/etc/fake-hwclock.data"

void clock_restore(void)
{
    if (known)
        return;
    FILE *f = fopen(HWCLOCK_FILE, "r");
    if (!f)
        return;
    struct tm tm = {};
    int n = fscanf(f, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    fclose(f);
    if (n != 6 || tm.tm_year < 2026)
        return;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    time_t t = timegm(&tm);
    if (t <= 0 || known)
        return;
    offset_us = (int64_t)t * 1000000 - (int64_t)timer_now_us();
    estimated = true;
}

void clock_save(bool force)
{
    static uint64_t last_us;
    uint64_t now = timer_now_us();
    if (!clock_approx() || (!force && last_us && now - last_us < 60000000))
        return;
    last_us = now;
    time_t t = (time_t)(clock_realtime_us() / 1000000);
    struct tm tm;
    gmtime_r(&t, &tm);
    FILE *f = fopen(HWCLOCK_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
            tm.tm_sec);
    fclose(f);
}

void clock_set_tz(int minutes)
{
    tz_minutes = minutes;
    // POSIX の TZ は「UTC からの差を引く向き」で書く（JST は UTC+9 なので "JST-9"）。
    // setenv で設定する（シェルの export で environ が作り直されても効くように）
    int m = minutes < 0 ? -minutes : minutes;
    const char *name = minutes == 540 ? "JST" : minutes == 0 ? "UTC" : "LOC";
    char tz[32];
    if (m % 60)
        snprintf(tz, sizeof(tz), "%s%c%d:%02d", name, minutes > 0 ? '-' : '+', m / 60, m % 60);
    else
        snprintf(tz, sizeof(tz), "%s%c%d", name, minutes > 0 ? '-' : '+', m / 60);
    malloc_kernel_begin();  // アプリ（monocs_set_timezone）から呼ばれても、environ はカーネル用のヒープに
    setenv("TZ", tz, 1);
    malloc_kernel_end();
    tzset();
}
