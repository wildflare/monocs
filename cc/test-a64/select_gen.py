#!/usr/bin/env python3
# if 変換（ir.inc の ir_if_convert。値を選ぶだけの小さな分岐を csel にする。2026-11-26）の試験を生成する。
# いろいろな型・比べ方・形（三項演算子、片方だけの if、if/else、腕の中の安い計算、2 つの変数を同時に選ぶ、
# 0 との比べ、条件が比較でない値、ループの中）の関数を作り、境界のまわりの値で呼んで結果を出す。
# GCC と slimcc の出力が同じことを確かめる（run.sh の 8e）。
#
#   python3 select_gen.py 出力ファイル [乱数の種] [関数の数]

import random
import sys

TYPES = {
    "signed char": (-128, 127), "unsigned char": (0, 255),
    "short": (-32768, 32767), "unsigned short": (0, 65535),
    "int": (-2**31, 2**31 - 1), "unsigned": (0, 2**32 - 1),
    "long": (-2**63, 2**63 - 1), "unsigned long": (0, 2**64 - 1),
}
OPS = ["<", "<=", ">", ">=", "==", "!="]
ARITH = ["+", "-", "&", "|", "^"]


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


def small(r):
    return str(r.choice([0, 1, 2, 3, 7, 100, -1, -5]))


def arm(r, ty):
    # 腕の値: 引数そのまま・定数・安い計算（ずらしは 0〜3）
    k = r.randint(0, 4)
    if k == 0:
        return r.choice(["a", "b"])
    if k == 1:
        return f"({ty})" + small(r)
    if k == 2:
        return f"({ty})(a {r.choice(ARITH)} b)"
    if k == 3:
        return f"({ty})(a {r.choice(ARITH)} {small(r)})"
    return f"({ty})(b << {r.randint(0, 3)})"


def gen_func(r, k):
    ty = r.choice(list(TYPES))
    op = r.choice(OPS)
    cond = r.choice([f"a {op} b", f"a {op} 0", "a", f"(a & 1)", f"a {op} b && b != 0"])
    shape = r.randint(0, 5)
    body = []
    if shape == 0:
        body.append(f"    return {cond} ? {arm(r, ty)} : {arm(r, ty)};")
    elif shape == 1:
        body.append(f"    {ty} x = {arm(r, ty)};")
        body.append(f"    if ({cond}) x = {arm(r, ty)};")
        body.append("    return x;")
    elif shape == 2:
        body.append(f"    {ty} x;")
        body.append(f"    if ({cond}) x = {arm(r, ty)}; else x = {arm(r, ty)};")
        body.append("    return x;")
    elif shape == 3:
        body.append(f"    {ty} x = a, y = b;")
        body.append(f"    if ({cond}) {{ x = {arm(r, ty)}; y = {arm(r, ty)}; }}")
        body.append("    return (" + ty + ")(x ^ (y << 1));")
    elif shape == 4:
        body.append(f"    {ty} x = {arm(r, ty)};")
        body.append(f"    if (!({cond})) x = {arm(r, ty)};")
        body.append("    return x;")
    else:
        body.append(f"    {ty} s = 0;")
        body.append("    for (int i = 0; i < 5; i++) {")
        body.append(f"        {ty} t = ({ty})(a + i);")
        body.append(f"        s = ({ty})(s + (t {op} b ? t : ({ty})1));")
        body.append("    }")
        body.append("    return s;")
    src = f"static __attribute__((noinline)) {ty} f{k}({ty} a, {ty} b) {{\n" + "\n".join(body) + "\n}\n"
    lo, hi = TYPES[ty]
    vals = sorted({lo, lo + 1, -1 if lo < 0 else 2, 0, 1, hi - 1, hi, r.randint(lo, hi), r.randint(lo, hi)})
    vals = [v for v in vals if lo <= v <= hi]
    calls = []
    fmt = "%llu" if ty.startswith("unsigned") else "%lld"
    cast = "unsigned long long" if ty.startswith("unsigned") else "long long"
    for x in vals:
        for y in r.sample(vals, min(4, len(vals))):
            calls.append(f'    printf("{k} {fmt}\\n", ({cast})f{k}({lit(ty, x)}, {lit(ty, y)}));')
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
