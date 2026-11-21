#!/usr/bin/env python3
# AAPCS64 の呼び出し規約のテストを生成する。
# 呼び出し側（caller.c）と呼ばれる側（callee.c）を別々のコンパイラでコンパイルし、
# 引数と戻り値がビット単位で正しく渡ることを確かめる。
#
#   python3 abi_gen.py 出力ディレクトリ [乱数の種] [関数の数]

import random
import sys

TYPES = [
    # (名前, 定義, 可変長引数で渡せるか)
    ("char", None, False),
    ("signed char", None, False),
    ("unsigned char", None, False),
    ("short", None, False),
    ("unsigned short", None, False),
    ("int", None, True),
    ("unsigned", None, True),
    ("long", None, True),
    ("unsigned long", None, True),
    ("float", None, False),
    ("double", None, True),
    ("long double", None, True),
    ("VP", "typedef void *VP", True),
    ("S1", "struct S1 { char a; }", True),
    ("S3", "struct S3 { char a[3]; }", True),
    ("S6", "struct S6 { short a; int b; }", True),
    ("S12", "struct S12 { int a, b, c; }", True),
    ("S16", "struct S16 { long a, b; }", True),
    ("SA16", "struct SA16 { _Alignas(16) long a; long b; }", True),
    ("SCD", "struct SCD { char c; double d; }", True),
    ("SIF", "struct SIF { int i; float f; }", True),
    ("HF1", "struct HF1 { float a; }", True),
    ("HF2", "struct HF2 { float a, b; }", True),
    ("HF3", "struct HF3 { float a[3]; }", True),
    ("HF4", "struct HF4 { float a, b, c, d; }", True),
    ("HD2", "struct HD2 { double a, b; }", True),
    ("HD3", "struct HD3 { double a; double b[2]; }", True),
    ("HD4", "struct HD4 { double a, b, c, d; }", True),
    ("HL2", "struct HL2 { long double a, b; }", True),
    ("HN2", "struct HN2 { struct { float x; } a; float b; }", True),
    ("UF", "union UF { float f; float g; }", True),
    ("UFI", "union UFI { float f; int i; }", True),
    ("F5", "struct F5 { float a[5]; }", True),
    ("B17", "struct B17 { char a[17]; }", True),
    ("B24", "struct B24 { long a[3]; }", True),
    ("B40", "struct B40 { double d; char c[32]; }", True),
]

TYPE_DEFS = {name: d for name, d, _ in TYPES if d}


def ctype(name):
    d = TYPE_DEFS.get(name)
    if d is None or d.startswith("typedef"):
        return name
    return ("struct " if d.startswith("struct") else "union ") + name


def gen(outdir, seed, nfuncs):
    rnd = random.Random(seed)
    names = [t[0] for t in TYPES]
    va_names = [t[0] for t in TYPES if t[2]]
    rtn_names = names + ["void"]

    hdr = ["#include <stdarg.h>", "#include <string.h>", "#include <stdio.h>", ""]
    for name, d, _ in TYPES:
        if d:
            hdr.append(d + ";")
    hdr.append("")
    # 値は種から決まるバイト列で埋める（浮動小数点も、ビット単位で比べる）
    hdr.append("static void fill(void *p, unsigned long n, unsigned seed) {")
    hdr.append("  unsigned char *b = p;")
    hdr.append("  for (unsigned long i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = seed >> 16; }")
    hdr.append("}")
    hdr.append("extern int failures;")
    hdr.append("")

    funcs = []
    for fi in range(nfuncs):
        variadic = rnd.random() < 0.25
        nargs = rnd.randint(1, 14)
        if variadic:
            args = ["int"] + [rnd.choice(va_names) for _ in range(nargs)]
        else:
            args = [rnd.choice(names) for _ in range(nargs)]
        rtn = rnd.choice(rtn_names)
        funcs.append((fi, rtn, args, variadic))

    def sizeof_expr(t):
        return f"sizeof({ctype(t)})"

    proto = []
    for fi, rtn, args, variadic in funcs:
        if variadic:
            plist = "int n, ..."
        else:
            plist = ", ".join(f"{ctype(t)} a{i}" for i, t in enumerate(args))
        proto.append(f"{ctype(rtn)} f{fi}({plist});")

    common = "\n".join(hdr + proto) + "\n"

    # 呼ばれる側: 受け取った値を、種から作った期待値と比べる。char などは int への変換を考えて比べる
    callee = [common, ""]
    for fi, rtn, args, variadic in funcs:
        if variadic:
            plist = "int n, ..."
        else:
            plist = ", ".join(f"{ctype(t)} a{i}" for i, t in enumerate(args))
        callee.append(f"{ctype(rtn)} f{fi}({plist}) {{")
        if variadic:
            callee.append("  va_list ap; va_start(ap, n);")
            callee.append(f"  if (n != {fi}) {{ printf(\"f{fi}: n\\n\"); failures++; }}")
            for i, t in enumerate(args[1:], 1):
                callee.append(f"  {{ {ctype(t)} e, v = va_arg(ap, {ctype(t)}); fill(&e, {sizeof_expr(t)}, {fi * 100 + i});")
                callee.append(f"    if ({cmp_cond(t, 'e', 'v')}) {{ printf(\"f{fi}: va arg {i} ({t})\\n\"); failures++; }} }}")
            callee.append("  va_end(ap);")
        else:
            for i, t in enumerate(args):
                callee.append(f"  {{ {ctype(t)} e; fill(&e, {sizeof_expr(t)}, {fi * 100 + i});")
                callee.append(f"    if ({cmp_cond(t, 'e', 'a' + str(i))}) {{ printf(\"f{fi}: arg {i} ({t})\\n\"); failures++; }} }}")
        if rtn != "void":
            callee.append(f"  {{ {ctype(rtn)} r; fill(&r, {sizeof_expr(rtn)}, {fi * 100 + 99}); return r; }}")
        callee.append("}")
        callee.append("")

    caller = [common, "int failures;", "", "int main(void) {"]
    for fi, rtn, args, variadic in funcs:
        caller.append("  {")
        for i, t in enumerate(args):
            if variadic and i == 0:
                continue
            caller.append(f"    {ctype(t)} a{i}; fill(&a{i}, {sizeof_expr(t)}, {fi * 100 + i});")
        if variadic:
            alist = ", ".join([str(fi)] + [f"a{i}" for i in range(1, len(args))])
        else:
            alist = ", ".join(f"a{i}" for i in range(len(args)))
        if rtn == "void":
            caller.append(f"    f{fi}({alist});")
        else:
            caller.append(f"    {ctype(rtn)} r = f{fi}({alist}), e; fill(&e, {sizeof_expr(rtn)}, {fi * 100 + 99});")
            caller.append(f"    if ({cmp_cond(rtn, 'e', 'r')}) {{ printf(\"f{fi}: return ({rtn})\\n\"); failures++; }}")
        caller.append("  }")
    caller.append('  printf("%d functions, %d failures\\n", ' + str(nfuncs) + ', failures);')
    caller.append("  return failures != 0;")
    caller.append("}")

    with open(f"{outdir}/callee.c", "w") as f:
        f.write("\n".join(callee) + "\n")
    with open(f"{outdir}/caller.c", "w") as f:
        f.write("\n".join(caller) + "\n")


# パディングのある型は、メンバーごとに比べる（パディングの中身は渡らなくてよい）
PADDED = {
    "SCD": ["c", "d"],
    "S6": ["a", "b"],
}


def cmp_cond(t, a, b):
    """a と b（変数名）が異なるときに真になる C の式"""
    if t in PADDED:
        return " || ".join(f"memcmp(&{a}.{m}, &{b}.{m}, sizeof({a}.{m}))" for m in PADDED[t])
    if t == "long double":
        # long double は 16 バイトすべてが値（AArch64 の binary128）
        return f"memcmp(&{a}, &{b}, 16)"
    return f"memcmp(&{a}, &{b}, sizeof({ctype(t)}))"


if __name__ == "__main__":
    outdir = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    nfuncs = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    gen(outdir, seed, nfuncs)
