// 日時（壁の時計）とタイムゾーン（os/kernel/clock.c）
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 今の日時（UNIX 時間のマイクロ秒）。設定されるまでは、起動を 2026-01-01 00:00:00 UTC として数える
int64_t clock_realtime_us(void);
// 日時が設定されたか
bool clock_known(void);
void clock_set(int64_t unix_us);
// 日時がだいたいわかっているか（合わせたか、SD カードに覚えた時刻から推定したか。make・ファイルの日時に使う）
bool clock_approx(void);
// /etc/fake-hwclock.data（Raspberry Pi OS の fake-hwclock と同じ）から推定の時刻を読む・今の時刻を書く
// （force でなければ 60 秒に 1 回まで）
void clock_restore(void);
void clock_save(bool force);

// タイムゾーン（UTC からの分。JST は +540）
void clock_set_tz(int minutes);

// 環境変数の TZ（"TZ=JST-9" など。os/libc/syscalls.c の environ が指す）
extern char clock_tz_string[32];
