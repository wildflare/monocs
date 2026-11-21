#!/bin/sh
# OpenMP（cc/omp.inc・os/libc/omp.c）を、GCC の -fopenmp（libgomp）と比べる。
#   sh tools/test/openmp.sh [KERNEL]   （既定は標準の slimcc 版 build/kslim/kernel8.img。GCC 版は build/kernel8.img。先に make sdimage）
# 1. Linux: slimcc -fopenmp でコンパイルし、os/libc/omp.c（MONOCS_OMP_LINUX）とつないだものが、GCC と同じ出力
#    （スレッドの数 1・2・3・既定）
# 2. Monocs（QEMU）: cc -o omptest -fopenmp omptest.c && ./omptest の出力が GCC と同じ（2 回）
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
D=$R/tools/test/openmp
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -O1 -fopenmp -o "$T/gcc" "$D/omptest.c"
"$T/gcc" > "$T/gcc.txt"
gcc -std=gnu2x -O1 -c -DMONOCS_OMP_LINUX -pthread -o "$T/rt.o" "$R/os/libc/omp.c"
"$R/cc/slimcc" -fopenmp -I "$R/os/api/include" -c -o "$T/slim.o" "$D/omptest.c"
gcc -o "$T/slim" "$T/slim.o" "$T/rt.o" -pthread
fail=0
for n in 1 2 3 ""; do
    if [ -n "$n" ]; then export OMP_NUM_THREADS=$n; else unset OMP_NUM_THREADS; fi
    "$T/gcc" > "$T/g.txt"
    timeout 60 "$T/slim" > "$T/s.txt" || true
    if diff "$T/g.txt" "$T/s.txt" > "$T/d"; then echo "ok: Linux slimcc = GCC (threads ${n:-default})"
    else echo "DIFF: Linux threads ${n:-default} (< GCC, > slimcc)"; cat "$T/d"; fail=1; fi
done
unset OMP_NUM_THREADS
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$D/omptest.c" ::/root/test/
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o omptest -fopenmp omptest.c && ./omptest > o1.txt; cc -o omptest -fopenmp omptest.c && ./omptest > o2.txt; echo done""-1\r' 'until:done-1:600' \
    > "$T/qemu.log" 2>&1 || true
for f in o1 o2; do
    mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"
    if diff "$T/gcc.txt" "$T/$f.txt" > "$T/d"; then echo "ok: Monocs $f = GCC ($(wc -l < "$T/$f.txt") 行)"
    else echo "DIFF: Monocs $f (< GCC, > Monocs)"; cat "$T/d"; fail=1; fi
done
grep -a "invalid pointer\|does not stop\|exception" "$T/qemu.log" && fail=1
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
