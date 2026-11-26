// サンドボックスの中で curses（os/curses/curses.c をそのまま写したもの）をコンパイルするための代わり（kernel_shim.c）
#pragma once
#include <stdint.h>
uint64_t timer_now_us(void);
void timer_delay_us(uint64_t us);
