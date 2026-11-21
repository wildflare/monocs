#!/bin/sh
# os/libc/glob.c を Linux の glibc の glob と比べる（試すフォルダは一時フォルダに作る）
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# GLOB_ONLYDIR などで AddressSanitizer の glob の差し込みが落ちる（glibc を呼んだところで）ので、比べるのは
# 検査なし（-fsanitize=undefined だけ）。Monocs の glob は、下でもう一度、検査付きで同じパターンを流す
for v in plain asan; do
    san=-fsanitize=undefined
    [ $v = asan ] && san=-fsanitize=address,undefined
    gcc -std=c23 -O1 -g $san -c -Dglob=monocs_glob -Dglobfree=monocs_globfree -o "$T/glob_$v.o" "$R/os/libc/glob.c"
    gcc -std=c23 -O1 -g $san -o "$T/test_glob_$v" "$R/tools/libc/test_glob.c" "$T/glob_$v.o" \
        $([ $v = asan ] && echo -DONLY_MONOCS)
done
mkdir -p "$T/d/sub/deep" "$T/d/other" "$T/d/dir.d" "$T/home"
cd "$T/d"
touch a.c b.c zz.c x.h .hidden .h2 sub/a sub/b.txt sub/deep/c.txt other/o.c dir.d/f '[' 'a['
mkdir -p sub/s2 && touch sub/s2/b.txt
touch "$T/home/xa" "$T/home/xb"
HOME="$T/home" "$T/test_glob_plain"
HOME="$T/home" "$T/test_glob_asan"
