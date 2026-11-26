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
#define FRAME_SP0       296     // 下位 EL から来たときの SP_EL0（ネストした例外が SP_EL0 を作業用に使うので、退避する）
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

// 下位 EL（AArch64。サンドボックスの EL0。os/kernel/sandbox.c）から: SP_EL0 をフレームに退避し、EL0 が使っていた
// TPIDR_EL0 を sandbox_user_tp に置いて、カーネルの TPIDR_EL0（コアごとの TLS。sandbox_kernel_tp）に戻す。
// サンドボックスはコア 0 だけで動くので、1 組の変数でよい。戻るとき（exception_common の終わり）に逆をする
.macro ventry_lower type
    .balign 0x80
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    mrs     x0, sp_el0
    str     x0, [sp, #FRAME_SP0]
    mrs     x0, tpidr_el0
    adrp    x1, sandbox_user_tp
    str     x0, [x1, :lo12:sandbox_user_tp]
    adrp    x1, sandbox_kernel_tp
    ldr     x0, [x1, :lo12:sandbox_kernel_tp]
    msr     tpidr_el0, x0
    mov     x0, #\type
    b       exception_common
.endm

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
    // 下位 EL（AArch64。サンドボックスの EL0）
    ventry_lower  EXC_SYNC + 8
    ventry_lower  EXC_IRQ + 8
    ventry_lower  EXC_FIQ + 8
    ventry_lower  EXC_SERROR + 8
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
    // EL0（SPSR の M[3:0] = 0）へ戻るとき: SP_EL0 と、EL0 の TPIDR_EL0 を戻す（ventry_lower の逆）
    ldr     x1, [sp, #FRAME_SPSR]
    tst     x1, #0xF
    b.ne    2f
    ldr     x1, [sp, #FRAME_SP0]
    msr     sp_el0, x1
    adrp    x1, sandbox_user_tp
    ldr     x1, [x1, :lo12:sandbox_user_tp]
    msr     tpidr_el0, x1
2:
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

// サンドボックスに入る（os/kernel/sandbox.c）: x0 = 入口、x1 = スタックの上端、x2 = 入口の x0 に渡すもの。割り込みを止めてから、SP_EL0・
// ELR・SPSR（EL0t、DAIF は全部外す = EL0 では割り込みを受ける）を決め、TPIDR_EL0 を 0 に、汎用・FP のレジスタを
// 0 にして（カーネルの値を見せない）eret する。戻ってこない（出るときは sandbox.c が longjmp する）
    .text
    .global sandbox_eret
sandbox_eret:
    msr     daifset, #0xf
    msr     tpidrro_el0, x2             // 入口の x0 に渡すもの（レジスタを 0 にした後で読む）
    msr     sp_el0, x1
    msr     elr_el1, x0
    msr     spsr_el1, xzr
    msr     tpidr_el0, xzr
    eor     v0.16b, v0.16b, v0.16b
    eor     v1.16b, v1.16b, v1.16b
    eor     v2.16b, v2.16b, v2.16b
    eor     v3.16b, v3.16b, v3.16b
    eor     v4.16b, v4.16b, v4.16b
    eor     v5.16b, v5.16b, v5.16b
    eor     v6.16b, v6.16b, v6.16b
    eor     v7.16b, v7.16b, v7.16b
    eor     v8.16b, v8.16b, v8.16b
    eor     v9.16b, v9.16b, v9.16b
    eor     v10.16b, v10.16b, v10.16b
    eor     v11.16b, v11.16b, v11.16b
    eor     v12.16b, v12.16b, v12.16b
    eor     v13.16b, v13.16b, v13.16b
    eor     v14.16b, v14.16b, v14.16b
    eor     v15.16b, v15.16b, v15.16b
    eor     v16.16b, v16.16b, v16.16b
    eor     v17.16b, v17.16b, v17.16b
    eor     v18.16b, v18.16b, v18.16b
    eor     v19.16b, v19.16b, v19.16b
    eor     v20.16b, v20.16b, v20.16b
    eor     v21.16b, v21.16b, v21.16b
    eor     v22.16b, v22.16b, v22.16b
    eor     v23.16b, v23.16b, v23.16b
    eor     v24.16b, v24.16b, v24.16b
    eor     v25.16b, v25.16b, v25.16b
    eor     v26.16b, v26.16b, v26.16b
    eor     v27.16b, v27.16b, v27.16b
    eor     v28.16b, v28.16b, v28.16b
    eor     v29.16b, v29.16b, v29.16b
    eor     v30.16b, v30.16b, v30.16b
    eor     v31.16b, v31.16b, v31.16b
    msr     fpsr, xzr
    msr     fpcr, xzr
    mov     x0, xzr
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, xzr
    mov     x5, xzr
    mov     x6, xzr
    mov     x7, xzr
    mov     x8, xzr
    mov     x9, xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    mrs     x0, tpidrro_el0
    eret
