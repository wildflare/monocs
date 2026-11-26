// サンドボックス（EL0）の中の C ライブラリの、システムコール（docs/sandbox.md の 3.2。os/kernel/sandbox.c）。
// svc #0、番号は x8（Linux と同じ形）、引数は x0〜x2、戻り値は x0。OS のメモリには触れないので、OS に頼むのはこの道だけ
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SB_SYS_EXIT     0       // exit(終了の値)
#define SB_SYS_WRITE    1       // write(1 か 2, buf, len) → 書いた長さ（buf はサンドボックスの中であること）
#define SB_SYS_READ     2       // read(0, buf, len, timeout_ms（負ならいつまでも）) → 読んだ長さ（生のバイト。時間切れは 0）
#define SB_SYS_TERMSIZE 3       // → 行 << 32 | 桁
#define SB_SYS_CLOCK    4       // clock(0 = 実時間、1 = 起動から) → ナノ秒
#define SB_SYS_SLEEP    5       // sleep(ナノ秒)
#define SB_SYS_RANDOM   6       // random(buf, len ≤ 256) → 0 か -1
#define SB_SYS_GO       7       // go(url, len, back): ページを終えて移る・戻る（C の Web。cweb.h）

// カーネルが入口（__sandbox_start）の x0 に渡すもの（データの領域の頭に置く）
struct sandbox_info {
    uint64_t heap_start, heap_end;  // malloc に使ってよい範囲
    int argc;
    char **argv;
};

static inline long sb_sys4(long no, long a0, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("mov x8, %5\n\tmov x0, %1\n\tmov x1, %2\n\tmov x2, %3\n\tmov x3, %4\n\tsvc #0\n\tmov %0, x0"
                     : "=r"(r) : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(no) : "x0", "x1", "x2", "x3", "x8", "memory");
    return r;
}

static inline long sb_sys(long no, long a0, long a1, long a2)
{
    return sb_sys4(no, a0, a1, a2, 0);
}

void __sandbox_heap_init(uint64_t start, uint64_t end);
