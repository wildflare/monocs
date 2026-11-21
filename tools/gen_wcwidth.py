#!/usr/bin/env python3
"""os/libc/wcwidth_table.h を作る（wcwidth の幅の表。os/libc/wcwidth.c が使う）

Python の unicodedata（Unicode のデータ）から、glibc の wcwidth と同じ考え方で分ける:
  幅 0: 結合文字（一般カテゴリ Mn・Me）、書式文字（Cf。ただし U+00AD SOFT HYPHEN と、前に付く連結記号
        Prepended_Concatenation_Mark（U+0600〜U+0605 など。PropList.txt）は 1）、
        ハングルの中声・終声（U+1160〜U+11FF、U+D7B0〜U+D7FF）、U+200B
  幅 2: East Asian Width が W（全角）か F（全角の英数）
  ほかは 1（制御文字・サロゲートは wcwidth.c が -1 にする）

    python3 tools/gen_wcwidth.py > os/libc/wcwidth_table.h
"""
import sys
import unicodedata


def ranges(pred):
    out, start = [], None
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            ok = False
        else:
            ok = pred(cp)
        if ok and start is None:
            start = cp
        elif not ok and start is not None:
            out.append((start, cp - 1))
            start = None
    if start is not None:
        out.append((start, 0x10FFFF))
    return out


# Prepended_Concatenation_Mark（Unicode 15.1 の PropList.txt）
PREPENDED = set(range(0x0600, 0x0606)) | {0x06DD, 0x070F, 0x0890, 0x0891, 0x08E2, 0x110BD, 0x110CD}


def zero(cp):
    if cp == 0x00AD or cp in PREPENDED:
        return False
    if 0x1160 <= cp <= 0x11FF or 0xD7B0 <= cp <= 0xD7FF or cp == 0x200B:
        return True
    return unicodedata.category(chr(cp)) in ('Mn', 'Me', 'Cf')


def wide(cp):
    return unicodedata.east_asian_width(chr(cp)) in ('W', 'F') and not zero(cp)


def emit(name, rs):
    print(f'static const struct wcwidth_range {name}[] = {{')
    line = ''
    for a, b in rs:
        item = f'{{0x{a:X}, 0x{b:X}}}, '
        if len(line) + len(item) > 100:
            print('    ' + line.rstrip())
            line = ''
        line += item
    if line:
        print('    ' + line.rstrip())
    print('};')


print(f'// 生成したもの（tools/gen_wcwidth.py。Unicode {unicodedata.unidata_version}）。手で書き換えない')
print('struct wcwidth_range { unsigned first, last; };')
emit('wcwidth_zero', ranges(zero))
emit('wcwidth_wide', ranges(wide))
