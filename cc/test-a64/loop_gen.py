#!/usr/bin/env python3
# ループの変形（ir.inc の反転 ir_stmt・展開 ir_unroll_for・LICM ir_licm、sched.inc の並べ替え）の試験を
# 生成する。いろいろな形の for ループを持つ関数を作り、いろいろな引数で呼んで結果を出す。
# GCC と slimcc でコンパイルしたものの出力が同じことを確かめる（run.sh の 8）。
# 未定義動作（符号つきのあふれ、範囲外の添字）にならない値だけを使う。
#
#   python3 loop_gen.py 出力ファイル [乱数の種] [関数の数]

import random
import sys

TYPES = ["signed char", "short", "int", "long", "unsigned", "unsigned long"]
LIMIT = {"signed char": 100, "short": 3000, "int": 100000, "long": 100000,
         "unsigned": 100000, "unsigned long": 100000}


def gen_func(r, k):
    ty = r.choice(TYPES)
    unsigned = ty.startswith("unsigned")
    step = r.choice([1, 1, 1, 2, 3])
    up = r.random() < 0.6 or unsigned
    if not up:
        step = -step
    if up:
        op = r.choice(["<", "<="])
    else:
        op = r.choice([">", ">="])
    flip = r.random() < 0.3      # E op V の形
    bound_const = r.random() < 0.4
    acc_ty = r.choice(["unsigned long", "unsigned"])
    lines = []
    lines.append(f"__attribute__((noinline)) long f{k}({ty} lo, {ty} hi, int m) {{")
    lines.append(f"  {acc_ty} acc = 1;")
    lines.append("  int cnt = 0;")
    lines.append("  double d = 0;")
    if bound_const:
        b = r.randint(0, 40) if up else r.randint(-40, 0) if not unsigned else 0
        bound = str(b)
        hi_use = "(void)hi;"
    else:
        bound = "hi"
        hi_use = ""
    if hi_use:
        lines.append("  " + hi_use)
    v = "v"
    if flip:
        rev = {"<": ">", "<=": ">=", ">": "<", ">=": "<="}[op]
        cond = f"{bound} {rev} {v}"
    else:
        cond = f"{v} {op} {bound}"
    inc = r.choice([f"{v}++" if step == 1 else f"{v} += {step}",
                    f"++{v}" if step == 1 else f"{v} = {v} + {step}" if r.random() < 0.3 else f"{v} += {step}"])
    if step == -1:
        inc = r.choice([f"{v}--", f"--{v}", f"{v} -= 1"])
    elif step < 0:
        inc = f"{v} -= {-step}"
    lines.append(f"  for ({ty} {v} = lo; {cond}; {inc}) {{")
    body = []
    nstmt = r.randint(1, 4)
    for _ in range(nstmt):
        c = r.randint(0, 9)
        if c == 0:
            body.append(f"    if (({v} & 3) == (m & 3)) continue;")
        elif c == 1:
            body.append(f"    if (cnt > m + 50) break;")
        elif c == 2:
            body.append(f"    if (cnt == m + 77) return -cnt;")
        elif c == 3:
            body.append(f"    for (int w = 0; w < (int)({v} & 3); w++) acc += w * 3 + 1;")
        elif c == 4:
            body.append(f"    acc = acc * 3 + (long){v} % 7;")
        elif c == 5:
            body.append(f"    arr[(cnt + m) & 63] += (int){v};")
        elif c == 6:
            body.append(f"    acc += arr[((unsigned long){v} + cnt) & 63];")
        elif c == 7:
            body.append(f"    {{ long t = (long){v} * 5 - m; acc += t > 0 ? t : -t; d = d * 0.5 + (double){v}; }}")
        elif c == 8:
            body.append(f"    acc += sink((long){v});")
        else:
            body.append(f"    acc ^= (long){v} << (cnt & 7);")
    body.append("    cnt++;")
    lines += body
    lines.append("  }")
    lines.append("  return (long)(acc & 0xffffffffff) + cnt * 1000003L + (long)(d * 16);")
    lines.append("}")
    return ty, unsigned, up, LIMIT[ty], lines


def main():
    out = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    r = random.Random(seed)
    src = ["#include <stdio.h>", "static int arr[64];",
           "__attribute__((noinline)) long sink(long x) { arr[x & 63]++; return x & 15; }"]
    calls = []
    for k in range(n):
        ty, unsigned, up, lim, lines = gen_func(r, k)
        src += lines
        for _ in range(6):
            span = r.choice([0, 1, 2, 3, 4, 5, 7, 8, 9, 31, 64])
            if unsigned:
                lo = r.randint(0, 50)
                hi = lo + span if r.random() < 0.8 else max(0, lo - 1)
            else:
                lo = r.randint(-50, 50)
                hi = lo + span if up else lo - span
                if r.random() < 0.15:
                    hi = lo - 1 if up else lo + 1
            m = r.randint(-5, 20)
            calls.append(f"  printf(\"f{k} %ld\\n\", f{k}({lo}, {hi}, {m}));")
    src.append("int main(void) {")
    src += calls
    src.append("  long s = 0; for (int i = 0; i < 64; i++) s = s * 31 + arr[i];")
    src.append("  printf(\"arr %ld\\n\", s);")
    src.append("  return 0;")
    src.append("}")
    open(out, "w").write("\n".join(src) + "\n")


main()
