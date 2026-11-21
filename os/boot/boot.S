// 起動コード
// 出典: Arm Architecture Reference Manual for A-profile (ARM DDI 0487)
//       D13（AArch64 システムレジスタ）

// SCR_EL3: NS(0) | RES1(4,5) | SMD(7) | HCE(8) | RW(10)
// 下位 EL を非セキュア・AArch64 にし、SMC を無効化する
#define SCR_EL3_VALUE       0x5B1

// SPSR: DAIF をすべてマスク（ビット 6〜9）し、指定 EL の SP_ELx を使う
#define SPSR_EL2H           0x3C9
#define SPSR_EL1H           0x3C5

// HCR_EL2.RW(31): EL1 を AArch64 で動かす
#define HCR_EL2_RW          (1 << 31)

// CNTHCTL_EL2: EL1PCTEN(0) | EL1PCEN(1)。EL1 から物理タイマーを使えるようにする
#define CNTHCTL_EL2_VALUE   0x3

// CPTR_EL2: RES1 ビットだけを立て、TFP(10) を 0 にして FP/SIMD をトラップしない
#define CPTR_EL2_VALUE      0x33FF

// SCTLR_EL1: RES1 ビットのみ。MMU・キャッシュ・アライメント検査は無効、リトルエンディアン
#define SCTLR_EL1_VALUE     0x30D00800

// CPACR_EL1.FPEN(20,21) = 0b11: EL0/EL1 で FP/SIMD をトラップしない
#define CPACR_EL1_FPEN      (3 << 20)

    .section .text.boot, "ax"
    .global _start
_start:
    // 1. コア 0 以外は待機させる
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF           // Aff0 = コア番号
    cbnz    x0, park

    // 2. 開始 EL を x19 に保存し（kernel_main に渡す）、EL1 へ降りる
    mrs     x19, CurrentEL
    lsr     x19, x19, #2
    cmp     x19, #3
    b.eq    from_el3
    cmp     x19, #2
    b.eq    from_el2
    b       in_el1

// EL3 → EL2 → EL1 に降りる手順（コア 0 とコア 1〜3 で共通）
.macro from_el3_to next
    ldr     x0, =SCR_EL3_VALUE
    msr     scr_el3, x0
    mov     x0, #SPSR_EL2H
    msr     spsr_el3, x0
    adr     x0, \next
    msr     elr_el3, x0
    eret
.endm

.macro from_el2_to next
    mov     x0, #CNTHCTL_EL2_VALUE
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #HCR_EL2_RW
    msr     hcr_el2, x0
    mov     x0, #CPTR_EL2_VALUE
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    ldr     x0, =SCTLR_EL1_VALUE
    msr     sctlr_el1, x0
    mov     x0, #SPSR_EL1H
    msr     spsr_el2, x0
    adr     x0, \next
    msr     elr_el2, x0
    eret
.endm

from_el3:
    from_el3_to from_el2

from_el2:
    from_el2_to in_el1

in_el1:
    // 3. FP/SIMD を有効にする（C コードより前）
    mov     x0, #CPACR_EL1_FPEN
    msr     cpacr_el1, x0
    isb

    // 4. スタックを設定し、BSS をゼロクリアして kernel_main を呼ぶ
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    stp     xzr, xzr, [x0], #16     // BSS は 16 バイト境界（linker.ld）
    b       1b
2:
    mov     x0, x19
    bl      kernel_main

park:
    wfe
    b       park

// コア 1〜3 の入口。ファームウェアのスピンテーブル（0xE0、0xE8、0xF0）に書いた番地から始まる
// （os/kernel/smp.c）。EL1 に降り、FP/SIMD を有効にし、コアごとのスタックで secondary_main を呼ぶ
    .global secondary_entry
secondary_entry:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    sec_from_el3
    cmp     x0, #2
    b.eq    sec_from_el2
    b       sec_in_el1

sec_from_el3:
    from_el3_to sec_from_el2

sec_from_el2:
    from_el2_to sec_in_el1

sec_in_el1:
    mov     x0, #CPACR_EL1_FPEN
    msr     cpacr_el1, x0
    isb
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF
    // スタックの上端 = __stack_smp_base + コア番号 × 8MB（コア 1 は __stack_smp_base からの 8MB）
    ldr     x0, =__stack_smp_base
    mov     x1, #0x800000
    madd    x0, x19, x1, x0
    mov     sp, x0
    mov     x0, x19
    bl      secondary_main
    b       park
