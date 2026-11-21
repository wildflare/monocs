#!/usr/bin/env python3
# 内蔵アセンブラの網羅テスト用に、命令をたくさん並べた .s を生成する。
# GNU as の出力と比べる（test-a64/asm_compare.sh）。
#
#   python3 asm_gen.py out.s [乱数の種]

import random
import sys

rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
out = []


def emit(s):
    out.append("\t" + s)


def x(n):
    return "xzr" if n == 31 else f"x{n}"


def w(n):
    return "wzr" if n == 31 else f"w{n}"


def regs():
    return [0, 1, 7, 15, 16, 28, 29, 30, 31]


def decode_bitmask(n, immr, imms, width):
    # Arm ARM の DecodeBitMasks
    combined = (n << 6) | ((~imms) & 0x3F)
    length = combined.bit_length() - 1
    if length < 1:
        return None
    size = 1 << length
    levels = size - 1
    s = imms & levels
    r = immr & levels
    if s == levels:
        return None
    elem = (1 << (s + 1)) - 1
    elem = ((elem >> r) | (elem << (size - r))) & ((1 << size) - 1)
    v = 0
    for i in range(0, width, size):
        v |= elem << i
    return v


out.append("\t.text")
out.append("start:")

# ビットマスク即値（論理演算）: 符号化できるすべての値
for width in (32, 64):
    seen = set()
    for n in ((0, 1) if width == 64 else (0,)):
        for immr in range(64):
            for imms in range(64):
                v = decode_bitmask(n, immr, imms, width)
                if v is None or v in seen:
                    continue
                if width == 32 and immr >= 32:
                    continue
                seen.add(v)
    vals = sorted(seen)
    for i, v in enumerate(vals):
        op = ["and", "orr", "eor", "ands"][i % 4]
        r = (lambda k: x(k)) if width == 64 else (lambda k: w(k))
        dst = r(i % 31) if op != "ands" else r(i % 31)
        emit(f"{op} {dst}, {r((i * 7) % 31)}, #{hex(v)}")
        if i % 9 == 0:
            emit(f"tst {r(i % 31)}, #{hex(v)}")

# mov の即値（movz・movn・orr で表せるもの）
for _ in range(400):
    k = rnd.randrange(3)
    if k == 0:
        v = rnd.randrange(1 << 16) << (16 * rnd.randrange(4))
    elif k == 1:
        v = ~(rnd.randrange(1 << 16) << (16 * rnd.randrange(4))) & ((1 << 64) - 1)
    else:
        v = rnd.choice([0x5555555555555555, 0xFF00FF00FF00FF00, 0x0F0F0F0F0F0F0F0F,
                        0x00FFFFFF00000000, 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE])
    emit(f"mov {x(rnd.randrange(31))}, #{hex(v)}")
for v in (0, 1, 0xFFFF, 0x10000, 0xFFFF0000, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000, 0x55555555):
    emit(f"mov w3, #{hex(v)}")
for hw in range(4):
    emit(f"movz x5, #0x1234, lsl #{16 * hw}")
    emit(f"movn x6, #0xabcd, lsl #{16 * hw}")
    emit(f"movk x7, #0xbeef, lsl #{16 * hw}")
emit("movz w8, #0xffff, lsl #16")
emit("movk w9, #1, lsl #16")

# 加減算
for op in ("add", "adds", "sub", "subs"):
    for imm in (0, 1, 4095, 4096, 0x123000, 0xFFF000):
        emit(f"{op} x1, x2, #{imm}")
        emit(f"{op} w1, w2, #{imm & 0xFFF}")
    emit(f"{op} x3, x4, #1, lsl #12")
    emit(f"{op} x3, x4, x5")
    emit(f"{op} w3, w4, w5")
    for sh in ("lsl", "lsr", "asr"):
        emit(f"{op} x3, x4, x5, {sh} #13")
        emit(f"{op} w3, w4, w5, {sh} #31")
    for ext in ("uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw"):
        emit(f"{op} x3, x4, w5, {ext}")
        emit(f"{op} x3, x4, w5, {ext} #2")
    emit(f"{op} x3, x4, x5, sxtx #3")
emit("add sp, sp, #16")
emit("sub sp, sp, #0x1000")
emit("add x0, sp, #8")
emit("add sp, x29, #0")
emit("add sp, sp, x16")
emit("sub sp, x29, x17")
emit("add x1, x17, w16, sxtw")
emit("add w0, w1, #-5")
emit("sub x0, x1, #-7")
emit("mov x29, sp")
emit("mov sp, x16")
emit("mov x16, sp")
emit("mov wsp, w1")
for op in ("cmp", "cmn"):
    emit(f"{op} x1, #0")
    emit(f"{op} x1, #4095")
    emit(f"{op} w1, #17, lsl #12")
    emit(f"{op} x1, x2")
    emit(f"{op} w1, w2, lsl #3")
    emit(f"{op} sp, #16")
    emit(f"{op} x1, w2, uxtw")
emit("neg x0, x1")
emit("neg w0, w1, lsl #2")
emit("negs x3, x4")

# 論理演算（レジスタ）
for op in ("and", "orr", "eor", "ands", "bic", "orn", "eon", "bics"):
    emit(f"{op} x1, x2, x3")
    emit(f"{op} w1, w2, w3, ror #7")
    emit(f"{op} x1, x2, x3, lsl #63")
    emit(f"{op} x1, x2, x3, asr #1")
emit("mvn x0, x1")
emit("mvn w0, w1, lsl #4")
emit("tst x1, x2")
emit("tst w1, w2, lsr #3")
for r in regs():
    emit(f"mov {x(r)}, {x((r + 5) % 32)}")
    emit(f"mov {w(r)}, {w((r + 3) % 32)}")

# シフト・ビットフィールド
for op in ("lsl", "lsr", "asr", "ror"):
    for s in (0, 1, 31, 63):
        emit(f"{op} x1, x2, #{s}")
        if s < 32:
            emit(f"{op} w1, w2, #{s}")
    emit(f"{op} x1, x2, x3")
    emit(f"{op} w1, w2, w3")
for op in ("sxtb", "sxth", "uxtb", "uxth"):
    emit(f"{op} w1, w2")
for op in ("sxtb", "sxth", "sxtw"):
    emit(f"{op} x1, w2")
for _ in range(60):
    lsb = rnd.randrange(64)
    wd = rnd.randrange(1, 65 - lsb)
    op = rnd.choice(["ubfx", "sbfx", "bfi", "bfxil", "ubfiz", "sbfiz"])
    emit(f"{op} x1, x2, #{lsb}, #{wd}")
    lsb32 = rnd.randrange(32)
    wd32 = rnd.randrange(1, 33 - lsb32)
    emit(f"{op} w1, w2, #{lsb32}, #{wd32}")
emit("ubfm x1, x2, #3, #9")
emit("sbfm w1, w2, #31, #0")
emit("bfm x0, x1, #60, #2")

# 乗除算・その他
for op in ("mul", "mneg", "udiv", "sdiv", "smulh", "umulh"):
    emit(f"{op} x1, x2, x3")
    if op not in ("smulh", "umulh"):
        emit(f"{op} w1, w2, w3")
emit("smull x0, w1, w2")
emit("umull x0, w1, w2")
emit("madd x1, x2, x3, x4")
emit("msub w1, w2, w3, w4")
for op in ("clz", "cls", "rbit", "rev", "rev16"):
    emit(f"{op} x1, x2")
    emit(f"{op} w1, w2")
emit("rev32 x1, x2")

# 条件
conds = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
for c in conds:
    emit(f"cset w0, {c}")
    emit(f"csetm x1, {c}")
    emit(f"csel x2, x3, x4, {c}")
    emit(f"csinc w2, w3, w4, {c}")
    emit(f"csinv x2, x3, x4, {c}")
    emit(f"csneg w2, w3, w4, {c}")
    emit(f"cinc x5, x6, {c}")
    emit(f"cinv w5, w6, {c}")
    emit(f"cneg x5, x6, {c}")
emit("cset w0, cs")
emit("cset w0, cc")

# 分岐（同じセクションのローカルラベル）
out.append(".L.back:")
for c in conds:
    emit(f"b.{c} .L.back")
    emit(f"b.{c} .L.fwd")
emit("b .L.back")
emit("bl .L.fwd")
emit("cbz x1, .L.back")
emit("cbnz w2, .L.fwd")
for b in (0, 1, 31, 32, 63):
    emit(f"tbz x3, #{b}, .L.back")
    emit(f"tbnz x3, #{b}, .L.fwd")
emit("tbz w3, #5, .L.fwd")
emit("adr x0, .L.back")
emit("adr x1, .L.fwd+3")
emit("br x16")
emit("blr x17")
emit("ret")
emit("ret x3")
out.append("1:")
emit("b 1b")
emit("b 1f")
out.append("1:")
emit("nop")
out.append(".L.fwd:")

# ロード・ストア
ldst = [("ldr", "x", 8), ("str", "x", 8), ("ldr", "w", 4), ("str", "w", 4),
        ("ldrb", "w", 1), ("strb", "w", 1), ("ldrh", "w", 2), ("strh", "w", 2),
        ("ldrsb", "w", 1), ("ldrsb", "x", 1), ("ldrsh", "w", 2), ("ldrsh", "x", 2),
        ("ldrsw", "x", 4),
        ("ldr", "b", 1), ("str", "b", 1), ("ldr", "h", 2), ("str", "h", 2),
        ("ldr", "s", 4), ("str", "s", 4), ("ldr", "d", 8), ("str", "d", 8),
        ("ldr", "q", 16), ("str", "q", 16)]
for mn, rk, sz in ldst:
    rt = f"{rk}3"
    for ofs in (0, sz, sz * 4095, -8, -256, 255, 1 if sz > 1 else 3, 17):
        emit(f"{mn} {rt}, [x4, #{ofs}]")
    emit(f"{mn} {rt}, [sp]")
    emit(f"{mn} {rt}, [x29, #-16]")
    emit(f"{mn} {rt}, [x5, #16]!")
    emit(f"{mn} {rt}, [x5, #-256]!")
    emit(f"{mn} {rt}, [x5], #16")
    emit(f"{mn} {rt}, [x5], #-1")
    emit(f"{mn} {rt}, [x6, x7]")
    emit(f"{mn} {rt}, [x6, w7, uxtw]")
    emit(f"{mn} {rt}, [x6, w7, sxtw]")
    emit(f"{mn} {rt}, [x6, x7, sxtx]")
    shift = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[sz]
    emit(f"{mn} {rt}, [x6, x7, lsl #{shift}]")
    emit(f"{mn} {rt}, [x6, w7, sxtw #{shift}]")
for mn in ("ldur", "stur"):
    emit(f"{mn} x1, [x2, #8]")
    emit(f"{mn} w1, [x2, #-3]")
    emit(f"{mn} q1, [x2]")
for mn in ("ldurb", "sturb", "ldurh", "sturh", "ldursb", "ldursh"):
    emit(f"{mn} w1, [x2, #-5]")
emit("ldursw x1, [x2, #7]")
emit("ldr xzr, [x17]")
for mn in ("ldp", "stp"):
    for rk, sc in (("x", 8), ("w", 4), ("q", 16), ("d", 8), ("s", 4)):
        emit(f"{mn} {rk}1, {rk}2, [x3]")
        emit(f"{mn} {rk}1, {rk}2, [x3, #{sc * 63}]")
        emit(f"{mn} {rk}1, {rk}2, [x3, #{-sc * 64}]")
        emit(f"{mn} {rk}1, {rk}2, [sp, #-{sc * 2}]!")
        emit(f"{mn} {rk}1, {rk}2, [sp], #{sc * 2}")
emit("ldpsw x1, x2, [x3, #8]")
emit("stp xzr, xzr, [x16], #16")
emit("stp x29, x30, [sp, #-16]!")
emit("ldp x29, x30, [sp], #16")
for sfx in ("", "b", "h"):
    emit(f"ldxr{sfx} w1, [x2]")
    emit(f"ldaxr{sfx} w1, [x2]")
    emit(f"stxr{sfx} w3, w1, [x2]")
    emit(f"stlxr{sfx} w3, w1, [x2]")
    emit(f"ldar{sfx} w1, [x2]")
    emit(f"stlr{sfx} w1, [x2]")
emit("ldaxr x1, [sp]")
emit("stlxr w17, x0, [x9]")
emit("ldar x16, [x0]")
emit("stlr x1, [x2]")

# システム
for b in ("sy", "st", "ld", "ish", "ishst", "ishld", "nsh", "osh", "oshld"):
    emit(f"dmb {b}")
    emit(f"dsb {b}")
emit("isb")
emit("isb sy")
for h in ("nop", "yield", "wfe", "wfi", "sev", "sevl", "eret", "clrex"):
    emit(h)
emit("clrex #3")
emit("svc #0")
emit("svc #0x1234")
emit("hvc #1")
emit("smc #2")
emit("brk #0xffff")
emit("hlt #7")
for sr in ("tpidr_el0", "tpidr_el1", "nzcv", "daif", "fpcr", "fpsr", "currentel", "cntfrq_el0",
           "cntpct_el0", "cntvct_el0", "cntp_ctl_el0", "cntp_cval_el0", "cntp_tval_el0",
           "cnthctl_el2", "cntvoff_el2", "midr_el1", "mpidr_el1", "ctr_el0", "sctlr_el1",
           "sctlr_el2", "cpacr_el1", "cptr_el2", "hcr_el2", "hstr_el2", "scr_el3", "ttbr0_el1",
           "tcr_el1", "mair_el1", "vbar_el1", "esr_el1", "far_el1", "elr_el1", "spsr_el1",
           "sp_el0", "elr_el2", "spsr_el2", "elr_el3", "spsr_el3", "dczid_el0",
           "id_aa64mmfr0_el1", "s3_3_c13_c0_2"):
    emit(f"mrs x1, {sr}")
    if sr not in ("currentel", "midr_el1", "mpidr_el1", "ctr_el0", "cntpct_el0", "cntvct_el0",
                  "dczid_el0", "id_aa64mmfr0_el1"):
        emit(f"msr {sr}, x2")
emit("msr daifset, #2")
emit("msr daifclr, #15")
emit("msr spsel, #1")

# 浮動小数点
for t in ("s", "d"):
    for op in ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"):
        emit(f"{op} {t}0, {t}1, {t}31")
    for op in ("fabs", "fneg", "fsqrt", "frintn", "frintp", "frintm", "frintz", "frinta",
               "frintx", "frinti", "fmov"):
        emit(f"{op} {t}2, {t}3")
    for op in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
        emit(f"{op} {t}1, {t}2, {t}3, {t}4")
    emit(f"fcmp {t}1, {t}2")
    emit(f"fcmpe {t}1, {t}2")
    emit(f"fcmp {t}1, #0.0")
    for g in ("w", "x"):
        for op in ("scvtf", "ucvtf"):
            emit(f"{op} {t}1, {g}2")
        for op in ("fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms",
                   "fcvtmu", "fcvtas", "fcvtau"):
            emit(f"{op} {g}1, {t}2")
    for v in ("1.0", "-2.5", "0.125", "31.0", "0.5"):
        emit(f"fmov {t}4, #{v}")
emit("fcvt d0, s1")
emit("fcvt s0, d1")
emit("fcvt h0, s1")
emit("fmov s0, w1")
emit("fmov w0, s1")
emit("fmov d0, x1")
emit("fmov x0, d1")
emit("fmov s0, wzr")
emit("fmov x1, v2.d[1]")
emit("fmov v2.d[1], x1")
emit("movi v0.2d, #0")
emit("movi d1, #0")
emit("movi v3.16b, #0xab")
emit("movi v3.8b, #7")
emit("mov v1.16b, v0.16b")
emit("mov v4.8b, v5.8b")
emit("mov v0.d[1], x17")
emit("mov v0.s[3], w2")
emit("mov v0.b[15], w2")
emit("mov v0.h[1], w2")
emit("mov x16, v0.d[1]")
emit("umov w1, v2.s[2]")
emit("umov w1, v2.b[7]")
emit("mov v1.d[0], v2.d[1]")
for op in ("and", "orr", "eor", "bic", "orn"):
    emit(f"{op} v1.16b, v2.16b, v3.16b")
    emit(f"{op} v1.8b, v2.8b, v3.8b")

# データ
out.append("\t.data")
out.append("dstart:")
emit(".byte 1, 2, 255, -1")
emit(".hword 0x1234, -2")
emit(".short 7")
emit(".word 0xdeadbeef, -1, 1 << 20")
emit(".long 5")
emit(".xword 0x0123456789abcdef, -1")
emit(".quad 9")
emit(".ascii \"ab\\n\\t\\\\\\\"\\x41\\101\"")
emit(".asciz \"xyz\"")
emit(".string \"s\"")
emit(".zero 5")
emit(".balign 16")
emit(".p2align 3")
emit(".align 2")
emit(".space 3, 0x7f")
emit(".xword start + 8")
emit(".xword dstart")
emit(".xword ext_sym + 16")
emit(".word ext_sym2")
out.append("\t.text")
emit("bl ext_fn")
emit("b ext_fn")
emit("adrp x0, ext_var")
emit("add x0, x0, :lo12:ext_var")
emit("ldr x1, [x0, :lo12:ext_var]")
emit("ldr w1, [x0, :lo12:ext_var]")
emit("ldrb w1, [x0, :lo12:ext_var]")
emit("ldr q1, [x0, :lo12:ext_var]")
emit("adrp x0, :got:ext_var")
emit("ldr x0, [x0, :got_lo12:ext_var]")
emit("adrp x0, dstart+4")
emit("add x0, x0, :lo12:dstart+4")
emit("b.eq ext_fn")
emit("cbz x0, ext_fn")
emit("tbnz x0, #3, ext_fn")
emit(".inst 0xd503201f")

print("\n".join(out), file=open(sys.argv[1], "w"))
