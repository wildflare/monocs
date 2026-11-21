#!/bin/sh
# slimld と GNU ld の結果を比べる（リポジトリの最上位で実行する。make ld-test から呼ばれる）
#   sh cc/test-a64/ld_test.sh 作業ディレクトリ 名前 入力...
# GNU ld には、SHF_MERGE を消した入力のコピー（nomerge.py）を渡す（slimld は統合しないため）
set -eu
TMP=$1
NAME=$2
shift 2
mkdir -p "$TMP/$NAME"
nm_inputs=""
i=0
for f in "$@"; do
    i=$((i + 1))
    out="$TMP/$NAME/$i-$(basename "$f")"
    python3 cc/test-a64/nomerge.py "$f" "$out"
    nm_inputs="$nm_inputs $out"
done
objs=$(for f in $nm_inputs; do case $f in *.o) echo "$f";; esac; done)
libs=$(for f in $nm_inputs; do case $f in *.a) echo "$f";; esac; done)
ld -nostdlib -T os/boot/linker.ld --no-warn-rwx-segments -o "$TMP/$NAME/gnu.elf" \
    $objs --start-group $libs --end-group
objcopy -O binary "$TMP/$NAME/gnu.elf" "$TMP/$NAME/gnu.img"
cc/slimld -o "$TMP/$NAME/slimld.img" "$@"
printf '%s: ' "$NAME"
python3 cc/test-a64/ld_compare.py "$TMP/$NAME/gnu.elf" "$TMP/$NAME/gnu.img" "$TMP/$NAME/slimld.img"
