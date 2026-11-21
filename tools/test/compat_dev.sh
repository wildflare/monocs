#!/bin/sh
# 実機で、Linux との互換の試験をまとめて流す（hwcheck.sh がカーネルごとに呼ぶ。単独でも動く）。
#   sh tools/test/compat_dev.sh [WEB_HOST]   （既定 example.com。Wi-Fi がつながっていなければ、その行は失敗と出る）
# 1. linuxapi.c（getopt・環境変数・文字列・正規表現・glob・ワイド文字・%ls・シグナル・popen）と linuxapi_nest.c
#    （system() の子の変更が親に残らない）: Linux（gcc・glibc、LANG=C.UTF-8）の出力と比べる
# 2. netapi.sh（ソケット・getaddrinfo・select・poll・alarm のうち、相手のいらないもの）
# 3. sockets/run.py（Pi 5 を相手に TCP・UDP・select のサーバー・DNS と HTTP）
# 4. SIGWINCH（telnet でつないで画面の大きさを変える。device/winch.c）
# 5. pthread（tools/test/pthread/pthtest.c）
# 6. OpenMP（tools/test/openmp/omptest.c）
# 7. 同時に開ける fd（tools/test/fds/fdtest.c。ファイルとソケットで番号を分け合う）
# Wi-Fi だけ（NOS_HOST）でも動く（結果は nos.py cmd で読む）。4 は USB の telnet が要るので、Wi-Fi のときは飛ばす
# 失敗があれば 1 で終わる
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
N="python3 $R/tools/uart/nos.py"
WEB=${1:-example.com}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ARGS='-a -b x --long=y --flag file1 -- -file2'

# 1. linuxapi
gcc -std=c2x -O1 -o "$T/linuxapi" "$R/tools/test/linuxapi.c"
gcc -std=c2x -O1 -o "$T/linuxapi_nest" "$R/tools/test/linuxapi_nest.c"
(cd "$T" && export LANG=C.UTF-8 && unset LC_ALL LC_CTYPE && ./linuxapi $ARGS > linux.txt &&
    ./linuxapi_nest -p 1 ./linuxapi | tail -2 > linux_nest.txt)
$N push "$R/tools/test/linuxapi.c" /tmp/linuxapi.c > /dev/null || { echo "push failed"; fail=1; }
$N push "$R/tools/test/linuxapi_nest.c" /tmp/linuxapi_nest.c > /dev/null || { echo "push failed"; fail=1; }
$N --timeout 600 cmd "cd /tmp; cc -o linuxapi linuxapi.c && ./linuxapi $ARGS > la_out.txt; cc -o linuxapi_nest linuxapi_nest.c && ./linuxapi_nest -p 1 \"./linuxapi\" | tail -2 > la_nest.txt; cd /" > /dev/null 2>&1
$N cmd 'cat /tmp/la_out.txt' > "$T/out.txt"
$N cmd 'cat /tmp/la_nest.txt' > "$T/nest.txt"
if diff "$T/linux.txt" "$T/out.txt" > "$T/d"; then echo "linuxapi: ok ($(wc -l < "$T/out.txt") 行が Linux と同じ)"
else echo "linuxapi: DIFF (< Linux, > Monocs)"; cat "$T/d"; fail=1; fi
if diff "$T/linux_nest.txt" "$T/nest.txt" > "$T/d"; then echo "linuxapi_nest: ok"
else echo "linuxapi_nest: DIFF"; cat "$T/d"; fail=1; fi

# 2. netapi
sh "$R/tools/test/netapi.sh" > "$T/netapi.txt" 2>&1 || fail=1
sed 's/^/netapi: /' "$T/netapi.txt" | head -20

# 3. sockets（期待する行がそろっているか）
python3 "$R/tools/test/sockets/run.py" "$WEB" > "$T/sock.txt" 2>&1
for want in 'tcp echo 1MB: match' 'tcp peek: match' 'tcp after shutdown: recv 0' 'nonblock connect: poll 1 out 1 so_error 0' \
    'closed port: refused' 'udp echo: match from port 1' 'udp timeout: -1 EAGAIN ~2s 1' \
    '3 clients echo 300KB each: match' 'server done: served 4, max concurrent 3'; do
    if ! grep -q "$want" "$T/sock.txt"; then echo "sockets: NG ($want)"; fail=1; fi
done
grep -q "^http $WEB: 200" "$T/sock.txt" && echo "sockets: http $WEB 200" || echo "sockets: http $WEB に届かない（Wi-Fi・DNS。失敗には数えない）"
grep -q 'tcp echo 1MB: match' "$T/sock.txt" && echo "sockets: tcp echo 1MB $(grep 'tcp echo' "$T/sock.txt" | grep -o '[0-9.]* s')"

# 4. SIGWINCH
if [ -n "${NOS_HOST:-}" ]; then echo "sigwinch: skip（USB の telnet が要る）"; else
$N push "$R/tools/test/device/winch.c" /tmp/winch.c > /dev/null || { echo "push failed"; fail=1; }
python3 "$R/tools/test/tnterm.py" 24 80 'wait:1' 'keys:\r' 'until:> :5' 'keys:cc -o /tmp/winch /tmp/winch.c && /tmp/winch\r' 'until:winch ready:120' 'naws:30:90' \
    'until:winch got:10' 'wait:1' 'screen:winch' 'keys:exit\r' 'wait:1' > "$T/winch.txt" 2>&1
if grep -q 'winch got 1 size 30x90' "$T/winch.txt"; then echo "sigwinch: ok"
else echo "sigwinch: NG"; grep -a 'winch' "$T/winch.txt" | tail -3; fail=1; fi
fi
# 5. pthread（tools/test/pthread/pthtest.c。Linux と同じ出力、スレッドの exit・例外がアプリ全体を終わらせる、EAGAIN）
gcc -std=gnu2x -O1 -pthread -o "$T/pthtest" "$R/tools/test/pthread/pthtest.c"
"$T/pthtest" > "$T/pth_linux.txt"
$N push "$R/tools/test/pthread/pthtest.c" /tmp/pthtest.c > /dev/null || { echo "push failed"; fail=1; }
$N --timeout 300 cmd 'cd /tmp; cc -o pthtest pthtest.c && ./pthtest > pth_out.txt; cc -o pthtest pthtest.c && ./pthtest exit; echo exit $? > pth_s.txt; cc -o pthtest pthtest.c && ./pthtest crash; echo crash $? >> pth_s.txt; cc -o pthtest pthtest.c && ./pthtest many >> pth_s.txt; cd /' > /dev/null 2>&1
$N cmd 'cat /tmp/pth_out.txt' > "$T/pth_out.txt"
$N cmd 'cat /tmp/pth_s.txt' > "$T/pth_s.txt"
printf 'exit 5\ncrash 139\nmade 2, then EAGAIN\nagain 0\n' > "$T/pth_want.txt"
if diff "$T/pth_linux.txt" "$T/pth_out.txt" > "$T/d" && diff "$T/pth_want.txt" "$T/pth_s.txt" >> "$T/d"; then
    echo "pthread: ok ($(wc -l < "$T/pth_out.txt") 行が Linux と同じ、exit・例外・EAGAIN)"
else echo "pthread: DIFF"; cat "$T/d"; fail=1; fi
# 6. OpenMP（tools/test/openmp/omptest.c。GCC の -fopenmp と同じ出力）
gcc -O1 -fopenmp -o "$T/omptest" "$R/tools/test/openmp/omptest.c"
(unset OMP_NUM_THREADS; "$T/omptest" > "$T/omp_gcc.txt")
$N push "$R/tools/test/openmp/omptest.c" /tmp/omptest.c > /dev/null || { echo "push failed"; fail=1; }
$N --timeout 300 cmd 'cd /tmp; cc -o omptest -fopenmp omptest.c && ./omptest > omp_out.txt; cd /' > /dev/null 2>&1
$N cmd 'cat /tmp/omp_out.txt' > "$T/omp_out.txt"
if diff "$T/omp_gcc.txt" "$T/omp_out.txt" > "$T/d"; then echo "openmp: ok ($(wc -l < "$T/omp_out.txt") 行が GCC の -fopenmp と同じ)"
else echo "openmp: DIFF (< GCC, > Monocs)"; cat "$T/d"; fail=1; fi
# 7. fd（Linux で ulimit -n を 256 にしたときと同じ出力）
gcc -O1 -o "$T/fdtest" "$R/tools/test/fds/fdtest.c"
"$T/fdtest" > "$T/fd_linux.txt"
$N push "$R/tools/test/fds/fdtest.c" /tmp/fdtest.c > /dev/null || { echo "push failed"; fail=1; }
# 出力はリダイレクトせずに受け取る（Monocs のリダイレクトは向け先のファイルに fd を 1 つ使い、番号がずれる）
$N --timeout 300 cmd 'cc -o /tmp/fdtest /tmp/fdtest.c && /tmp/fdtest' > "$T/fd_out.txt" 2>/dev/null
if diff "$T/fd_linux.txt" "$T/fd_out.txt" > "$T/d"; then echo "fds: ok ($(head -1 "$T/fd_out.txt"))"
else echo "fds: DIFF (< Linux, > Monocs)"; cat "$T/d"; fail=1; fi
[ $fail -eq 0 ] && echo "compat: すべて ok" || echo "compat: 失敗あり"
exit $fail
