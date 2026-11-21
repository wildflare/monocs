#!/bin/sh
# SD カードの /usr/lib に入れる共有のライブラリを build/lib/ に集める（tools/mkimg.sh と、実機に送るときに使う）。
#   /usr/lib/bearssl   BearSSL（third_party/bearssl。無改造）のうち lib/bearssl/files.txt のソースと、ヘッダー、
#                  LICENSE.txt、Monocs 用の compile_flags.txt（lib/bearssl/）
#   /usr/lib/tls       TLS でつなぐ薄い層（lib/tls/。monocs_tls.h）
#   /usr/lib/inflate   gzip・zlib の展開（lib/inflate/。monocs_inflate.h。自作）
#   /usr/lib/gpio      GPIO（lib/gpio/。monocs_gpio.h。自作）
#   /usr/lib/i2c       I2C のマスター（lib/i2c/。monocs_i2c.h。自作。/lib/gpio を使う）
#   /usr/lib/spi       SPI のマスター（lib/spi/。monocs_spi.h。自作。/lib/gpio を使う）
#   /usr/lib/softuart  ソフトウェアの UART（lib/softuart/。monocs_softuart.h。自作。/lib/gpio を使う）
#   /usr/lib/pwm       ハードウェアの PWM（lib/pwm/。monocs_pwm.h。自作。/lib/gpio を使う）
# 実機に送る:  sh tools/tls/mklib.sh && python3 tools/uart/nos.py pushdir build/lib /lib
set -eu
cd "$(dirname "$0")/../.."
OUT=build/lib
B=third_party/bearssl
rm -rf "$OUT"
mkdir -p "$OUT/bearssl/inc" "$OUT/bearssl/src" "$OUT/tls" "$OUT/inflate" "$OUT/gpio" "$OUT/i2c" "$OUT/spi" "$OUT/softuart" "$OUT/pwm"
cp "$B"/inc/*.h "$OUT/bearssl/inc/"
cp "$B"/src/*.h "$OUT/bearssl/src/"
cp "$B/LICENSE.txt" lib/bearssl/compile_flags.txt "$OUT/bearssl/"
grep -v '^#' lib/bearssl/files.txt | while read -r f; do
    mkdir -p "$OUT/bearssl/$(dirname "$f")"
    cp "$B/$f" "$OUT/bearssl/$f"
done
cp lib/tls/* "$OUT/tls/"
cp lib/inflate/* "$OUT/inflate/"
cp lib/gpio/* "$OUT/gpio/"
cp lib/i2c/* "$OUT/i2c/"
cp lib/spi/* "$OUT/spi/"
cp lib/softuart/* "$OUT/softuart/"
cp lib/pwm/* "$OUT/pwm/"
echo "mklib: $OUT（$(find "$OUT" -type f | wc -l) ファイル、$(du -sk "$OUT" | cut -f1) KB）"
