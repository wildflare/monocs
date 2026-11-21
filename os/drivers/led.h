// 基板上の ACT LED（緑）
#pragma once

void led_init(void);
void led_set(bool on);

// タイマー割り込みから毎ティック呼ぶ。点滅の周期は led_set_blink で決める
void led_tick(void);
// 点滅の半周期（ティック数）。0 なら点滅しない
void led_set_blink(unsigned half_period_ticks);
unsigned led_get_blink(void);

// アプリ用: 点滅を止めて、点灯・消灯を指定する（OS API 表では led_set の名前で渡す）
void led_set_manual(bool on);
