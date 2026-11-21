#!/bin/sh
# /lib/softuart と /bin/softuart を QEMU で試す（TX と RX を同じピンにして、送ったものを裏のコアで受ける）。
# QEMU は仮想の CPU を交互に動かし、2 つのコアの時間が合わないので、受けたものの正しさは見ない（使い方だけ）。
# 送って受けるのは実機の hw で。
#   sh tools/test/softuart.sh [KERNEL]   （既定は標準の build/kslim/kernel8.img。先に make sdimage）
# 実機（線なし、GPIO5）: nos.py push tools/test/softuart/sutest.c /root/test/sutest.c; nos.py cmd 'cc -o sutest -lsoftuart -lgpio /root/test/sutest.c && ./sutest hw'
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$R/tools/test/softuart/sutest.c" ::/root/test/
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o sutest -lsoftuart -lgpio sutest.c && ./sutest > t.txt; echo st $? >> t.txt\r' 'until:/root/test> :180' \
    'keys:softuart -x abc 5 -1 > c.txt; softuart -b 50 5 5 2>> c.txt; echo st $? >> c.txt; echo done""-1\r' \
    'until:done-1:120' > "$T/qemu.log" 2>&1 || true
for f in t c; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
if grep -q "^all ok" "$T/t.txt" && grep -qx "st 0" "$T/t.txt"; then echo "ok: sutest ($(grep -c '^ok' "$T/t.txt") 項目)"
else echo "NG: sutest"; cat "$T/t.txt"; fail=1; fi
printf 'softuart: open: Invalid argument\nst 1\n' > "$T/want.txt"
if diff "$T/want.txt" "$T/c.txt" > "$T/d"; then echo "ok: softuart コマンド"
else echo "DIFF: softuart コマンド (< want, > Monocs)"; cat "$T/d"; fail=1; fi
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
