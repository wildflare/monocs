// サンドボックスの中で curses をコンパイルするための代わり（kernel_shim.c。キーはシステムコールで読む）
#pragma once
#include <stdbool.h>
char uart_getc(void);
bool uart_has_input(void);
void uart_wait_input(void);
void uart_set_raw(bool raw);
void uart_puts(const char *s);
