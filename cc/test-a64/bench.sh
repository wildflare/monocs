#!/bin/sh
# コード生成の質の目安を測る（cc/ で実行する）
#   1. slimcc でコンパイルした slimcc（2 世代目）の .text の大きさ
#   2. 2 世代目が、自分のソース一式をアセンブリにする時間（3 回の最小）
#   3. 2 世代目でコンパイルしたベンチマーク（test-a64/bench/*.c）の実行時間（3 回の最小）
set -eu
TMP=${TMPDIR:-/tmp}/slimcc-bench.$$
mkdir -p "$TMP"
# 2 世代目はヘッダーを自分の場所から探すので、cc/ に置く
S2=./slimcc-bench2
trap 'rm -rf "$TMP" "$S2"' EXIT
srcs="alloc assembler bitint codegen hashmap main options parse platform preprocess strings tokenize type unicode"
for f in $srcs; do ./slimcc -O2 -c -o "$TMP/$f.o" "$f.c"; done
./slimcc -o "$S2" $TMP/*.o -lm
text=$(size -A "$S2" | awk '$1 == ".text" {print $2}')
echo "text: $text bytes"

best() {
    b=999999
    for i in 1 2 3; do
        s=$(date +%s%N)
        "$@" >/dev/null
        e=$(( ($(date +%s%N) - s) / 1000000 ))
        [ $e -lt $b ] && b=$e
    done
    echo $b
}
compile_all() {
    for f in $srcs; do "$S2" -S -o "$TMP/out.s" "$f.c"; done
}
echo "self-compile: $(best compile_all) ms"
for b in test-a64/bench/*.c; do
    n=$(basename "$b" .c)
    ./slimcc -O2 -o "$TMP/$n" "$b" -lm
    gcc -O2 -o "$TMP/$n.gcc" "$b" -lm
    echo "$n: $(best "$TMP/$n") ms (gcc -O2: $(best "$TMP/$n.gcc") ms)"
done
