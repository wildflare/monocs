#!/usr/bin/env python3
# ELF の .o、または .a の各メンバーで、セクションの SHF_MERGE と SHF_STRINGS を消したコピーを作る。
# GNU ld に文字列・定数の統合をさせず、slimld（統合しない）の結果とバイト単位で比べるために使う。
#
#   python3 nomerge.py 入力 出力
import struct
import sys

SHF_MERGE, SHF_STRINGS, SHF_ALLOC = 0x10, 0x20, 0x2


def patch_elf(d, base):
    if d[base:base + 4] != b"\x7fELF":
        return
    shoff, = struct.unpack_from("<Q", d, base + 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, base + 0x3A)
    for i in range(shnum):
        p = base + shoff + i * shentsize + 8
        flags, = struct.unpack_from("<Q", d, p)
        if flags & SHF_ALLOC and flags & SHF_MERGE:
            struct.pack_into("<Q", d, p, flags & ~(SHF_MERGE | SHF_STRINGS))


def main(src, dst):
    d = bytearray(open(src, "rb").read())
    if d[:8] == b"!<arch>\n":
        ofs = 8
        while ofs + 60 <= len(d):
            size = int(d[ofs + 48:ofs + 58].decode().strip())
            patch_elf(d, ofs + 60)
            ofs += 60 + size + (size & 1)
    else:
        patch_elf(d, 0)
    open(dst, "wb").write(d)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
