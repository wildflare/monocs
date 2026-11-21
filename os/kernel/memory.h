// メモリ管理（ヒープの範囲）
#pragma once

#include <stddef.h>
#include <stdint.h>

void memory_init(void);
uintptr_t memory_heap_start(void);
uintptr_t memory_heap_end(void);
size_t memory_heap_used(void);

// アプリ用のヒープ（上から下へ広げる。os/libc/malloc.c が使う）
uintptr_t memory_heap_brk(void);    // カーネル用のヒープの今の終わり
uintptr_t memory_app_low(void);
uintptr_t memory_app_top(void);
void *memory_app_grow(size_t incr);
void memory_app_reset(void);
// 入れ子のアプリ（system()）の間だけ、親の領域の下に子の領域を作る（os/libc/malloc.c）
bool memory_app_push(void);
void memory_app_pop(void);
