#!/bin/sh
# /lib/gpio と /bin/pinctrl を QEMU で試す（QEMU の GPIO は機能・出力・値だけ。プルとエッジは実機で:
#   nos.py push tools/test/gpio/gpiotest.c /root/test/gpiotest.c; nos.py cmd 'cc -o gpiotest -lgpio /root/test/gpiotest.c && ./gpiotest hw'）
#   sh tools/test/gpio.sh [KERNEL]   （既定は標準の build/kslim/kernel8.img。先に make sdimage）
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$R/tools/test/gpio/gpiotest.c" ::/root/test/
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o gpiotest -lgpio gpiotest.c && ./gpiotest > t.txt; echo st $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:pinctrl set 5 op dh > p.txt; pinctrl get 5,14 >> p.txt; pinctrl -p get 29 >> p.txt; pinctrl set 5 dl; pinctrl lev 5 >> p.txt\r' 'until:/root/test> :120' \
    'keys:pinctrl set 5 ip; pinctrl get 5 >> p.txt; pinctrl funcs 18 >> p.txt; pinctrl set 14 op 2>> p.txt; echo st $? >> p.txt; echo done""-1\r' 'until:done-1:120' \
    > "$T/qemu.log" 2>&1 || true
for f in t p; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
if grep -q "^all ok" "$T/t.txt" && grep -qx "st 0" "$T/t.txt"; then echo "ok: gpiotest ($(grep -c '^ok' "$T/t.txt") 項目)"
else echo "NG: gpiotest"; cat "$T/t.txt"; fail=1; fi
cat > "$T/want.txt" <<'W'
 5: op dh -- | hi // GPIO5 = output
14: a0    -- | ?? // GPIO14 = TXD0
0
 5: ip    -- | lo // GPIO5 = input
18, GPIO18, PCM_CLK, SD10, DPI_D14, I2CSL_SDA_MOSI, SPI1_CE0_N, PWM0
pinctrl: GPIO14 (GPIO14) is used by Monocs
st 1
W
# -p get 29 はヘッダーの 29 番 = GPIO5 で、その行は ` 29: …` で始まる（出したもの）
# GPIO14 の値は線の状態による（QEMU は lo、実機は hi）ので ?? にして比べる
grep -v '^29:' "$T/p.txt" | sed 's/^\(14: a0    -- | \)..\( \)/\1??\2/' > "$T/p2.txt" || true
if diff "$T/want.txt" "$T/p2.txt" > "$T/d" && grep -q '^29: op dh -- | hi // GPIO5 = output' "$T/p.txt"; then echo "ok: pinctrl"
else echo "DIFF: pinctrl (< want, > Monocs)"; cat "$T/d"; cat "$T/p.txt"; fail=1; fi
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
