// MMU（恒等マップ）
#pragma once

// Normal メモリとしてマップする RAM の終わり（Zero 2 W の 512MB）
#define MMU_RAM_END     0x20000000UL

#include <stdbool.h>

void mmu_init(void);
void mmu_enable(void);
void mmu_map_page0(bool on);

// スタックのガードページ（各コアのスタックのいちばん下、64KB を無効にする。あふれたら例外になる）
#define MMU_STACK_GUARD_SIZE 0x10000UL
#include <stdint.h>
uint64_t mmu_stack_guard(int core);  // コア core のガードページの先頭
