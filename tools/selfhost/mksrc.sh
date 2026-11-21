#!/bin/sh
# セルフホスト用に、SD カードの /usr/src/kernel に入れるものを build/selfsrc/ に集める（make sdimage-self から呼ばれる）
#   tools/selfhost/mksrc.sh 出力ディレクトリ libc.a オブジェクト...
# オブジェクト（build/kslim/... の .o）の並びから、ビルドスクリプトが使う build_files.h を作る。
# パスはリポジトリと同じ相対パスにする（__FILE__ などが Linux 上のクロスビルドと一致するように）
set -eu
OUT=$1
LIBC=$2
shift 2

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/cc/platform" "$OUT/cc/rt" "$OUT/build/fatfs"
# OS（api.def なども含め、ディレクトリごと）
cp -r os "$OUT/"
# slimcc-a64 と slimld
cp cc/*.c cc/*.h cc/*.inc "$OUT/cc/"
cp cc/platform/monocs.c "$OUT/cc/platform/"
# libgcc の代わり（long double の演算、__clear_cache）
cp cc/rt/softfp128.c cc/rt/clear_cache.c "$OUT/cc/rt/"
# FatFs（パッチを当てたもの）
cp build/fatfs/*.c build/fatfs/*.h "$OUT/build/fatfs/"
# lwIP（ソースとヘッダー。設定 os/net/port は os と一緒に入る）
mkdir -p "$OUT/third_party/lwip/src"
cp -r third_party/lwip/src/core third_party/lwip/src/netif third_party/lwip/src/include "$OUT/third_party/lwip/src/"
# picolibc（slimcc でビルドしたもの。デバッグ情報はないが、念のため削る）
strip -g -o "$OUT/lib/libc.a" "$LIBC"
cp tools/selfhost/build.c "$OUT/build.c"
# 中のコンパイラの識別（os/slimcc/cache.c が ../../build/gen/compiler_id.h で読む）
mkdir -p "$OUT/build/gen"
cp build/gen/compiler_id.h "$OUT/build/gen/"

# ファイルの一覧
for o in "$@"; do
    rel=${o#build/kslim/}
    case $rel in
    slimcc/*)
        src=cc/${rel#slimcc/}
        echo "{\"${src%.o}.c\", \"$o\", KIND_SLIMCC},"
        ;;
    rt/*)
        echo "{\"cc/${rel%.o}.c\", \"$o\", KIND_RT},"
        ;;
    fatfs/*)
        echo "{\"build/fatfs/$(basename "${rel%.o}").c\", \"$o\", KIND_FATFS},"
        ;;
    lwip/*)
        src=third_party/lwip/src/${rel#lwip/}
        echo "{\"${src%.o}.c\", \"$o\", KIND_LWIP},"
        ;;
    *)
        if [ -f "${rel%.o}.S" ]; then
            echo "{\"${rel%.o}.S\", \"$o\", KIND_ASM},"
        else
            echo "{\"${rel%.o}.c\", \"$o\", KIND_OS},"
        fi
        ;;
    esac
done > "$OUT/build_files.h"
echo "mksrc: $OUT/ に用意しました（$(wc -l < "$OUT/build_files.h") ファイル、$(du -sk "$OUT" | cut -f1) KB）"
