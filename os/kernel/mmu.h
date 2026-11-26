// MMU（恒等マップ）
#pragma once

// Normal メモリとしてマップする RAM の終わり（Zero 2 W の 512MB）
#define MMU_RAM_END     0x20000000UL

#include <stdbool.h>
#include <stdint.h>

void mmu_init(void);
void mmu_enable(void);
void mmu_map_page0(bool on);

// スタックのガードページ（各コアのスタックのいちばん下、64KB を無効にする。あふれたら例外になる）
#define MMU_STACK_GUARD_SIZE 0x10000UL
#include <stdint.h>
uint64_t mmu_stack_guard(int core);  // コア core のガードページの先頭

// サンドボックス（os/kernel/sandbox.c）: 2MB のブロックを EL0 から使える形にする（CODE: EL0 が実行・読むだけ、
// DATA: EL0 が読み書き・実行できない）・戻す（NORMAL: EL1 だけ）
enum mmu_sandbox { MMU_SANDBOX_NORMAL, MMU_SANDBOX_CODE, MMU_SANDBOX_DATA, MMU_SANDBOX_IMAGE };   // IMAGE: EL0 が読み書き・実行
bool mmu_sandbox_map(uint64_t addr, uint64_t size, enum mmu_sandbox kind);
