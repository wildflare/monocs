#!/bin/sh
# lib/bearssl/files.txt（/lib/bearssl に入れる BearSSL のソース。TLS のクライアントと SSH のサーバーが使うもの）を作り直す。
# BearSSL の全ファイルと /lib/tls・curl を Linux 用の slimcc でコンパイルし、GNU ld が書庫から取り出したもの
# （= 実際に使われるファイル）をマップから拾う。/lib/tls が使う BearSSL の関数を変えたら流す。
set -eu
cd "$(dirname "$0")/../.."
R=$(pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
B=third_party/bearssl
(cd $B && find src -name '*.c' | sort) > "$T/all.txt"
while read -r f; do
    "$R/cc/slimcc" -c -I$B/inc -I$B/src -o "$T/$(echo "$f" | tr / _).o" "$B/$f"
done < "$T/all.txt"
ar rcs "$T/lib.a" "$T"/src_*.o
for f in bin/curl/curl.c lib/tls/tls.c lib/inflate/inflate.c tools/tls/linux_shim.c; do
    "$R/cc/slimcc" -std=gnu23 -c -Ios/api/include -Ilib/tls -Ilib/inflate -I$B -o "$T/app_$(basename "$f").o" "$f"
done
gcc -o "$T/curl" "$T"/app_*.o "$T/lib.a" -Wl,-Map="$T/map"
# SSH のサーバー（services/sshd）が使うものも（鍵交換・署名・AES-CTR など）
"$R/cc/slimcc" -std=gnu23 -c -Ios/api/include -I$B -o "$T/sshd.o" services/sshd/sshd.c
"$R/cc/slimcc" -std=gnu23 -c -o "$T/sshd_main.o" tools/ssh/linux_main.c
gcc -o "$T/sshd" "$T/sshd.o" "$T/sshd_main.o" "$T/lib.a" -lutil -Wl,-Map="$T/map2"
cat "$T/map" "$T/map2" | grep -o "^$T/lib.a(src_[a-z0-9_]*\.c\.o)" | sed 's/.*(//; s/\.o)//' | sort -u > "$T/used.txt"
{
    echo "# /lib/bearssl に入れる BearSSL のソース（third_party/bearssl からの位置）。TLS のクライアント（/lib/tls の"
    echo "# br_ssl_client_init_full と信頼の起点の読み込み）と SSH のサーバー（services/sshd）が使うものだけ。"
    echo "# 作り直すのは sh tools/tls/files.sh"
    while read -r f; do
        grep -qx "$(echo "$f" | tr / _)" "$T/used.txt" && echo "$f"
    done < "$T/all.txt"
} > lib/bearssl/files.txt
echo "files.sh: $(grep -vc '^#' lib/bearssl/files.txt) ファイル"
# 試すとき: cp "$T/curl" どこか; ./curl https://example.com/
[ -n "${KEEP_CURL:-}" ] && cp "$T/curl" "$KEEP_CURL"
exit 0
