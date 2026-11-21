#!/bin/sh
# /lib/spi と /bin/spi を QEMU で試す。QEMU の SPI0 に SPI フラッシュ（w25q32）をつなぐ。QEMU は CS を動かさない
# （フラッシュはいつも選ばれたまま）ので、フラッシュに通じるのは起動してから最初の命令だけ。起動を 2 回に分ける。
#   sh tools/test/spi.sh [KERNEL]   （既定は標準の build/kslim/kernel8.img。先に make sdimage）
# 実機（何もつながない）: nos.py push tools/test/spi/spitest.c /root/test/spitest.c; nos.py cmd 'cc -o spitest -lspi -lgpio /root/test/spitest.c && ./spitest hw'
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$R/tools/test/spi/spitest.c" ::/root/test/
export QEMU_EXTRA="-device w25q32,bus=spi"
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o spitest -lspi -lgpio spitest.c && ./spitest > t.txt; echo st $? >> t.txt; echo done""-1\r' 'until:done-1:120' > "$T/q1.log" 2>&1 || true
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:spi 9f -r 3 > c.txt; spi -c 1 -m 3 -s 5000000 1 2 3 >> c.txt; spi -m 4 2>> c.txt; spi zz 2>> c.txt; echo done""-2\r' \
    'until:done-2:120' > "$T/q2.log" 2>&1 || true
for f in t c; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
if grep -q "^all ok" "$T/t.txt" && grep -qx "st 0" "$T/t.txt"; then echo "ok: spitest ($(grep -c '^ok' "$T/t.txt") 項目)"
else echo "NG: spitest"; cat "$T/t.txt"; fail=1; fi
printf '%s\n' '0x00 0xef 0x40 0x16' '0x00 0x00 0x00' 'spi: mode must be 0-3' 'spi: bad byte "zz"' > "$T/want.txt"
if diff "$T/want.txt" "$T/c.txt" > "$T/d"; then echo "ok: spi コマンド"
else echo "DIFF: spi コマンド (< want, > Monocs)"; cat "$T/d"; fail=1; fi
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
