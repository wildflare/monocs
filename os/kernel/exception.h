// 例外処理
#pragma once

#include <stdint.h>

// vectors.S が作るフレーム。配置を変えるときは vectors.S の FRAME_* も直す。
struct trap_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t esr;
    uint64_t far;
    uint64_t fpsr;
    uint64_t fpcr;
    uint64_t sp0;           // 下位 EL（EL0。サンドボックス）から来たときの SP_EL0（os/kernel/sandbox.c）
    __uint128_t q[32];
};

enum exception_type {
    EXC_SYNC = 0,
    EXC_IRQ = 1,
    EXC_FIQ = 2,
    EXC_SERROR = 3,
};

void exception_init(void);

// コア 0 がどこにいたかの記録（アプリの起動が遅かったときの調べ。os/slimcc/run.c）
#define STALL_MAX 4096
void stall_sample_begin(void);
int stall_sample_end(const uint64_t **pc, const uint64_t **lr, const uint8_t **core);
void stall_record(int core, uint64_t pc, uint64_t lr);
#define STALL_MBOX_BIT 2    // メールボックス 0 の、記録の頼みのビット（1 は仕事の中断）

// コア 0 が最後の割り込みのときにいた番地と x30（シェルが止まったときの調べ）
void core0_where(uint64_t *pc, uint64_t *lr);
