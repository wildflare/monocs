// メモリマップド I/O のアクセサ
#pragma once

#include <stdint.h>

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

// 指定サイクル数だけ空回りする（GPIO のプルアップ設定などの待ち時間用）
static inline void delay_cycles(uint32_t count)
{
    while (count--)
        __asm__ volatile("nop");
}
