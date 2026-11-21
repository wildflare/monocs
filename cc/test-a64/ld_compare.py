#!/usr/bin/env python3
# slimld のイメージと、GNU ld のイメージ（ELF を objcopy -O binary したもの）を比べる。
#
#   python3 ld_compare.py gnu.elf gnu.img slimld.img
#
# GOT の項目の並びは GNU ld と slimld で異なる（ld.c の冒頭を参照）ので、次の違いだけを許す:
#   - .got の中身（ただし、項目の値の集まりは同じであること）
#   - GOT から読む ldr（64 ビット、符号なしオフセット）の即値。両方が同じ値の項目を指していること
import struct
import sys

LOAD = 0x80000


def got_range(elf):
    d = open(elf, "rb").read()
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    for h in hdrs:
        name = d[stroff + h[0]:d.index(b"\0", stroff + h[0])].decode()
        if name == ".got":
            return h[3], h[3] + h[5]
    return None


def main(elf, gnu_img, my_img):
    a = open(gnu_img, "rb").read()
    b = open(my_img, "rb").read()
    if len(a) != len(b):
        sys.exit(f"size differs: GNU ld {len(a)}, slimld {len(b)}")
    got = got_range(elf)
    q = lambda img, addr: struct.unpack_from("<Q", img, addr - LOAD)[0]
    if got:
        lo, hi = got
        va = sorted(q(a, x) for x in range(lo, hi, 8))
        vb = sorted(q(b, x) for x in range(lo, hi, 8))
        if va != vb:
            sys.exit(f"GOT values differ: {va} vs {vb}")
    bad = 0
    ldr_ok = 0
    for ofs in range(0, len(a) - 3, 4):
        if a[ofs:ofs + 4] == b[ofs:ofs + 4]:
            continue
        addr = ofs + LOAD
        if got and got[0] <= addr < got[1]:
            continue
        ia, = struct.unpack_from("<I", a, ofs)
        ib, = struct.unpack_from("<I", b, ofs)
        # ldr Xt, [Xn, #imm]（0xF9400000）で、レジスタが同じ
        if got and (ia & 0xFFC00000) == 0xF9400000 and (ia & 0x3FF) == (ib & 0x3FF) \
                and (ib & 0xFFC00000) == 0xF9400000:
            page = got[0] & ~0xFFF
            ea = page + ((ia >> 10) & 0xFFF) * 8
            eb = page + ((ib >> 10) & 0xFFF) * 8
            if (got[1] - 1) & ~0xFFF == page and got[0] <= ea < got[1] and got[0] <= eb < got[1] \
                    and q(a, ea) == q(b, eb):
                ldr_ok += 1
                continue
        print(f"  differs at {addr:#x}: GNU ld {ia:08x}, slimld {ib:08x}")
        bad += 1
        if bad > 20:
            break
    if bad:
        sys.exit(1)
    print(f"same ({len(a)} bytes; GOT loads that differ only in the slot: {ldr_ok})")


if __name__ == "__main__":
    main(*sys.argv[1:])
