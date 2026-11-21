// ウォッチドッグ（固まったら・止まったら再起動する。os/kernel/watchdog.c）
#pragma once
#include <stdbool.h>

void watchdog_init(void);       // 起動の早いうちに入れる（kernel_main）
void watchdog_tick(void);       // コア 0 のタイマー割り込みから（餌をやる）
void watchdog_starve(void);     // 餌をやめる（約 16 秒でリセット。試しの起動の期限）
void watchdog_set(bool on);     // シェルの watchdog on・off
bool watchdog_enabled(void);
// カーネルが止まる: why を表示し、記録に残して、数秒後に再起動する（watchdog off なら止まったまま）
[[noreturn]] void kernel_halt(const char *why);
