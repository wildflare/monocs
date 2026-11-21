// ARM 汎用タイマー（システムティック）
#pragma once

#include <stdint.h>

#define TIMER_HZ    100

void timer_init(void);
void timer_handle_irq(void);
uint64_t timer_ticks(void);
uint64_t timer_freq(void);

// 起動からの経過時間（マイクロ秒）。割り込みなしでも使える
uint64_t timer_now_us(void);
void timer_delay_us(uint64_t us);
// コア 0 を次の割り込みまで眠らせる（省電力。コア 1〜3 では何もしない）。眠っていた時間の合計
void cpu_idle(void);
uint64_t timer_idle_us(void);
void cpu_idle_account(uint64_t ticks);
void cpu_event_stream(bool on);    // このコアの wfe を約 0.85ms ごとに起こす（タイマーのイベントストリーム）  // wfe などで眠った時間（cntpct のカウント）を、このコアの眠っていた時間に足す
