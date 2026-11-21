#!/bin/sh
# 実機（Zero 2 W）の確認。Pi 5 に USB（内側の端子）と UART をつないだ状態で、リポジトリの一番上で:
#   sh tools/test/hwcheck.sh [KERNEL...]     （既定は build/kernel8.img（GCC 版）と build/kslim/kernel8.img（標準）の順）
# カーネルを送って再起動し、シェル・telnet・HTTP・USB イーサネット・アプリの TCP・4 コア・セルフホストの
# ビルド・mi（/apps/mi があれば）・Linux との互換（compat_dev.sh: linuxapi・netapi・ソケット）を確かめる。
# 最後に送ったカーネルが実機に残る（標準の slimcc 版が普段の状態。2026-11-16 から）
set -u
cd "$(dirname "$0")/../.."
N="python3 tools/uart/nos.py"
[ $# -eq 0 ] && set -- build/kernel8.img build/kslim/kernel8.img
for k in "$@"; do
    echo "== $k"
    $N kernel "$k" 2>&1 | tail -1
    $N cmd 'mkdir /tmp' > /dev/null 2>&1
    for f in tools/test/device/*.c; do $N push "$f" "/tmp/$(basename "$f")" > /dev/null; done
    $N cmd 'cd /tmp; for f in *.c; do cc -o ${f%.c} $f; done; cd /' > /dev/null 2>&1
    $N cmd uptime 2>/dev/null | head -1
    $N run samples/hello.c 2>&1 | grep Hello
    $N run samples/libc_mc.c 2>&1 | tail -2 | head -1
    python3 tools/test/telnet_check.py 2>&1 | tail -4
    curl -s -o /dev/null -w 'http GET %{http_code}\n' http://192.168.7.2/samples/hello.c
    python3 tools/test/echotest.py 5
    python3 -I tools/test/appecho.py
    # セルフホストのビルド（/usr/src/kernel。古いときは nos.py pushdir build/selfsrc /usr/src/kernel）。失敗したらエラーの行を出す
    $N --timeout 900 cmd 'cd /usr/src/kernel' > /dev/null
    b=$($N --timeout 900 cmd 'cc -o build-kernel build.c && ./build-kernel' 2>&1)
    echo "$b" | grep 'done (' || { echo "self build: FAILED"; echo "$b" | grep -v '^\[' | tail -5; }
    $N cmd 'cd /' > /dev/null
    if $N cmd 'ls /apps' 2>/dev/null | grep -q '^mi$'; then
        python3 -I tools/test/hwterm.py 30 100 'keys:\r' 'until:/> :5' 'keys: mi /tmp/mi_test.txt\r' \
            'until:ESC=Menu:120' 'keys:\x11' 'wait:1' 2>&1 | grep 'ESC=Menu'
    fi
    # nos.py cmd の終了の値（シェルの $?）、timeout、カーネルの記録（nos.py log = GET /.log）。2026-11-15
    $N push tools/test/shell/spin.c /tmp/spin.c > /dev/null
    $N cmd 'cc -o /tmp/spin /tmp/spin.c' > /dev/null 2>&1
    $N cmd 'false' > /dev/null 2>&1; r1=$?
    $N cmd 'timeout 1 /tmp/spin' > /dev/null 2>&1; r2=$?
    lg=$($N log 2>/dev/null | grep -c 'Monocs: hello\|svc: started /usr/sbin/httpd.c')
    [ $r1 -eq 1 ] && [ $r2 -eq 124 ] && [ "$lg" -ge 2 ] && echo "shell status, timeout, log: ok" ||
        echo "shell status, timeout, log: NG (false $r1, timeout $r2, log $lg)"
    # USB シリアル: 大量に出力している途中で Pi 5 が読むのをやめても、後で戻る（2026-11-14 に直した不具合。
    # 出力が終わるのを UART で待つ）
    timeout 5 $N --timeout 2 cmd '/tmp/flood' > /dev/null 2>&1
    for i in $(seq 1 12); do
        $N --uart --force --timeout 20 cmd 'echo flood-done' 2>/dev/null | grep -q '^flood-done' && break
    done
    $N cmd 'echo usb-alive' 2>/dev/null | grep -q '^usb-alive' && echo "usb after stalled reader: ok" || echo "usb after stalled reader: NG"
    sh tools/test/compat_dev.sh 2>&1 | grep -v '^netapi: ok' | sed 's/^/  /'
done
