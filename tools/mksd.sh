#!/bin/sh
# 実機（Zero 2 W）用の SD カードの中身を build/sdcard/ に集める。
# ファームウェアは Pi 5 の Raspberry Pi OS が持つもの（全機種共通）を使う。
#
# 使い方: tools/mksd.sh [ファームウェアのディレクトリ（既定 /boot/firmware）]
set -eu

FW=${1:-/boot/firmware}
OUT=build/sdcard

for f in bootcode.bin start.elf fixup.dat bcm2710-rpi-zero-2-w.dtb overlays/disable-bt.dtbo; do
    if [ ! -f "$FW/$f" ]; then
        echo "mksd: $FW/$f がありません" >&2
        exit 1
    fi
done
# 入れるカーネルは標準の slimcc 版（2026-11-16 から。KERNEL=build/kernel8.img で GCC 版）
KERNEL=${KERNEL:-build/kslim/kernel8.img}
if [ ! -f "$KERNEL" ]; then
    echo "mksd: $KERNEL がありません。先に make を実行してください" >&2
    exit 1
fi

rm -rf "$OUT"
# ルートに置くのは、ファームウェアがルートから読むものだけ（bootcode.bin・start.elf・fixup.dat・config.txt）。
# カーネル・dtb・overlays は boot/（config.txt の os_prefix=boot/。boot/overlays/README があると、overlays も
# boot/overlays/ から読む）
mkdir -p "$OUT/boot/overlays"
cp "$FW/bootcode.bin" "$FW/start.elf" "$FW/fixup.dat" os/boot/config.txt "$OUT/"
cp "$FW/bcm2710-rpi-zero-2-w.dtb" "$FW/LICENCE.broadcom" "$OUT/boot/"
cp "$KERNEL" "$OUT/boot/kernel8.img"
cp "$FW/overlays/disable-bt.dtbo" "$OUT/boot/overlays/"
printf 'Device Tree overlays for Monocs (config.txt: os_prefix=boot/). The firmware reads overlays from here\nonly when this README exists.\n' > "$OUT/boot/overlays/README"
# カーネルの関数名（シェルの perf record。tools/perf/mksym.py）
SYM=${KERNEL%.img}.sym
[ -f "$SYM" ] && cp "$SYM" "$OUT/boot/kernel8.sym"
# 内蔵 Wi-Fi のファームウェア（third_party/firmware/README.md）。/usr/lib/firmware/brcm（Linux の /lib/firmware と同じもの）
mkdir -p "$OUT/usr/lib/firmware/brcm"
cp third_party/firmware/brcm/* "$OUT/usr/lib/firmware/brcm/"

# ライセンス（THIRD_PARTY.md）。Monocs 自身と、カーネル・SD カードに入る他のプロジェクトのもの
L="$OUT/usr/share/licenses"
mkdir -p "$L/monocs" "$L/slimcc" "$L/picolibc" "$L/fatfs" "$L/lwip" "$L/bearssl" "$L/firmware-brcm"
cp LICENSE THIRD_PARTY.md "$L/monocs/"
cp cc/LICENSE "$L/slimcc/LICENSE"
cp third_party/picolibc/COPYING.picolibc "$L/picolibc/"
cp third_party/fatfs/LICENSE.txt "$L/fatfs/"
cp third_party/lwip/COPYING "$L/lwip/"
cp third_party/bearssl/LICENSE.txt "$L/bearssl/"
cp third_party/firmware/brcm/LICENCE.cypress third_party/firmware/brcm/LICENCE.synaptics "$L/firmware-brcm/"

echo "mksd: $OUT/ に用意しました"
find "$OUT" -type f | sort
