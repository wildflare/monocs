// サンドボックス（EL0）の中の C ライブラリの、システムコール（docs/sandbox.md の 3.2。os/kernel/sandbox.c）。
// svc #0、番号は x8（Linux と同じ形）、引数は x0〜x2、戻り値は x0。OS のメモリには触れないので、OS に頼むのはこの道だけ
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SB_SYS_EXIT     0       // exit(終了の値)
#define SB_SYS_WRITE    1       // write(1 か 2, buf, len) → 書いた長さ（buf はサンドボックスの中であること）

// カーネルが入口（__sandbox_start）の x0 に渡すもの（データの領域の頭に置く）
struct sandbox_info {
    uint64_t heap_start, heap_end;  // malloc に使ってよい範囲
    int argc;
    char **argv;
};

static inline long sb_sys(long no, long a0, long a1, long a2)
{
    long r;
    __asm__ volatile("mov x8, %4\n\tmov x0, %1\n\tmov x1, %2\n\tmov x2, %3\n\tsvc #0\n\tmov %0, x0"
                     : "=r"(r) : "r"(a0), "r"(a1), "r"(a2), "r"(no) : "x0", "x1", "x2", "x8", "memory");
    return r;
}

void __sandbox_heap_init(uint64_t start, uint64_t end);
