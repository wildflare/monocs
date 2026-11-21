#!/bin/sh
# services/sshd のテスト（Linux。本物の ssh コマンドでつなぐ）。リポジトリの一番上で:  sh tools/ssh/test.sh
# sshd を Linux 用にコンパイルして（GCC の検査付き。TCP はソケット、コンソールは擬似端末の上の /bin/sh）、
# パスワード・公開鍵（ECDSA・RSA）のログイン、断るべきもの、鍵の交換し直し、大きな出力を確かめる。
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
PORT=$((20000 + $$ % 20000))
B=third_party/bearssl
# linux_main.c は Linux のヘッダーで（Monocs の os/api/include は sys/ioctl.h などを置き換えていて、Linux の struct winsize がない）
gcc -std=gnu23 -O1 -g -fsanitize=address,undefined -Wall -Wextra -c -o "$T/linux_main.o" tools/ssh/linux_main.c 2>&1 | grep -E "linux_main.c|error"
gcc -std=gnu23 -O1 -g -fsanitize=address,undefined -Wall -Wextra -c -o "$T/sftp.o" services/sshd/sftp.c 2>&1 | grep -E "sftp.c|error"
gcc -std=gnu23 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-function -DSSHD_PORT=22 \
    -DHOST_KEY_DIR="\"$T\"" -DAUTHORIZED_KEYS="\"$T/authorized_keys\"" -DMONOCS_AUTH_FILE="\"$T/htdigest\"" \
    -Ios/api/include -I$B -I$B/src -I$B/inc -o "$T/sshd" services/sshd/sshd.c "$T/linux_main.o" "$T/sftp.o" \
    $(find $B/src -name '*.c') -lutil 2>&1 | grep -E "sshd.c|error"
[ -x "$T/sshd" ] || exit 1
cd "$T"
PW="testpass-$$"
printf 'tester:Monocs:%s\n' "$(printf 'tester:Monocs:%s' "$PW" | md5sum | cut -d' ' -f1)" > htdigest
printf '#!/bin/sh\necho "%s"\n' "$PW" > askpass.sh
printf '#!/bin/sh\necho "wrong-password"\n' > askbad.sh
chmod +x askpass.sh askbad.sh
ssh-keygen -q -t ecdsa -b 256 -N "" -f id_ecdsa
ssh-keygen -q -t rsa -b 3072 -N "" -f id_rsa
ssh-keygen -q -t ed25519 -N "" -f id_ed
ssh-keygen -q -t ecdsa -b 256 -N "" -f id_other
cat id_ecdsa.pub id_rsa.pub id_ed.pub > authorized_keys
./sshd $PORT > sshd.log 2>&1 &
SPID=$!
trap 'kill $SPID 2>/dev/null; rm -rf "$T"' EXIT
sleep 1
O="-p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -o LogLevel=ERROR"
fail=0
# run 名前 期待する文字 ssh のオプション...  （シェルに echo を打って、返ってくるかを見る）
run() {
    name=$1; want=$2; shift 2
    out=$( (sleep 1; echo 'echo marker-$((40+2))'; sleep 1; echo exit; sleep 1) | timeout 30 ssh -tt $O "$@" tester@127.0.0.1 2>&1 | tr -d '\r')
    if echo "$out" | grep -aq "$want"; then echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name: $(echo "$out" | tail -2)"; fi
}
export DISPLAY=x SSH_ASKPASS_REQUIRE=force
SSH_ASKPASS=$T/askpass.sh run "password" "marker-42" -o PubkeyAuthentication=no
SSH_ASKPASS=$T/askbad.sh run "wrong password is refused" "Permission denied" -o PubkeyAuthentication=no -o NumberOfPasswordPrompts=1
SSH_ASKPASS=$T/askbad.sh run "ecdsa key" "marker-42" -i id_ecdsa -o PasswordAuthentication=no
SSH_ASKPASS=$T/askbad.sh run "rsa key (rsa-sha2-512)" "marker-42" -i id_rsa -o PasswordAuthentication=no
SSH_ASKPASS=$T/askbad.sh run "rsa key (rsa-sha2-256)" "marker-42" -i id_rsa -o PasswordAuthentication=no -o PubkeyAcceptedAlgorithms=rsa-sha2-256
SSH_ASKPASS=$T/askbad.sh run "ed25519 key is refused (not supported)" "Permission denied" -i id_ed -o PasswordAuthentication=no
SSH_ASKPASS=$T/askbad.sh run "unknown key is refused" "Permission denied" -i id_other -o PasswordAuthentication=no
SSH_ASKPASS=$T/askpass.sh run "rekey (RekeyLimit 16K)" "marker-42" -o PubkeyAuthentication=no -o RekeyLimit=16K
out=$(timeout 10 ssh -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i id_ecdsa -c aes256-gcm@openssh.com tester@127.0.0.1 true 2>&1)
echo "$out" | grep -q "no matching cipher" && echo "ok   other ciphers are refused" || { fail=$((fail + 1)); echo "FAIL other ciphers: $out"; }
# シェルで exit を打つと、ssh が終了の値 0 で終わる（exit を打たずに切ると 255）
(sleep 1; echo exit; sleep 5) | timeout 20 ssh -tt $O -i id_ecdsa tester@127.0.0.1 > /dev/null 2>&1
rc=$?
[ $rc -eq 0 ] && echo "ok   exit closes the session (status 0)" || { fail=$((fail + 1)); echo "FAIL exit: status $rc"; }
# 大きな出力（300KB）と、鍵の交換し直しをまたぐ出力
(sleep 1; echo 'head -c 300000 /dev/zero | tr -c x x; echo; echo done-big'; sleep 3; echo exit; sleep 1) | timeout 40 ssh -tt $O -i id_ecdsa -o RekeyLimit=64K tester@127.0.0.1 > big.out 2>&1
out=$(tr -d '\r\n' < big.out | tr -cd 'x' | wc -c)
[ "$out" -ge 300000 ] && echo "ok   big output with rekey ($out bytes)" || { fail=$((fail + 1)); echo "FAIL big output: $out: $(head -c 300 big.out)"; }
# exec（ssh host コマンド）: 出力・終了の値・標準入力・大きな出力・途中で切る
E="ssh $O -i id_ecdsa tester@127.0.0.1"
out=$(timeout 20 $E 'echo hello-exec; echo to-stderr >&2; exit 3' 2>&1 < /dev/null); rc=$?
[ "$out" = "$(printf 'hello-exec\nto-stderr')" ] && [ $rc -eq 3 ] && echo "ok   exec: output and status" || { fail=$((fail + 1)); echo "FAIL exec: rc $rc: $out"; }
head -c 3000000 /dev/urandom > in.bin
timeout 60 $E 'cat' < in.bin > out.bin 2>&1
cmp -s in.bin out.bin && echo "ok   exec: 3MB through stdin and stdout" || { fail=$((fail + 1)); echo "FAIL exec stdin: $(wc -c < out.bin) bytes"; }
timeout 3 $E 'sleep 30' < /dev/null > /dev/null 2>&1
out=$(timeout 20 $E 'echo after-cut' < /dev/null 2>&1)
[ "$out" = "after-cut" ] && echo "ok   exec: works after a cut-off command" || { fail=$((fail + 1)); echo "FAIL exec after cut: $out"; }
out=$(timeout 20 $E 'true' < /dev/null 2>&1; echo "rc=$?")
[ "$out" = "rc=0" ] && echo "ok   exec: no output" || { fail=$((fail + 1)); echo "FAIL exec no output: $out"; }
# 同時に複数の接続（3 つまで）: 対話のシェルを開いたまま exec、exec を 2 つ同時（順に動く）、対話の 2 つ目は使用中、4 つ目は断る
(sleep 8; echo exit; sleep 1) | timeout 20 ssh -tt $O -i id_ecdsa tester@127.0.0.1 > shell1.out 2>&1 &
SH1=$!
sleep 2
out=$(timeout 20 $E 'echo during-shell' < /dev/null 2>&1)
[ "$out" = "during-shell" ] && echo "ok   exec while an interactive shell is open" || { fail=$((fail + 1)); echo "FAIL exec with shell: $out"; }
out=$( (sleep 2; echo exit) | timeout 20 ssh -tt $O -i id_ecdsa tester@127.0.0.1 2>&1 | tr -d '\r')
echo "$out" | grep -q "console is in use" && echo "ok   second interactive shell: console in use" || { fail=$((fail + 1)); echo "FAIL second shell: $(echo "$out" | head -2)"; }
timeout 20 $E 'sleep 2; echo first' < /dev/null > ex1.out 2>&1 &
X1=$!
sleep 0.5
timeout 20 $E 'echo second' < /dev/null > ex2.out 2>&1 &
X2=$!
sleep 0.5
out=$(timeout 10 ssh $O -i id_ecdsa tester@127.0.0.1 'echo fourth' < /dev/null 2>&1)
echo "$out" | grep -q "fourth" && { fail=$((fail + 1)); echo "FAIL fourth connection was accepted"; } || echo "ok   fourth connection is refused"
wait $X1 $X2
[ "$(cat ex1.out)" = "first" ] && [ "$(cat ex2.out)" = "second" ] && echo "ok   two execs at once (run one after the other)" || { fail=$((fail + 1)); echo "FAIL two execs: $(cat ex1.out) / $(cat ex2.out)"; }
wait $SH1
# sftp・scp・sshfs（services/sshd/sftp.c。Linux ではふつうのファイルシステムの上で）
D=$T/sftproot
mkdir -p $D
head -c 2500000 /dev/urandom > big.bin
printf 'hello sftp\n' > small.txt
cat > batch.txt <<B
mkdir $D/dir
put big.bin $D/dir/big.bin
put small.txt $D/small.txt
rename $D/small.txt $D/dir/renamed.txt
ls $D/dir
get $D/dir/big.bin back.bin
get $D/dir/renamed.txt back.txt
df $D
rm $D/dir/renamed.txt
B
out=$(timeout 60 sftp -b batch.txt -P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -o LogLevel=ERROR -i id_ecdsa tester@127.0.0.1 2>&1)
cmp -s big.bin back.bin && cmp -s small.txt back.txt && echo "$out" | grep -q "renamed.txt" && [ ! -e $D/dir/renamed.txt ] && echo "ok   sftp: mkdir, put, rename, ls, get, df, rm" || { fail=$((fail + 1)); echo "FAIL sftp: $(echo "$out" | tail -5)"; }
timeout 60 scp -O -q -P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -i id_ecdsa big.bin tester@127.0.0.1:$D/scp-legacy.bin < /dev/null > /dev/null 2>&1
timeout 60 scp -q -P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -i id_ecdsa big.bin tester@127.0.0.1:$D/scp.bin && \
timeout 60 scp -q -P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -i id_ecdsa tester@127.0.0.1:$D/scp.bin scp-back.bin
cmp -s big.bin scp-back.bin && echo "ok   scp (SFTP): up and down 2.5MB" || { fail=$((fail + 1)); echo "FAIL scp"; }
# sshfs（FUSE が使えるときだけ）
mkdir -p mnt
if timeout 30 sshfs -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -o IdentityFile=$T/id_ecdsa tester@127.0.0.1:$D mnt 2>sshfs.err; then
    cp big.bin mnt/fs.bin && printf 'edited\n' > mnt/note.txt && mv mnt/note.txt mnt/note2.txt && mkdir mnt/sub && \
    cmp -s big.bin mnt/fs.bin && [ "$(cat mnt/note2.txt)" = "edited" ] && ls mnt | grep -q sub && rm mnt/fs.bin && rmdir mnt/sub && \
    cmp -s $D/note2.txt mnt/note2.txt && echo "ok   sshfs: cp, write, rename, mkdir, rm, rmdir" || { fail=$((fail + 1)); echo "FAIL sshfs: $(ls -la mnt)"; }
    # フォルダを 200 個開いたまま（bfs は読んでいる途中のフォルダを開いたまま下へ進む。手が 16 では足りなかった）
    mkdir -p $D/many && for i in $(seq 200); do mkdir -p $D/many/d$i; done
    python3 -I -c 'import os,sys; h=[os.scandir(os.path.join(sys.argv[1], "d%d" % i)) for i in range(1, 201)]; print(len(h))' mnt/many > many.out 2>&1
    [ "$(cat many.out)" = 200 ] && echo "ok   sshfs: 200 directories open at once" || { fail=$((fail + 1)); echo "FAIL sshfs many dirs: $(tail -1 many.out)"; }
    rm -rf $D/many
    fusermount -u mnt
else
    echo "skip sshfs: $(head -1 sshfs.err)"
fi
# USB から届いたことにすると、パスワードなしで入れる
kill $SPID; sleep 0.5
SSHD_NETIF=usb ./sshd $PORT > sshd2.log 2>&1 &
SPID=$!
sleep 1
SSH_ASKPASS=$T/askbad.sh run "from USB: no password" "marker-42" -o PubkeyAuthentication=no
# 壊れた入力を送っても落ちない（検査に引っかからない）
python3 - "$PORT" <<'PY'
import socket, os, random, sys
port = int(sys.argv[1]); random.seed(3)
for i in range(300):
    s = socket.create_connection(('127.0.0.1', port), timeout=2)
    k = i % 4
    try:
        if k == 0: s.sendall(os.urandom(random.randrange(1, 3000)))
        elif k == 1: s.sendall(b'SSH-2.0-fuzz\r\n' + os.urandom(random.randrange(1, 3000)))
        elif k == 2: s.sendall(b'SSH-2.0-fuzz\r\n' + (random.randrange(1, 40000)).to_bytes(4, 'big') + os.urandom(random.randrange(1, 40000)))
        else:
            pay = bytes([20]) + os.urandom(16) + os.urandom(random.randrange(0, 600))
            pad = 8 - (5 + len(pay)) % 8; pad += 8 if pad < 4 else 0
            s.sendall(b'SSH-2.0-fuzz\r\n' + (1 + len(pay) + pad).to_bytes(4, 'big') + bytes([pad]) + pay + bytes(pad))
        s.settimeout(0.2)
        try: s.recv(65536)
        except Exception: pass
    except Exception: pass
    s.close()
PY
if kill -0 $SPID 2>/dev/null && ! grep -q "ERROR\|runtime error" sshd2.log sshd.log; then echo "ok   fuzz (300 broken connections)"; else fail=$((fail + 1)); echo "FAIL fuzz: $(grep -h 'ERROR\|runtime error' sshd.log sshd2.log | head -3)"; fi
SSH_ASKPASS=$T/askbad.sh run "still works after fuzz" "marker-42" -o PubkeyAuthentication=no
[ $fail -eq 0 ] && echo "ssh test: すべて成功" || echo "ssh test: NG ($fail)"
exit $fail
