#!/bin/sh
# 今の slimcc と、git の版 REF（既定 HEAD）の slimcc で、いろいろなファイルの -S が 1 バイトも変わらないかを比べる。
# 出力を変えないはずの変更（コンパイラの中を速くする、記録の形を変えるなど）のときに使う。
#   sh tools/slimcc/cmp_s.sh [REF]       （リポジトリの一番上で。先に make -C cc slimcc）
set -u
cd "$(dirname "$0")/../.."
REF=${1:-HEAD}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
git archive "$REF" cc | tar -x -C "$T"
make -s -C "$T/cc" slimcc > /dev/null 2>&1 || { echo "cmp_s: $REF の slimcc を作れません"; exit 1; }
# 組み込みのヘッダーを探すので、cc/ に置いて動かす
cp "$T/cc/slimcc" cc/.slimcc_ref
cd cc
bad=0
for f in tokenize.c parse.c codegen.c assembler.c preprocess.c type.c ../tools/test/linuxapi.c test/attribute.c \
         test/asm.c test/inline_asm.c test/atomic.c test/varargs.c test/struct.c test/float.c test/bitfield.c \
         test-a64/bench/nbody.c ../os/kernel/smp.c ../os/kernel/exception.c ../os/libc/memstream.c ../os/net/tcp_api.c \
         ../os/shell/shell.c ../os/fs/vfs.c; do
    F="-Itest -I../os/api/include -I../os/net/port -I../third_party/lwip/src/include -I../third_party/fatfs/source -w"
    for d in "" -DSLIMCC_LIBRARY; do
        ./slimcc $F $d -S -o "$T/a.s" "$f" 2>/dev/null
        ./.slimcc_ref $F $d -S -o "$T/b.s" "$f" 2>/dev/null
        cmp -s "$T/a.s" "$T/b.s" || { echo "DIFF $f $d"; bad=1; }
    done
done
rm -f .slimcc_ref
[ $bad = 0 ] && echo "cmp_s: all same（$REF と）"
exit $bad
