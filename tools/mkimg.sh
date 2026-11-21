#!/bin/sh
# build/sdcard/ の中身を入れた SD カードイメージ build/sdcard.img を作る。
# QEMU の SD カードとして使うほか、実機用に Rufus などでそのまま書き込める。
#
# 使い方: tools/mkimg.sh [AUTORUN.c]
#   AUTORUN.c を指定すると、起動時に自動で実行される /autorun.c として入れる
#
# 構成: 64MB、MBR、1 MiB 目から FAT32 のパーティション 1 つ
# （QEMU の SD カードは、容量が 2 のべき乗である必要がある）
set -eu

IMG=build/sdcard.img
SIZE_MB=64
PART_START=2048     # セクタ（1 MiB）

if [ ! -d build/sdcard ]; then
    echo "mkimg: build/sdcard/ がありません。先に make sdcard を実行してください" >&2
    exit 1
fi

rm -f "$IMG"
truncate -s ${SIZE_MB}M "$IMG"
echo "start=$PART_START, type=c" | /usr/sbin/sfdisk --quiet "$IMG"
/usr/sbin/mkfs.vfat -F 32 -n MONOCS --offset $PART_START "$IMG" \
    $(( (SIZE_MB * 1024 * 1024 / 512 - PART_START) / 2 )) >/dev/null

PART="$IMG@@$((PART_START * 512))"
mcopy -s -i "$PART" build/sdcard/* ::/
# ファームウェアがルートから読むものは隠す（ls に出さない。-a で見える）。隠し属性でも起動することは実機で確かめた。
# config.txt は人が直すことがあるので見せておく
mattrib -i "$PART" +h ::/bootcode.bin ::/start.elf ::/fixup.dat
# フォルダの構成（2026-11-19。Linux の FHS をもとに、ソースのまま動かす Monocs に合わせた）:
#   /usr は Monocs に付いてくるもの（更新で入れ替わる）: /usr/bin コマンド・/usr/sbin サービス（どちらもソース）、
#   /usr/lib ライブラリ（-l名前）・firmware・slimcc のヘッダー、/usr/include ヘッダー、/usr/share/samples サンプル、
#   /usr/src/kernel カーネルのソース（セルフホスト）。/root はホーム（自分のもの）: /root/apps 自分のアプリ（ソース。
#   PATH の先頭）。/etc 設定、/tmp 作業用、/var ログ・キャッシュ
mmd -i "$PART" ::/root ::/root/apps ::/tmp ::/var ::/usr/bin ::/usr/sbin ::/usr/src   # /usr/share は build/sdcard（ライセンス）から

# コンパイル時に読むヘッダー
#   /usr/lib/slimcc/include  slimcc-a64 付属（stdarg.h、stddef.h など。GCC の /usr/lib/gcc/…/include にあたる）
#   /usr/include             picolibc と Monocs（monocs.h、curses.h、glob.h（picolibc の BSD 版を Linux と同じ形のものに置き換える）。ncurses.h・ncursesw/curses.h は curses.h を読むだけ）
mmd -i "$PART" ::/usr/lib/slimcc
mcopy -s -i "$PART" cc/slimcc_headers/include ::/usr/lib/slimcc/
mcopy -s -i "$PART" build/picolibc-install/include ::/usr/
mcopy -o -i "$PART" os/api/include/monocs.h os/api/include/monocs_auth.h os/api/include/monocs_sysstat.h os/api/include/curses.h os/api/include/ncurses.h os/api/include/glob.h ::/usr/include/
mmd -i "$PART" ::/usr/include/ncursesw 2>/dev/null || true
mcopy -o -i "$PART" os/api/include/ncursesw/curses.h os/api/include/ncursesw/ncurses.h ::/usr/include/ncursesw/
# ソケット・poll（picolibc にないもの、使えないもの（poll.h・arpa/inet.h）を Linux と同じ形のもので置き換える）
mmd -i "$PART" ::/usr/include/netinet 2>/dev/null || true
mcopy -o -i "$PART" os/api/include/poll.h os/api/include/netdb.h os/api/include/pthread.h os/api/include/omp.h os/api/include/dlfcn.h os/api/include/termcap.h os/api/include/utime.h ::/usr/include/
mcopy -o -i "$PART" os/api/include/sys/socket.h os/api/include/sys/poll.h os/api/include/sys/ioctl.h os/api/include/sys/sysinfo.h os/api/include/sys/resource.h os/api/include/sys/termios.h ::/usr/include/sys/
mcopy -o -i "$PART" os/api/include/netinet/in.h os/api/include/netinet/tcp.h ::/usr/include/netinet/
mcopy -o -i "$PART" os/api/include/arpa/inet.h ::/usr/include/arpa/

# サンプルプログラム
mcopy -s -i "$PART" samples ::/usr/share/

# 付いてくるコマンド（/usr/bin）とサービス（/usr/sbin）は、ソースのまま置く（名前で打てばその場でコンパイルして
# 動く。2 回目からはキャッシュ。2026-11-19 にソースのままに戻した）。フォルダのもの（curl、sshd など）はフォルダごと
mcopy -i "$PART" bin/*.c ::/usr/bin/
for d in bin/*/; do
    mcopy -s -i "$PART" "${d%/}" ::/usr/bin/
done
mcopy -i "$PART" services/*.c ::/usr/sbin/
for d in services/*/; do
    mcopy -s -i "$PART" "${d%/}" ::/usr/sbin/
done
# 共有のライブラリ: TLS（/usr/lib/tls）と、その中身の BearSSL（/usr/lib/bearssl。tools/tls/mklib.sh が集める）
sh tools/tls/mklib.sh > /dev/null
mcopy -s -i "$PART" build/lib/bearssl build/lib/tls build/lib/inflate build/lib/gpio build/lib/i2c build/lib/spi build/lib/softuart build/lib/pwm ::/usr/lib/
# TLS が相手を確かめるための認証局の証明書（Linux と同じ場所。このイメージを作る機械のものを写す。
# Debian の ca-certificates（Mozilla の一覧）。なければ入れない: https は使えない）
mmd -i "$PART" ::/etc 2>/dev/null || true
if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
    mmd -i "$PART" ::/etc/ssl ::/etc/ssl/certs
    mcopy -i "$PART" /etc/ssl/certs/ca-certificates.crt ::/etc/ssl/certs/
fi

# サービス（裏で動くプログラム。os/kernel/svc.c）と、起動時に始めるものの一覧
mcopy -i "$PART" services/services.txt ::/etc/services.txt

# 起動時に動かすプログラム（実機の確かめ用）
if [ $# -ge 1 ]; then
    mcopy -i "$PART" "$1" ::/autorun.c
    echo "mkimg: $1 を /autorun.c として入れました"
fi

echo "mkimg: $IMG を作りました"
mdir -i "$PART" ::/
