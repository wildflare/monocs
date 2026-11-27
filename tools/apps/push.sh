#!/bin/sh
# Pi 5 のプログラムのフォルダ（Linux 用のソースと Makefile のまま）を、実機の /root/apps/名前 に送る（ソースのまま。
# 名前で打てば、その場でコンパイルして動く。2 回目からはキャッシュ）。
#   sh tools/apps/push.sh [-c] [-a] DIR [NAME]
# NAME を省くと、Makefile が作るプログラムの名前（Monocs と同じ解釈器 tools/make/make_eval.c で決める。
# file-dir-manager → fd）。Makefile がなければフォルダの名前。実機では  NAME 引数...  で動く（PATH の /root/apps）。
# 送るもの: .c・.h（サブフォルダも）、Makefile・makefile・GNUmakefile、compile_flags.txt、.runignore。
# 送らないもの: .git などの「.」で始まるフォルダ、build、.o・.d・実行ファイルなど。DIR は読むだけで書き換えない。
# Monocs の run は、Makefile があれば、そこに書かれた .c だけをコンパイルする（テスト用の .c なども送ってよい）。
# Makefile がないときは、フォルダの .c をすべてコンパイルする（除くものは .runignore に書く）。
# 実機が使われているとき（アプリが動いている・telnet・ssh でつながっている）は、何もせずに止まる（nos.py）。
# -c: 送る前に実機の /root/apps/NAME を消す（Pi 5 で消したファイルが実機に残らないように。実機で直したものも消える）
# -a: フォルダの中身を全部送る（2026-11-27。ユーザーの「同じプログラムが動くようにしたい」から。Pi 5 と実機で同じ中身に）。
#     git のリポジトリなら、git が管理しているファイルと、管理していないが .gitignore で除いていないファイル
#     （git ls-files -co --exclude-standard。中身は作業中のまま、コミットしていない変更も。.gitignore・.gitattributes
#     などの git の設定は除く）。git でなければ、
#     .git と build を除いた全部。Makefile があれば、Monocs が作るのは Makefile に書かれた .c だけなので、
#     文書・テスト・Windows 用のファイルなどを送っても作り方は変わらない
set -eu
cd "$(dirname "$0")/../.."
clean=0
all=0
while [ $# -gt 0 ]; do
    case "$1" in
    -c) clean=1; shift ;;
    -a) all=1; shift ;;
    *) break ;;
    esac
done
[ $# -ge 1 ] || { echo "usage: sh tools/apps/push.sh [-c] [-a] DIR [NAME]" >&2; exit 2; }
DIR=$(cd "$1" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
NAME=${2:-}
if [ -z "$NAME" ]; then
    gcc -std=c23 -O1 -o "$T/make_eval" tools/make/make_eval.c 2>/dev/null &&
        NAME=$("$T/make_eval" -info "$DIR" 2>/dev/null | sed -n 's/^target: //p')
    [ -n "$NAME" ] || NAME=$(basename "$DIR")
fi
case "$NAME" in */*|.*|'') echo "push.sh: 名前が正しくない: $NAME" >&2; exit 2 ;; esac
mkdir "$T/app"
list_files() {
    if [ $all -eq 1 ] && git -C "$DIR" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        (cd "$DIR" && git ls-files -co --exclude-standard | grep -v '^\.git')   # .gitignore などの git の設定は送らない
    elif [ $all -eq 1 ]; then
        (cd "$DIR" && find . -mindepth 1 \( -name .git -o -name build \) -prune -o -type f -print)
    else
        (cd "$DIR" && find . -mindepth 1 \( -name '.*' -o -name build \) -prune -o -type f \
            \( -name '*.c' -o -name '*.h' -o -name Makefile -o -name makefile -o -name GNUmakefile \
               -o -name compile_flags.txt \) -print)
    fi
}
list_files | while IFS= read -r f; do
    [ -f "$DIR/$f" ] || continue    # git が管理しているが、作業中に消したもの
    mkdir -p "$T/app/$(dirname "$f")"
    cp "$DIR/$f" "$T/app/$f"
done
[ -f "$DIR/.runignore" ] && cp "$DIR/.runignore" "$T/app/"
n=$(find "$T/app" -type f | wc -l)
[ "$n" -gt 0 ] || { echo "push.sh: $DIR に送るものがない" >&2; exit 1; }
N="python3 tools/uart/nos.py"
# 実機が使われていれば（アプリが動いている・telnet・ssh でつながっている）、nos.py がここで止める
$N cmd 'mkdir -p /root/apps' > /dev/null || { echo "push.sh: 送るのをやめました" >&2; exit 1; }
[ $clean -eq 1 ] && $N cmd "rm -rf /root/apps/$NAME" > /dev/null 2>&1
$N pushdir "$T/app" "/root/apps/$NAME" | tail -1
echo "push.sh: $DIR -> /root/apps/$NAME（$n ファイル）。実機では  $NAME 引数..."
