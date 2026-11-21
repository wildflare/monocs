#!/usr/bin/env python3
# カーネルの関数名の表（SD カードの /boot/kernel8.sym。シェルの perf record が使う。os/shell/perf.c）を作る
#   mksym.py KERNEL8.IMG (--elf KERNEL.ELF | --map KERNEL.MAP) -o OUT.sym
# 1 行目は「# monocs-kernel-sym 識別」。識別は動いているカーネルと同じ計算（os/kernel/ksym.c の ksym_kernel_id）:
# .text・.rodata（0x80000〜__tdata_start）の FNV-1a（64 ビット）に 1 を OR したもの。
# 続く行は「番地 名前」（コードの範囲のシンボルを番地の順に）。
import argparse, subprocess, sys

LOAD = 0x80000

def fnv64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def from_elf(path):
    out = subprocess.run(["nm", "-n", "--defined-only", path], capture_output=True, text=True, check=True).stdout
    syms, tdata = [], None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr, kind, name = int(parts[0], 16), parts[1], parts[2]
        if name == "__tdata_start":
            tdata = addr
        if kind in "tTwW" and not name.startswith("$"):
            syms.append((addr, name))
    return syms, tdata

def from_map(path):
    # slimld の -Map: 「section 番地 大きさ 名前」と「番地 名前」
    syms, tdata, text = [], None, None
    for line in open(path):
        parts = line.split()
        if parts and parts[0] == "section":
            if parts[3] == ".text":
                text = (int(parts[1], 16), int(parts[1], 16) + int(parts[2], 16))
            continue
        if len(parts) != 2:
            continue
        addr, name = int(parts[0], 16), parts[1]
        if name == "__tdata_start":
            tdata = addr
        syms.append((addr, name))
    if text:
        syms = [(a, n) for a, n in syms if text[0] <= a < text[1]]
    return sorted(syms), tdata

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("img")
    ap.add_argument("--elf")
    ap.add_argument("--map")
    ap.add_argument("-o", required=True)
    a = ap.parse_args()
    syms, tdata = from_elf(a.elf) if a.elf else from_map(a.map)
    if tdata is None:
        sys.exit("mksym: __tdata_start が見つかりません")
    img = open(a.img, "rb").read()
    kid = fnv64(img[:tdata - LOAD]) | 1
    with open(a.o, "w") as f:
        f.write("# monocs-kernel-sym %016x\n" % kid)
        for addr, name in syms:
            f.write("%x %s\n" % (addr, name))

main()
