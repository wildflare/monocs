// サンドボックスの中で curses をコンパイルするための代わり（kernel_shim.c）
#pragma once
#include <stdbool.h>
bool contap_size(int *rows, int *cols);
