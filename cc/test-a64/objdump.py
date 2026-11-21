#!/usr/bin/env python3
# GNU as が出力した ELF の .o を、slimcc の -dump-internal-as と同じ形式で出力する。
# 内蔵アセンブラの出力と比べるために使う（test-a64/asm_compare.sh）。
#
#   python3 objdump.py file.o
#
# 比べないもの: 空のセクション、セクションシンボル、ファイルシンボル、.L で始まるシンボル、
# マッピングシンボル（$x、$d）

import struct
import sys


def main(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("not a 64-bit little-endian ELF file")
    (e_shoff,) = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    shdrs = []
    for i in range(e_shnum):
        shdrs.append(struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize))

    def strat(tab_off, idx):
        end = data.index(b"\0", tab_off + idx)
        return data[tab_off + idx:end].decode()

    shstr = shdrs[e_shstrndx][4]
    names = [strat(shstr, h[0]) for h in shdrs]

    symtab = None
    for i, h in enumerate(shdrs):
        if h[1] == 2:  # SHT_SYMTAB
            symtab = i
    syms = []
    if symtab is not None:
        h = shdrs[symtab]
        strtab = shdrs[h[6]][4]
        for j in range(h[5] // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", data, h[4] + j * 24)
            syms.append((strat(strtab, st_name), st_info, st_other, st_shndx, st_value, st_size))

    rels = {}
    for i, h in enumerate(shdrs):
        if h[1] == 4:  # SHT_RELA
            target = h[7]
            lst = []
            for j in range(h[5] // 24):
                r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, h[4] + j * 24)
                sym = syms[r_info >> 32]
                rtype = r_info & 0xFFFFFFFF
                if (sym[1] & 0xF) == 3:  # STT_SECTION
                    tname = "[" + names[sym[3]] + "]"
                else:
                    tname = sym[0]
                lst.append((r_offset, rtype, tname, r_addend))
            rels[target] = sorted(lst)

    out = []
    order = sorted(range(e_shnum), key=lambda i: names[i])
    for i in order:
        name, h = names[i], shdrs[i]
        sh_type, sh_flags, sh_offset, sh_size, sh_addralign = h[1], h[2], h[4], h[5], h[8]
        if i == 0 or sh_type in (2, 3, 4):  # NULL、SYMTAB、STRTAB、RELA
            continue
        if sh_size == 0 and not rels.get(i) and not name.startswith(".note"):
            continue
        out.append(f"section {name} type={sh_type} flags=0x{sh_flags:x} "
                   f"align={sh_addralign} size={sh_size}")
        if sh_type != 8:  # NOBITS
            body = data[sh_offset:sh_offset + sh_size]
            for j in range(0, sh_size, 32):
                out.append(f"  data {j:06x} " + body[j:j + 32].hex())
        for r in rels.get(i, []):
            out.append(f"  reloc {r[0]:06x} {r[1]} {r[2]} {r[3]:+d}")

    lines = []
    for s in syms:
        name, info, other, shndx, value, size = s
        stype, bind = info & 0xF, info >> 4
        if not name or stype in (3, 4) or name.startswith(".L") or name.startswith("$"):
            continue
        if shndx == 0:
            sect = "*UND*"
        elif shndx == 0xFFF1:
            sect = "*ABS*"
        elif shndx == 0xFFF2:
            sect = "*COM*"
        else:
            sect = names[shndx]
        lines.append(f"symbol {name} bind={bind} type={stype} vis={other} sect={sect} "
                     f"value={value} size={size}")
    out += sorted(lines)
    if out:
        print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
