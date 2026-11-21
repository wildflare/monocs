#!/bin/sh
# 内蔵アセンブラと GNU as の出力を比べる。cc/ で実行する
#   sh test-a64/asm_compare.sh 作業ディレクトリ file.s...
# セクションの中身、リロケーション、シンボルが一致しなければ差分を表示する
set -u
TMP=$1
shift
same=0
diff=0
for s in "$@"; do
    b=$(basename "$s" .s)
    if ! as -o "$TMP/$b.o" "$s" 2>"$TMP/$b.aserr"; then
        echo "GNU as が失敗: $s"
        diff=$((diff + 1))
        continue
    fi
    python3 test-a64/objdump.py "$TMP/$b.o" > "$TMP/$b.ref"
    if ! ./slimcc -dump-internal-as "$s" > "$TMP/$b.out" 2>"$TMP/$b.err"; then
        echo "内蔵アセンブラが失敗: $s: $(head -1 "$TMP/$b.err")"
        diff=$((diff + 1))
        continue
    fi
    if cmp -s "$TMP/$b.ref" "$TMP/$b.out"; then
        same=$((same + 1))
    else
        echo "不一致: $s"
        diff -u "$TMP/$b.ref" "$TMP/$b.out" | head -20 | sed 's/^/    /'
        diff=$((diff + 1))
    fi
done
echo "一致 $same、不一致 $diff"
[ $diff -eq 0 ]
