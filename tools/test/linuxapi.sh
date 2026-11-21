#!/bin/sh
# Linux のプログラムがよく使う関数（tools/test/linuxapi.c）を、Linux（gcc・glibc）と Monocs（QEMU）で動かして比べる。
#   sh tools/test/linuxapi.sh [KERNEL]   （既定は標準の slimcc 版 build/kslim/kernel8.img。GCC 版は build/kernel8.img。先に make sdimage）
# 1. 同じ引数で 2 回動かし、どちらも Linux と同じ出力になること（getopt の状態などがアプリごとに初めに戻る）
# 2. 入れ子（linuxapi_nest.c が system() で linuxapi.c を動かす）で、子が変えた環境変数・getopt の状態が
#    親に残らないこと（最後の 2 行。その前の行の順は Linux と違う: glibc の system() は stdout を書き出さずに
#    子を動かすので、パイプでは親の printf が後に出る。Monocs は書き出してから動かす）。シェルにも残らないこと
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ARGS='-a -b x --long=y --flag file1 -- -file2'
gcc -std=c2x -O1 -o "$T/linuxapi" "$R/tools/test/linuxapi.c"
gcc -std=c2x -O1 -o "$T/linuxapi_nest" "$R/tools/test/linuxapi_nest.c"
(cd "$T" && export LANG=C.UTF-8 && unset LC_ALL LC_CTYPE && ./linuxapi $ARGS > linux.txt && ./linuxapi_nest -p 1 ./linuxapi | tail -2 > linux_nest.txt)

cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$R/tools/test/linuxapi.c" "$R/tools/test/linuxapi_nest.c" ::/root/test/
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 120 'until:/> :120' \
    "keys:cd /root/test; cc -o linuxapi linuxapi.c && ./linuxapi $ARGS > out1.txt; cc -o linuxapi linuxapi.c && ./linuxapi $ARGS > out2.txt; echo done\"\"-runs\\r" \
    'until:done-runs:600' \
    'keys:cc -o linuxapi_nest linuxapi_nest.c && ./linuxapi_nest -p 1 "./linuxapi" | tail -2 > nest.txt; env > env.txt; echo done""-nest\r' \
    'until:done-nest:600' > "$T/qemu.log" 2>&1 || true
for f in out1 out2 nest env; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
for f in out1 out2; do
    if diff "$T/linux.txt" "$T/$f.txt" > "$T/d"; then echo "ok: $f = Linux"; else echo "DIFF: $f (< Linux, > Monocs)"; cat "$T/d"; fail=1; fi
done
if diff "$T/linux_nest.txt" "$T/nest.txt" > "$T/d"; then echo "ok: nest = Linux"; else echo "DIFF: nest"; cat "$T/d"; fail=1; fi
if grep -q MONOCS "$T/env.txt"; then echo "DIFF: シェルに環境変数が残った"; grep MONOCS "$T/env.txt"; fail=1; else echo "ok: env"; fi
grep -a "invalid pointer\|exception" "$T/qemu.log" && fail=1
exit $fail
