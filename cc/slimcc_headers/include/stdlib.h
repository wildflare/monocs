#ifndef _SLIMCC_STDLIB_H
#define _SLIMCC_STDLIB_H

#include_next <stdlib.h>

// Monocs（picolibc）: _GNU_SOURCE のとき malloc_usable_size も宣言する（glibc は malloc.h だけ。Linux 以外では宣言せずに
// 呼ぶプログラムがある（QuickJS の qjs.c・quickjs.c）。Monocs 独自。2026-11-20）
#if defined(__MONOCS__) && __GNU_VISIBLE
size_t malloc_usable_size(void *ptr);
#endif

#endif
