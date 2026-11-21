#!/bin/sh
# pthread（os/libc/pthread.c）を Linux（gcc -pthread、glibc）と Monocs（QEMU）で動かして比べる。
#   sh tools/test/pthread.sh [KERNEL]   （既定は標準の slimcc 版 build/kslim/kernel8.img。GCC 版は build/kernel8.img。先に make sdimage）
# 1. pthtest.c の出力が Linux と同じ（戻り値・mutex・条件変数・once・key・バリア・読み書きのロック・時間切れなど）
# 2. スレッドの中の exit(5) でアプリ全体が 5 で終わる・NULL への書き込みで 139、空いたコアより多く作ると EAGAIN
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
D=$R/tools/test/pthread
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -std=gnu2x -O1 -pthread -o "$T/pthtest" "$D/pthtest.c"
"$T/pthtest" > "$T/linux.txt"
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$D/pthtest.c" ::/root/test/
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o pthtest pthtest.c && ./pthtest > p.txt; cc -o pthtest pthtest.c && ./pthtest > p2.txt; echo done""-1\r' 'until:done-1:300' \
    'keys:cc -o pthtest pthtest.c && ./pthtest exit; echo exit $? > s.txt; cc -o pthtest pthtest.c && ./pthtest crash; echo crash $? >> s.txt\r' 'until:/root/test> :120' \
    'keys:cc -o pthtest pthtest.c && ./pthtest many >> s.txt; echo done""-2\r' 'until:done-2:120' > "$T/qemu.log" 2>&1 || true
for f in p p2 s; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
for f in p p2; do
    if diff "$T/linux.txt" "$T/$f.txt" > "$T/d"; then echo "ok: $f = Linux ($(wc -l < "$T/$f.txt") 行)"
    else echo "DIFF: $f (< Linux, > Monocs)"; cat "$T/d"; fail=1; fi
done
cat > "$T/want.txt" <<'W'
exit 5
crash 139
made 2, then EAGAIN
again 0
W
if diff "$T/want.txt" "$T/s.txt" > "$T/d"; then echo "ok: exit・例外・EAGAIN"; else echo "DIFF (< want, > Monocs)"; cat "$T/d"; fail=1; fi
grep -a "invalid pointer\|does not stop" "$T/qemu.log" && fail=1
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
