// サンドボックスの中の curses（OS の os/curses/curses.c を書き換えずに写したもの。tools/tls/mklib.sh が curses/ に置く）が
// 呼ぶ OS の関数の代わり。キー・時刻・画面の大きさはシステムコールで。Ctrl-C はいつもサンドボックスを止める
// （raw にしてもキーとしては届かない）
#include <stdint.h>
#include <stdio.h>
#include "sandbox_sys.h"
#include "drivers/timer.h"
#include "drivers/uart.h"
#include "kernel/contap.h"
#include "kernel/term.h"

static int pend = -1;

bool uart_has_input(void)
{
    if (pend < 0) {
        unsigned char c;
        if (sb_sys4(SB_SYS_READ, 0, (long)&c, 1, 0) == 1)
            pend = c;
    }
    return pend >= 0;
}

void uart_wait_input(void)
{
    while (pend < 0) {
        unsigned char c;
        if (sb_sys4(SB_SYS_READ, 0, (long)&c, 1, -1) == 1)
            pend = c;
    }
}

char uart_getc(void)
{
    uart_wait_input();
    char c = (char)pend;
    pend = -1;
    return c;
}

void uart_set_raw(bool raw)
{
    (void)raw;
}

void uart_puts(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

uint64_t timer_now_us(void)
{
    return (uint64_t)sb_sys(SB_SYS_CLOCK, 1, 0, 0) / 1000;
}

void timer_delay_us(uint64_t us)
{
    sb_sys(SB_SYS_SLEEP, (long)(us * 1000), 0, 0);
}

bool term_size(int *rows, int *cols)
{
    unsigned long v = (unsigned long)sb_sys(SB_SYS_TERMSIZE, 0, 0, 0);
    *rows = (int)(v >> 32);
    *cols = (int)(v & 0xFFFFFFFF);
    return true;
}

bool contap_size(int *rows, int *cols)
{
    (void)rows, (void)cols;
    return false;       // 大きさの変わったことは、まだ知らせない
}
