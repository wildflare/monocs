#!/usr/bin/env python3
# 比べを続ける形（ir.inc の ir_ccmp_chain。a && b・a || b を cmp・ccmp に。2026-11-27）の試験を生成する。
# いろいろな型・比べ方・定数（ccmp の即値は 0〜31、それより大きいものはレジスタ）・つなぎ方（&&・||・混ぜたもの・
# !・2〜4 個）・使い方（値として返す、if、三項演算子、合流先の値、ループの条件）の関数を作り、境界のまわりの値で
# 呼んで結果を出す。GCC と slimcc の出力が同じことを確かめる（run.sh の 8f）。
#
#   python3 ccmp_gen.py 出力ファイル [乱数の種] [関数の数]

import random
import sys

TYPES = {
    "signed char": (-128, 127), "unsigned char": (0, 255),
    "short": (-32768, 32767), "unsigned short": (0, 65535),
    "int": (-2**31, 2**31 - 1), "unsigned": (0, 2**32 - 1),
    "long": (-2**63, 2**63 - 1), "unsigned long": (0, 2**64 - 1),
}
OPS = ["<", "<=", ">", ">=", "==", "!="]
CONSTS = [0, 1, 7, 31, 32, 91, 124, 4095, 70000, -1, -3]


def lit(ty, v):
    if ty == "unsigned long":
        return f"{v}UL"
    if ty == "long":
        return "(-9223372036854775807L - 1)" if v == -2**63 else f"{v}L"
    if ty == "unsigned":
        return f"{v}U"
    if ty == "int" and v == -2**31:
        return "(-2147483647 - 1)"
    return str(v)


def operand(r, ty):
    if r.random() < 0.5:
        return r.choice(["a", "b", "c"])
    return f"({ty}){r.choice(CONSTS)}"


def comparison(r, ty):
    x = r.choice(["a", "b", "c"])
    s = f"{x} {r.choice(OPS)} {operand(r, ty)}"
    return f"!({s})" if r.random() < 0.15 else s


def condition(r, ty):
    n = r.randint(2, 4)
    mode = r.choice(["&&", "||", "mix"])
    s = comparison(r, ty)
    for _ in range(n - 1):
        op = mode if mode != "mix" else r.choice(["&&", "||"])
        s = f"({s}) {op} {comparison(r, ty)}" if mode == "mix" else f"{s} {op} {comparison(r, ty)}"
    return s


def gen_func(r, k):
    ty = r.choice(list(TYPES))
    cond = condition(r, ty)
    shape = r.randint(0, 5)
    body = []
    if shape == 0:
        body.append(f"    return {cond};")
    elif shape == 1:
        body.append(f"    if ({cond}) return ({ty})(a + 1);")
        body.append(f"    return ({ty})(b ^ c);")
    elif shape == 2:
        body.append(f"    return ({cond}) ? a : ({ty})(c - 2);")
    elif shape == 3:
        body.append(f"    {ty} x = b;")
        body.append(f"    if ({cond}) x = ({ty})(x + c);")
        body.append(f"    return x;")
    elif shape == 4:
        body.append("    int n = 0;")
        body.append("    for (int i = 0; i < 6; i++) {")
        body.append(f"        if ({cond}) n += i;")
        body.append(f"        a = ({ty})(a + 3); b = ({ty})(b - 1);")
        body.append("    }")
        body.append("    return n;")
    else:
        body.append("    int n = 0;")
        body.append(f"    while (n < 9 && ({cond})) {{ n++; a = ({ty})(a + 1); c = ({ty})(c ^ n); }}")
        body.append("    return n;")
    rty = "long long" if shape in (0, 4, 5) else ty
    src = (f"static __attribute__((noinline)) {rty} f{k}({ty} a, {ty} b, {ty} c) {{\n" + "\n".join(body) +
           "\n}\n")
    lo, hi = TYPES[ty]
    vals = sorted({lo, -1 if lo < 0 else 2, 0, 1, 7, 31, 32, 91, 123, 124, hi, r.randint(lo, hi)})
    vals = [v for v in vals if lo <= v <= hi]
    calls = []
    fmt = "%llu" if rty.startswith("unsigned") else "%lld"
    cast = "unsigned long long" if rty.startswith("unsigned") else "long long"
    for _ in range(12):
        x, y, z = (r.choice(vals) for _ in range(3))
        calls.append(f'    printf("{k} {fmt}\\n", ({cast})f{k}({lit(ty, x)}, {lit(ty, y)}, {lit(ty, z)}));')
    return src, calls


def main():
    out = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 300
    r = random.Random(seed)
    funcs, calls = [], []
    for k in range(n):
        s, c = gen_func(r, k)
        funcs.append(s)
        calls.extend(c)
    with open(out, "w") as f:
        f.write("#include <stdio.h>\n\n" + "\n".join(funcs) + "\nint main(void) {\n" + "\n".join(calls) +
                "\n    return 0;\n}\n")


main()
