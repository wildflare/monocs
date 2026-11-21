// AArch64 システムレジスタのアクセサ
#pragma once

#include <stdint.h>

#define read_sysreg(reg) ({                                 \
    uint64_t _v;                                            \
    __asm__ volatile("mrs %0, " #reg : "=r"(_v));           \
    _v;                                                     \
})

#define write_sysreg(reg, v) do {                           \
    uint64_t _v = (uint64_t)(v);                            \
    __asm__ volatile("msr " #reg ", %0" :: "r"(_v));        \
} while (0)

static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
#define DAIF_I (1u << 7)    // DAIF の I（IRQ を止めている）

// このコアの番号（MPIDR_EL1.Aff0。0〜3）
static inline int cpu_id(void) { return (int)(read_sysreg(mpidr_el1) & 0xFF); }
