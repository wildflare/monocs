#!/usr/bin/env python3
# switch の振り分け（ir.inc の ir_switch・ir_switch_tree。case が多いときは二分探索）の試験を生成する。
# いろいろな型・case の値（まばら・密集・負・型の端）・範囲（case a ... b）・default のある・ない・
# 落ちる流れを持つ関数を作り、境界のまわりの値で呼んで結果を出す。GCC と slimcc の出力が同じことを
# 確かめる（run.sh の 8d）。2026-11-27 から、表引き（ir_switch_table）にかかる密なもの（型の端に接したもの、
# case が 100・250 個のもの）も。
#
#   python3 switch_gen.py 出力ファイル [乱数の種] [関数の数]

import random
import sys

TYPES = {
    "signed char": (-128, 127), "unsigned char": (0, 255),
    "short": (-32768, 32767), "unsigned short": (0, 65535),
    "int": (-2**31, 2**31 - 1), "unsigned": (0, 2**32 - 1),
    "long": (-2**63, 2**63 - 1), "unsigned long": (0, 2**64 - 1),
}


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


def gen_func(r, k):
    ty = r.choice(list(TYPES))
    lo, hi = TYPES[ty]
    ncase = r.choice([1, 3, 4, 5, 6, 8, 12, 20, 33, 60, 100, 250])
    style = r.choice(["dense", "sparse", "edge", "dense", "dense_lo", "dense_hi"])
    vals = set()
    ranges = []
    base = r.randint(max(lo, -1000), min(hi, 1000) - 200) if hi - lo > 2000 else lo
    if style == "dense_lo":
        base, style = lo, "dense"                       # 型のいちばん小さい値から
    elif style == "dense_hi":
        base, style = max(lo, hi - ncase * 2), "dense"  # 型のいちばん大きい値まで
    while len(ranges) < ncase:
        if style == "dense":
            a = base + r.randint(0, ncase * 2)
        elif style == "sparse":
            a = r.randint(lo, hi)
        else:
            a = r.choice([lo, lo + 1, hi, hi - 1, 0, -1 if lo < 0 else 1, base + r.randint(0, 50)])
        a = max(lo, min(hi, a))
        b = a
        if r.random() < 0.2:
            b = min(hi, a + r.randint(1, 5))
        if any(not (b < x or a > y) for x, y in ranges):
            if r.random() < 0.9:
                continue
            break
        ranges.append((a, b))
    has_def = r.random() < 0.7
    lines = [f"__attribute__((noinline)) int f{k}({ty} v) {{", "  int s = 0;", "  switch (v) {"]
    order = ranges[:]
    r.shuffle(order)
    shared = r.random() < 0.4      # いくつかの case が同じ本体（ビットの表 ir_switch_bits にかかる形。2026-11-27）
    for i, (a, b) in enumerate(order):
        if a == b:
            lines.append(f"  case {lit(ty, a)}:")
        else:
            lines.append(f"  case {lit(ty, a)} ... {lit(ty, b)}:")
        if shared and i + 1 < len(order) and r.random() < 0.75:
            continue                # 本体なし（次の case と同じ行き先）
        lines.append(f"    s += {i + 1};")
        if r.random() < 0.8:
            lines.append("    break;")
    if has_def:
        lines.append("  default:")
        lines.append("    s += 1000;")
    lines.append("  }")
    lines.append("  return s;")
    lines.append("}")
    # 呼ぶ値: 各範囲の端とそのとなり、型の端、乱数
    probes = set()
    for a, b in ranges:
        for x in (a - 1, a, b, b + 1, (a + b) // 2):
            if lo <= x <= hi:
                probes.add(x)
    for x in (lo, hi, 0):
        probes.add(x)
    for _ in range(5):
        probes.add(r.randint(lo, hi))
    calls = [f"  printf(\"f{k} %s %d\\n\", \"{x}\", f{k}(({ty}){lit(ty, x)}));" for x in sorted(probes)]
    return lines, calls


def main():
    out = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    r = random.Random(seed)
    src = ["#include <stdio.h>"]
    calls = []
    for k in range(n):
        lines, c = gen_func(r, k)
        src += lines
        calls += c
    src.append("int main(void) {")
    src += calls
    src.append("  return 0;")
    src.append("}")
    open(out, "w").write("\n".join(src) + "\n")


main()
