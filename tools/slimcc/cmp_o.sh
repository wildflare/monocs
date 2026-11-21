#!/bin/sh
# -c の .o が、かけらの道（既定。中間表現の関数の記録を内蔵アセンブラが直接読む）と、文字列の道
# （-fno-slimcc-direct）で 1 バイトも変わらないかを比べる。cc/ir.inc の記録・cc/assembler.c の frag_insn を
# 変えたときに使う。かけらの道でだけ失敗したファイルも数える。
#   sh tools/slimcc/cmp_o.sh       （リポジトリの一番上で。先に make -C cc slimcc）
# test/macro.c は __TIME__ を使うので、2 回のコンパイルの間に秒が変わると違う（もう一度流す）
set -u
cd "$(dirname "$0")/../../cc"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0; n=0
for f in *.c test/*.c ../tools/test/linuxapi.c test-a64/bench/*.c ../os/kernel/*.c ../os/libc/*.c ../os/net/*.c \
         ../os/shell/*.c ../os/fs/vfs.c ../os/drivers/usb.c ../os/drivers/wifi.c; do
    F="-Itest -I../os/api/include -I../os/net/port -I../third_party/lwip/src/include -I../third_party/fatfs/source -I. -w"
    if ! ./slimcc $F -c -o "$T/d.o" "$f" 2>/dev/null; then
        ./slimcc $F -fno-slimcc-direct -c -o "$T/t.o" "$f" 2>/dev/null && { echo "DIRECT FAIL $f"; bad=1; }
        continue
    fi
    ./slimcc $F -fno-slimcc-direct -c -o "$T/t.o" "$f" 2>/dev/null || { echo "TEXT FAIL $f"; bad=1; continue; }
    n=$((n + 1))
    cmp -s "$T/d.o" "$T/t.o" || { echo "DIFF $f"; bad=1; }
done
[ $bad = 0 ] && echo "cmp_o: all $n same"
exit $bad
