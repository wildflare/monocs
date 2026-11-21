#!/bin/sh
# 中のコンパイラの識別（コンパイルのキャッシュの鍵。os/slimcc/cache.c）を build/gen/compiler_id.h に書く
#   tools/compiler_id.sh 出力 ファイル...
# コンパイラの出力に効くもの（cc/ のソース、Monocs 用の設定 cc/platform/monocs.c、キャッシュの形とオプションの
# 決め方 os/slimcc/cache.c・run.c）の名前と中身の SHA-256 の先頭 16 桁。中身が同じなら書き直さない
# （書き直すと cache.c がコンパイルし直しになるため）
set -eu
OUT=$1
shift
ID=$(for f in "$@"; do echo "$f"; done | LC_ALL=C sort | while IFS= read -r f; do
    printf '%s\n' "$f"; cat "$f"
done | sha256sum | cut -c1-16)
mkdir -p "$(dirname "$OUT")"
NEW="// 生成物（tools/compiler_id.sh）。中のコンパイラの識別
#define SLIMCC_COMPILER_ID 0x${ID}ULL"
if [ ! -f "$OUT" ] || [ "$(cat "$OUT")" != "$NEW" ]; then
    printf '%s\n' "$NEW" > "$OUT"
fi
