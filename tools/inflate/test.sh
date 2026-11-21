#!/bin/sh
# /lib/inflate（展開）のテスト（Linux）。リポジトリの一番上で:  sh tools/inflate/test.sh
#   1. gzip（-1・-6・-9）・zlib・DEFLATE だけ・圧縮なしのブロックを、いろいろな中身と読む大きさ（16KB・1・7 バイト）で
#      展開して、元と比べる（GCC の検査付きと、slimcc でコンパイルしたもの）
#   2. 続けて並んだ gzip の塊、後ろに 0 の詰め物があるもの
#   3. 壊した入力 1,500 個で、落ちない（検査に引っかからない）こと
set -u
cd "$(dirname "$0")/../.."
R=$(pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -std=gnu23 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Ilib/inflate -o "$T/asan" tools/inflate/inftest.c lib/inflate/inflate.c || exit 1
cc/slimcc -std=gnu23 -Ilib/inflate -o "$T/slim" tools/inflate/inftest.c lib/inflate/inflate.c || exit 1
cd "$T"
: > empty
head -c 1 /dev/urandom > one
head -c 300000 /dev/urandom > rand
head -c 5000000 /dev/zero > zeros
cat "$R"/cc/*.c > src
cat "$R"/cc/slimcc "$R"/cc/slimcc > bin
python3 -c "open('rep','wb').write(b'abc'*100000 + bytes(range(256))*500)"
fail=0
check() {   # check 名前 形式 入力 元 [読む大きさ]
    for b in ./asan ./slim; do
        $b "$2" ${5:-16384} < "$3" > t.out 2> t.err && cmp -s t.out "$4" || { fail=$((fail + 1)); echo "FAIL $1 $b: $(head -1 t.err)"; }
    done
}
for f in empty one rand zeros src bin rep; do
    for lv in 1 6 9; do
        gzip -$lv -c $f > t.gz
        check "gzip -$lv $f" 2 t.gz $f
        check "auto gzip $f" 3 t.gz $f
        if [ "$(stat -c %s t.gz)" -lt 400000 ]; then
            check "gzip -$lv $f (1 byte reads)" 2 t.gz $f 1
            check "gzip -$lv $f (7 byte reads)" 2 t.gz $f 7
        fi
    done
    python3 -c "
import zlib
d = open('$f', 'rb').read()
open('t.z', 'wb').write(zlib.compress(d, 6))
c = zlib.compressobj(6, zlib.DEFLATED, -15); open('t.raw', 'wb').write(c.compress(d) + c.flush())
c = zlib.compressobj(0); open('t.z0', 'wb').write(c.compress(d) + c.flush())"
    check "zlib $f" 1 t.z $f
    check "auto zlib $f" 3 t.z $f
    check "raw $f" 0 t.raw $f
    check "stored $f" 1 t.z0 $f
done
(gzip -c src; gzip -c rep) > cat.gz; cat src rep > cat.want
check "concatenated members" 2 cat.gz cat.want
(gzip -c src; head -c 512 /dev/zero) > pad.gz
check "zero padding after the member" 2 pad.gz src
echo "compare: failures=$fail"
gzip -9 -c src > s.gz
python3 - <<'PY' || fail=$((fail + 1))
import subprocess, random, os, sys
d = open('s.gz', 'rb').read(); random.seed(1); crash = 0; ok = 0
for i in range(1500):
    b = bytearray(d)
    k = random.randrange(4)
    if k == 0:
        for _ in range(random.randrange(1, 4)): b[random.randrange(len(b))] ^= 1 << random.randrange(8)
    elif k == 1: b = b[:random.randrange(len(b))]
    elif k == 2:
        p = random.randrange(len(b)); b[p:p + random.randrange(1, 50)] = os.urandom(random.randrange(1, 50))
    else: b = bytearray(os.urandom(random.randrange(1, 400)))
    for fmt in ('2', '3', '0'):
        r = subprocess.run(['./asan', fmt], input=bytes(b), capture_output=True)
        if r.returncode not in (0, 1): crash += 1; print('CRASH', i, fmt, r.returncode, r.stderr[-300:])
        if r.returncode == 0: ok += 1
print('fuzz: crashes', crash, '(accepted', ok, 'of 4500)')
sys.exit(1 if crash else 0)
PY
cat bin bin bin src src src | gzip -6 > big.gz
s=$(date +%s.%N); ./slim 2 < big.gz | wc -c | tr '\n' ' '; e=$(date +%s.%N)
echo "bytes from $(stat -c %s big.gz) in $(echo "$e - $s" | bc) s (slimcc)"
[ $fail -eq 0 ] && echo "inflate test: すべて成功" || echo "inflate test: NG ($fail)"
exit $fail
