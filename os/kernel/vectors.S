// EL1 の例外ベクタ
// 出典: ARM DDI 0487 D1.10（例外ベクタ表）
//
// 割り込み中に呼ぶ C コードが FP/SIMD を使っても壊れないよう、
// 汎用レジスタに加えて q0〜q31、FPSR、FPCR も退避する。
// フレームの配置は exception.h の struct trap_frame と一致させること。

#define FRAME_ELR       248
#define FRAME_SPSR      256
#define FRAME_ESR       264
#define FRAME_FAR       272
#define FRAME_FPSR      280
#define FRAME_FPCR      288
#define FRAME_Q         304
#define FRAME_SIZE      816

// 例外の種類（exception.h の enum exception_type と一致させる）
#define EXC_SYNC        0
#define EXC_IRQ         1
#define EXC_FIQ         2
#define EXC_SERROR      3
// 4 を足すと「SP_EL0 使用中」、8 で「下位 EL（AArch64）」、12 で「下位 EL（AArch32）」
#define EXC_OVERFLOW    16      // スタックがあふれた（ガードページの近くで例外になった）

// スタックのあふれ: OS とアプリは SP_EL1 で動き、例外も同じスタックに退避する。スタックがガードページ
// （os/kernel/mmu.c）まで来て例外になると、退避でまた例外になって止まらないので、入口で SP を調べる。
// SP が「ガードページの先頭（TPIDR_EL1。exception_init が設定する）から OVERFLOW_PAGES ページ」の中なら、
// コアごとの非常用のスタック（exception_overflow_stacks）に切り替え、種類に EXC_OVERFLOW を足して
// handle_exception に渡す（元のスタックには戻らない）。調べる間の作業用には、使っていない SP_EL0 と
// TPIDRRO_EL0 に x0・x1 を置く。
#define OVERFLOW_PAGES  18      // ガードページ 64KB + 8KB（例外のフレームを置ける余裕）
#define OVERFLOW_STACK  16384   // 非常用のスタックの大きさ（exception.c と一致させる）

.macro ventry type
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #\type
    b       exception_common
.endm

// 現在の EL、SP_ELx 使用のとき（スタックのあふれを調べる）
.macro ventry_spx type
    .balign 0x80
    msr     sp_el0, x0
    msr     tpidrro_el0, x1
    mov     x0, sp
    mrs     x1, tpidr_el1
    sub     x0, x0, x1
    lsr     x0, x0, #12
    cmp     x0, #OVERFLOW_PAGES
    mrs     x1, tpidrro_el0
    mrs     x0, sp_el0
    b.lo    1f
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mov     x0, #\type
    b       exception_common
1:  mov     x0, #(\type + EXC_OVERFLOW)
    b       exception_overflow
.endm

    .section .text.vectors, "ax"
    .balign 0x800
    .global exception_vectors
exception_vectors:
    // 現在の EL、SP_EL0 使用
    ventry  EXC_SYNC + 4
    ventry  EXC_IRQ + 4
    ventry  EXC_FIQ + 4
    ventry  EXC_SERROR + 4
    // 現在の EL、SP_ELx 使用（通常はここに来る）
    ventry_spx  EXC_SYNC
    ventry_spx  EXC_IRQ
    ventry_spx  EXC_FIQ
    ventry_spx  EXC_SERROR
    // 下位 EL（AArch64）
    ventry  EXC_SYNC + 8
    ventry  EXC_IRQ + 8
    ventry  EXC_FIQ + 8
    ventry  EXC_SERROR + 8
    // 下位 EL（AArch32）
    ventry  EXC_SYNC + 12
    ventry  EXC_IRQ + 12
    ventry  EXC_FIQ + 12
    ventry  EXC_SERROR + 12

// スタックがあふれた: x0 = 種類、x1 = 元の x1、SP_EL0 = 元の x0
exception_overflow:
    msr     tpidrro_el0, x1
    ldr     x1, =exception_overflow_stacks + OVERFLOW_STACK
    mov     sp, x1
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    lsl     x1, x1, #14                 // コア番号 × OVERFLOW_STACK
    add     sp, sp, x1
    sub     sp, sp, #FRAME_SIZE
    mrs     x1, sp_el0
    str     x1, [sp, #0]
    mrs     x1, tpidrro_el0
    str     x1, [sp, #8]
    b       exception_common
    .ltorg

exception_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]

    mrs     x1, elr_el1
    mrs     x2, spsr_el1
    stp     x1, x2, [sp, #FRAME_ELR]
    mrs     x1, esr_el1
    mrs     x2, far_el1
    stp     x1, x2, [sp, #FRAME_ESR]
    mrs     x1, fpsr
    mrs     x2, fpcr
    stp     x1, x2, [sp, #FRAME_FPSR]

    add     x1, sp, #FRAME_Q
    stp     q0, q1, [x1, #0]
    stp     q2, q3, [x1, #32]
    stp     q4, q5, [x1, #64]
    stp     q6, q7, [x1, #96]
    stp     q8, q9, [x1, #128]
    stp     q10, q11, [x1, #160]
    stp     q12, q13, [x1, #192]
    stp     q14, q15, [x1, #224]
    stp     q16, q17, [x1, #256]
    stp     q18, q19, [x1, #288]
    stp     q20, q21, [x1, #320]
    stp     q22, q23, [x1, #352]
    stp     q24, q25, [x1, #384]
    stp     q26, q27, [x1, #416]
    stp     q28, q29, [x1, #448]
    stp     q30, q31, [x1, #480]

    // handle_exception(type, frame)
    mov     x1, sp
    bl      handle_exception

    // C 側で書き換えた ELR/SPSR を反映して復帰する
    add     x1, sp, #FRAME_Q
    ldp     q0, q1, [x1, #0]
    ldp     q2, q3, [x1, #32]
    ldp     q4, q5, [x1, #64]
    ldp     q6, q7, [x1, #96]
    ldp     q8, q9, [x1, #128]
    ldp     q10, q11, [x1, #160]
    ldp     q12, q13, [x1, #192]
    ldp     q14, q15, [x1, #224]
    ldp     q16, q17, [x1, #256]
    ldp     q18, q19, [x1, #288]
    ldp     q20, q21, [x1, #320]
    ldp     q22, q23, [x1, #352]
    ldp     q24, q25, [x1, #384]
    ldp     q26, q27, [x1, #416]
    ldp     q28, q29, [x1, #448]
    ldp     q30, q31, [x1, #480]

    ldp     x1, x2, [sp, #FRAME_FPSR]
    msr     fpsr, x1
    msr     fpcr, x2
    ldp     x1, x2, [sp, #FRAME_ELR]
    msr     elr_el1, x1
    msr     spsr_el1, x2

    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #FRAME_SIZE
    eret
