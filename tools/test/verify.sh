#!/bin/sh
# 検証一式（QEMU だけで行う。実機は hwcheck.sh）。リポジトリの一番上で:  sh tools/test/verify.sh [quick]
#
#   1. make cc-test（slimcc-a64 のテスト一式）
#   2. picolibc を slimcc でビルドし直し、変わったかを表示（コード生成を変えたときは変わってよい。
#      変わったら 6 の libctest の一致で確かめる）
#   3. make kernel-slimcc、make ld-test（slimld と GNU ld の一致）
#   4. セルフホスト: QEMU の Monocs（GCC 版・slimcc 版のカーネル）で buildlibc.c・build.c を動かし、
#      できた libc.a・kernel8.img が Linux 上のビルドと一致するか。さらに slimcc 版の 2 世代目
#   5. （quick でなければ）libctest（printf・strtod・数学関数など 2133 行）の出力が GCC 版と slimcc 版で一致するか
#   6. （quick でなければ）ベンチマーク（cc/test-a64/bench.sh）
# quick: 4 の buildlibc.c と 5・6 を飛ばす（カーネルだけを変えたとき。約 5 分）
set -u
cd "$(dirname "$0")/../.."
Q=${1:-}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
QD="python3 tools/test/qdrive.py"
part() { echo "$T/$1@@$((2048 * 512))"; }

make all kernel-gcc 2>&1 | grep -E '\berror\b'
make cc-test > "$T/cctest.log" 2>&1; echo "cc-test exit=$?"
grep -E 'すべて成功|  NG|FAIL' "$T/cctest.log" | head -5

if [ "$Q" != quick ]; then
    cp build/picolibc-slimcc/libc.a "$T/libc_before.a"
    make picolibc-slimcc > "$T/pl.log" 2>&1; echo "picolibc-slimcc exit=$?"
    cmp -s "$T/libc_before.a" build/picolibc-slimcc/libc.a && echo "picolibc: unchanged" || echo "picolibc: CHANGED（libctest で確かめる）"
fi

make kernel-slimcc > "$T/ks.log" 2>&1; echo "kernel-slimcc exit=$?"
make ld-test 2>&1 | grep -E 'same|differ|rror'
make sdimage-self 2>&1 | tail -1

for k in gcc:build/kernel8.img slim:build/kslim/kernel8.img; do
    n=${k%%:*}; kk=${k#*:}
    cp build/sdcard-self.img "$T/v_$n.img"
    if [ "$Q" = quick ]; then
        $QD "$kk" "$T/v_$n.img" 3000 -- 'cc -o /tmp/hello /usr/share/samples/hello.c' '/tmp/hello' 'cd /usr/src/kernel' 'cc -o build-kernel build.c' './build-kernel' 'cd /' 2>&1 |
            grep -aE 'Hello|done \(|failed|\*\*\* exception|TIMEOUT|undefined'
    else
        $QD "$kk" "$T/v_$n.img" 3000 -- 'cc -o /tmp/hello /usr/share/samples/hello.c' '/tmp/hello' 'cd /usr/src/kernel' 'cc -o buildlibc buildlibc.c' './buildlibc' 'cc -o build-kernel build.c' './build-kernel' 'cd /' 2>&1 |
            grep -aE 'Hello|done \(|failed|\*\*\* exception|TIMEOUT|undefined'
        mcopy -o -i "$(part v_$n.img)" ::/usr/src/kernel/build/picolibc-slimcc/libc.a "$T/v_$n.libc" &&
            cmp -s "$T/v_$n.libc" build/picolibc-slimcc/libc.a && echo "$n: libc IDENTICAL" || echo "$n: libc DIFF"
    fi
    mcopy -o -i "$(part v_$n.img)" ::/usr/src/kernel/build/kslim/kernel8.img "$T/v_$n.out" &&
        cmp -s "$T/v_$n.out" build/kslim/kernel8.img && echo "$n: kernel IDENTICAL" || echo "$n: kernel DIFF"
done
cp build/sdcard-self.img "$T/g2.img"
$QD "$T/v_slim.out" "$T/g2.img" -- 'cd /usr/src/kernel' 'cc -o build-kernel build.c' './build-kernel' 2>&1 | grep -aE 'done \(|failed'
mcopy -o -i "$(part g2.img)" ::/usr/src/kernel/build/kslim/kernel8.img "$T/g2.out" &&
    cmp -s "$T/g2.out" build/kslim/kernel8.img && echo "gen2: IDENTICAL" || echo "gen2: DIFF"

if [ "$Q" != quick ]; then
    make sdimage > /dev/null 2>&1
    for k in gcc:build/kernel8.img slim:build/kslim/kernel8.img; do
        n=${k%%:*}; kk=${k#*:}
        cp build/sdcard.img "$T/lt_$n.img"
        mcopy -i "$(part lt_$n.img)" tools/picolibc/libctest.c ::/libctest.c
        $QD "$kk" "$T/lt_$n.img" 600 -- 'cc -o /libctest /libctest.c' '/libctest' 2>&1 | tr -d '\r' | sed -n '/\/> \/libctest$/,$p' > "$T/lt_$n.out"
    done
    if cmp -s "$T/lt_gcc.out" "$T/lt_slim.out" && grep -q 'total 2133 lines' "$T/lt_gcc.out"; then
        echo "libctest: IDENTICAL（$(grep 'total' "$T/lt_gcc.out")）"
    else
        echo "libctest: DIFF"; diff "$T/lt_gcc.out" "$T/lt_slim.out" | head
    fi
    (cd cc && sh test-a64/bench.sh 2>&1 | tail -5)
fi
