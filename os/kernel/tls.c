// コアごとの TLS（_Thread_local の変数）
//
// AArch64 の ELF の TLS（variant 1）: TPIDR_EL0（TP）の先に 16 バイトの TCB があり、その後に
// TLS の変数が並ぶ（先頭は TLS の境界にそろえる）。コンパイラとリンカは、変数の位置を
// 「TP + align_up(16, 境界) + TLS の先頭（.tdata）からの位置」として命令に埋め込む。
// 出典: ELF for the Arm 64-bit Architecture（AAELF64）4.6.11、GNU ld（elfnn-aarch64.c の tpoff）。
// 各コアの TLS は .tls_space（linker.ld。1MB ずつ）に置き、.tdata を写して .tbss の分を 0 にする。

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "tls.h"
#include "sysreg.h"
#include "../drivers/uart.h"

#define TLS_BLOCK_SIZE 0x100000
#define TLS_TCB_SIZE   16

extern char __tdata_start[], __tdata_end[], __tbss_start[], __tbss_end[];
extern char __tls_align[], __tls_space[];

// このコアの番号（初期値を持つ TLS の変数があれば .tdata が必ずできる）
_Thread_local int tls_core = -1;

static size_t tcb_size(void)
{
    size_t align = (size_t)__tls_align;
    return (TLS_TCB_SIZE + align - 1) / align * align;
}

void tls_init(int core)
{
    char *tp = __tls_space + (size_t)core * TLS_BLOCK_SIZE;
    char *block = tp + tcb_size();
    size_t init_size = __tdata_end - __tdata_start;
    uintptr_t end = (uintptr_t)__tbss_end > (uintptr_t)__tdata_end ? (uintptr_t)__tbss_end
                                                                   : (uintptr_t)__tdata_end;
    size_t total = end - (uintptr_t)__tdata_start;
    if (tcb_size() + total > TLS_BLOCK_SIZE) {
        uart_puts("*** tls: thread-local variables are too large\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    memcpy(block, __tdata_start, init_size);
    memset(block + init_size, 0, total - init_size);
    write_sysreg(tpidr_el0, tp);
    isb();
    tls_core = core;
}

