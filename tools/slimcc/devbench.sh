#!/bin/sh
# 実機で、カーネルの中のコンパイラの速さを測る（cc -c -o /dev/null の CPU サイクル。3 回の最小）。
#   sh tools/slimcc/devbench.sh [KERNEL...]   （既定は標準の build/kslim/kernel8.img。リポジトリの一番上で）
# カーネルを送って再起動し、25 秒待って（起動直後は CPU のクロックがそろわないことがある）から測る。
# 比べるときは、実機の /src/cc/*.c が同じであること（nos.py pushdir build/selfsrc /src で変わる）。
# 最後に送ったカーネルが実機に残る（標準に戻すときは最後に build/kslim/kernel8.img を）。
set -u
cd "$(dirname "$0")/../.."
N="python3 tools/uart/nos.py"
[ $# -eq 0 ] && set -- build/kslim/kernel8.img
for k in "$@"; do
    echo "== $k"
    $N kernel "$k" > /dev/null 2>&1 || { echo "devbench: $k を送れません"; exit 1; }
    sleep 25
    $N cmd ' clock' 2>&1 | grep -i measured | head -1
    for f in parse codegen; do
        best=
        for i in 1 2 3; do
            c=$($N cmd " perf -e cycles cc -c -o /dev/null /src/cc/$f.c" 2>&1 | grep -i cycles | head -1 | awk '{print $1}' | tr -d ,)
            [ -n "$c" ] || continue
            if [ -z "$best" ] || [ "$c" -lt "$best" ]; then best=$c; fi
        done
        echo "$f.c $(echo "$best" | awk '{printf "%.0fM", $1 / 1e6}') cycles"
    done
done
