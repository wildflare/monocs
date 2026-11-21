#!/bin/sh
# /lib/i2c と /bin の i2cdetect・i2cget・i2cset を QEMU で試す。QEMU のバス 1 に EEPROM（at24c、0x50）と
# 温度センサー（tmp105、0x48）をつなぐ。
#   sh tools/test/i2c.sh [KERNEL]   （既定は標準の build/kslim/kernel8.img。先に make sdimage）
# 実機（何もつないでいないバス 1）: nos.py push tools/test/i2c/i2ctest.c /root/test/i2ctest.c; nos.py cmd 'cc -o i2ctest -li2c -lgpio /root/test/i2ctest.c && ./i2ctest empty'
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$R/tools/test/i2c/i2ctest.c" ::/root/test/
export QEMU_EXTRA="-device at24c-eeprom,bus=i2c-bus.1,address=0x50,rom-size=256 -device tmp105,bus=i2c-bus.1,address=0x48"
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' 'keys:cd /root/test\r' 'until:/root/test> :30' \
    'keys:cc -o i2ctest -li2c -lgpio i2ctest.c && ./i2ctest > t.txt; echo st $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:i2cdetect -y 1 > c.txt; i2cdetect -y 0 0x40 0x4f >> c.txt; i2cset -y 1 0x50 0x20 0x34 >> c.txt\r' 'until:/root/test> :120' \
    'keys:i2cget -y 1 0x50 0x20 >> c.txt; i2cget -y 1 0x48 2 w >> c.txt; i2cset -y 1 0x50 0x60 0x1234 w >> c.txt\r' 'until:/root/test> :120' \
    'keys:i2cget -y 1 0x50 0x60 i 3 >> c.txt; i2cset -y -r 1 0x50 0x21 0x7 2>> c.txt; i2cset -y -m 0x0f 1 0x50 0x21 0xf0 >> c.txt\r' 'until:/root/test> :120' \
    'keys:i2cget -y 1 0x50 0x21 >> c.txt; i2cset -y 1 0x50 0x70 1 2 3 i >> c.txt; i2cget -y 1 0x50 0x70 i 4 >> c.txt\r' 'until:/root/test> :120' \
    'keys:i2cset -y 1 0x50 0x10 c >> c.txt; i2cget -y 1 0x50 >> c.txt; i2cget -y 1 0x13 2>> c.txt; echo st $? >> c.txt\r' 'until:/root/test> :120' \
    'keys:i2cget -y 3 0x50 2>> c.txt; i2cget -y 1 0x05 2>> c.txt; i2cdetect -l >> c.txt; echo done""-1\r' 'until:done-1:120' \
    > "$T/qemu.log" 2>&1 || true
for f in t c; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
if grep -q "^all ok" "$T/t.txt" && grep -qx "st 0" "$T/t.txt"; then echo "ok: i2ctest ($(grep -c '^ok' "$T/t.txt") 項目)"
else echo "NG: i2ctest"; cat "$T/t.txt"; fail=1; fi
printf '%s\n' \
'     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f' \
'00:                         -- -- -- -- -- -- -- -- ' \
'10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- ' \
'20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- ' \
'30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- ' \
'40: -- -- -- -- -- -- -- -- 48 -- -- -- -- -- -- -- ' \
'50: 50 -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- ' \
'60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- ' \
'70: -- -- -- -- -- -- -- --                         ' \
'     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f' \
'00:                                                 ' \
'10:                                                 ' \
'20:                                                 ' \
'30:                                                 ' \
'40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- ' \
'50:                                                 ' \
'60:                                                 ' \
'70:                                                 ' \
'0x34' \
'0x004b' \
'0x34 0x12 0xef' \
'Value 0x07 written, readback matched' \
'0x00' \
'0x01 0x02 0x03 0x00' \
'0x68' \
'Error: Read failed' \
'st 2' \
"Error: Could not open file \`/dev/i2c-3' or \`/dev/i2c/3': No such file or directory" \
'Error: Chip address out of range (0x08-0x77)!' \
'i2c-0	i2c       	3f205000.i2c (GPIO0/1)          	I2C adapter' \
'i2c-1	i2c       	3f804000.i2c (GPIO2/3)          	I2C adapter' > "$T/want.txt"
if diff "$T/want.txt" "$T/c.txt" > "$T/d"; then echo "ok: i2cdetect・i2cget・i2cset"
else echo "DIFF: コマンド (< want, > Monocs)"; cat "$T/d"; fail=1; fi
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
