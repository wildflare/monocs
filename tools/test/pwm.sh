#!/bin/sh
# /lib/pwm と /bin/pwm を QEMU で試す。QEMU は PWM そのものを真似ていない（クロックマネージャーは真似ている）ので、
# 引数・ピン・クロックの設定・コマンドの誤りだけ。波は実機で:
#   nos.py push tools/test/pwm/pwmtest.c /root/test/pwmtest.c; nos.py cmd 'cc -o pwmtest -lpwm -lgpio /root/test/pwmtest.c && ./pwmtest hw'
#   sh tools/test/pwm.sh [KERNEL]   （既定は標準の build/kslim/kernel8.img。先に make sdimage）
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$R/tools/test/pwm/pwmtest.c" ::/root/test/
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o pwmtest -lpwm -lgpio pwmtest.c && ./pwmtest > t.txt; echo st $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:pwm 5 1000 50 2> c.txt; pwm 18 0 50 2>> c.txt; pwm 18 1000 25; echo st $? >> c.txt; pwm 18 off; echo done""-1\r' \
    'until:done-1:120' > "$T/qemu.log" 2>&1 || true
for f in t c; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
if grep -q "^all ok" "$T/t.txt" && grep -qx "st 0" "$T/t.txt"; then echo "ok: pwmtest ($(grep -c '^ok' "$T/t.txt") 項目)"
else echo "NG: pwmtest"; cat "$T/t.txt"; fail=1; fi
printf 'pwm: GPIO5 has no PWM (use 12, 13, 18, 19)\npwm: Invalid argument\nst 0\n' > "$T/want.txt"
if diff "$T/want.txt" "$T/c.txt" > "$T/d"; then echo "ok: pwm コマンド"
else echo "DIFF: pwm コマンド (< want, > Monocs)"; cat "$T/d"; fail=1; fi
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
