#!/bin/sh
# シェルの書き方（展開・if・for・case・関数・ヒアドキュメント・sh FILE など）を bash と比べ、timeout・dmesg・止められたときの終了の値を確かめる（QEMU）。
#   sh tools/test/shell.sh [KERNEL]   （既定は標準の slimcc 版 build/kslim/kernel8.img。GCC 版は build/kernel8.img。先に make sdimage）
# 1. tools/test/shell/lines.sh の各行を、bash と Monocs で流して、o.txt に書かれたものを比べる
# 2. timeout（終わらないアプリ、system() の中の timeout、外の timeout が入れ子のアプリをまとめて止める）、
#    例外・Ctrl-C で止まったアプリの $?（139・130）、timeout のシグナル（TERM を関数で受ける・無視して -k・
#    -s KILL・--preserve-status）、dmesg（起動の表示とサービスの記録）
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
K=${1:-$R/build/kslim/kernel8.img}
D=$R/tools/test/shell
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -O1 -o "$T/args" "$D/args.c"
cp "$D/script.sh" "$T/"
printf '#!/bin/sh\nexec bash "$@"\n' > "$T/sh" && chmod +x "$T/sh"     # lines.sh の sh は bash で（Monocs の sh と比べる）
(cd "$T" && HOME=/root PATH="$T:$PATH" bash "$D/lines.sh" > /dev/null 2>&1; mv o.txt linux.txt)

cp "$R/build/sdcard.img" "$T/sd.img"
P="$T/sd.img@@1048576"
mmd -i "$P" ::/root/test 2>/dev/null || true
mcopy -i "$P" "$D/spin.c" "$D/nest.c" "$D/exit7.c" "$D/crash.c" "$D/args.c" "$D/term.c" "$D/ignore.c" "$D/script.sh" ::/root/test/
# 1 行ずつ打つ（行の終わりの \r）。bash と同じ HOME（Monocs の既定は /root）
keys=""
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    keys="$keys$(printf '%s' "$line" | sed 's/\\/\\\\/g')\\r"
done < "$D/lines.sh"
python3 "$R/tools/test/qterm.py" "$K" "$T/sd.img" 40 160 'until:/> :120' \
    'keys:cd /root/test; for p in spin nest exit7 crash args term ignore; do cc -o $p $p.c; done; echo done""-cc\r' 'until:done-cc:300' \
    "keys:$keys" 'keys:echo done""-lines\r' 'until:done-lines:600' \
    'keys:timeout 1 ./spin; echo spin $? > t.txt\r' 'until:/root/test> :120' \
    'keys:timeout 30 ./exit7; echo quick $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout 0 ./exit7; echo zero $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:./nest inner >> t.txt; echo nest-inner $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout 2 ./nest outer >> t.txt; echo nest-outer $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout 1x ./spin; echo bad $? >> t.txt\r' 'until:/root/test> :60' \
    'keys:./crash; echo crash $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:./spin\r' 'wait:3' 'keys:\x03' 'until:/root/test> :60' 'keys:echo ctrl-c $? >> t.txt\r' 'until:/root/test> :60' \
    'keys:timeout 2 ./term >> t.txt; echo term $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout --preserve-status 2 ./term > /dev/null; echo preserve $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout -s KILL 2 ./term >> t.txt; echo kill $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout -k 1 1 ./ignore >> t.txt; echo kill-after $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout -s INT 1 ./spin; echo int $? >> t.txt\r' 'until:/root/test> :120' \
    'keys:timeout -s NOPE 1 ./spin; echo badsig $? >> t.txt\r' 'until:/root/test> :60' \
    'keys:while true; do true; done\r' 'wait:2' 'keys:\x03' 'until:/root/test> :30' 'keys:echo loop-stop $? >> t.txt\r' 'until:/root/test> :60' \
    'keys:echo bg &\r' 'until:/root/test> :30' 'keys:echo amp $? >> t.txt\r' 'until:/root/test> :60' \
    'keys:dmesg > d.txt; echo done""-t\r' 'until:done-t:120' > "$T/qemu.log" 2>&1 || true
for f in o t d; do mcopy -n -i "$P" ::/root/test/$f.txt "$T/$f.txt" 2>/dev/null || echo "($f.txt がない)"; done
fail=0
if diff "$T/linux.txt" "$T/o.txt" > "$T/diff"; then
    echo "ok: $(wc -l < "$T/linux.txt") 行が bash と同じ"
else
    echo "DIFF (< bash, > Monocs)"; cat "$T/diff"; fail=1
fi
cat > "$T/want.txt" <<'E'
spin 124
quick 7
zero 7
inner status 124, parent goes on
nest-inner 0
nest-outer 124
bad 125
crash 139
ctrl-c 130
got TERM
term 124
preserve 3
kill 124
kill-after 124
int 124
badsig 125
loop-stop 130
amp 2
E
if diff "$T/want.txt" "$T/t.txt" > "$T/diff"; then echo "ok: timeout と止められたときの値"; else echo "DIFF timeout (< want, > Monocs)"; cat "$T/diff"; fail=1; fi
# 外の timeout: 子（spin）と親（nest.c）の両方が止まり、親の「after」は出ない
if grep -qx "after" "$T/t.txt"; then echo "DIFF: 外の timeout で親が止まらなかった"; fail=1; fi
for w in "Monocs" "svc: started /usr/sbin/wifid.c" "app ./crash: stopped by exception"; do
    if grep -q "$w" "$T/d.txt"; then echo "ok: dmesg に「$w」"; else echo "DIFF: dmesg に「$w」がない"; fail=1; fi
done
if grep -qE '^\[ *[0-9]+\.[0-9]{6}\] ' "$T/d.txt"; then echo "ok: dmesg の時刻"; else echo "DIFF: dmesg の時刻の形"; fail=1; fi
grep -a "invalid pointer" "$T/qemu.log" && fail=1
[ -n "${KEEP:-}" ] && cp -r "$T" "$KEEP"
exit $fail
