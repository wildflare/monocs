#!/bin/sh
# 実機の /usr/bin（付いてくるコマンド）と /usr/sbin（サービス）を、リポジトリのソースに入れ替える（どちらもソースの
# まま。名前で打てば、その場でコンパイルして動く）。
#   NOS_HOST=アドレス sh tools/update_bin.sh
# 1. bin・services を /usr/bin.new・/usr/sbin.new に送る（今のものはそのまま）
# 2. sftp で /usr/bin・/usr/sbin と入れ替え、/etc/services.txt を置き換える（シェルを通さない名前の変更）
# 3. 古いものを消して、再起動する（サービスを新しいソースで始める）
set -eu
: "${NOS_HOST:?NOS_HOST を設定してください（Wi-Fi のアドレス）}"
N="python3 tools/uart/nos.py"
KEY=${NOS_SSH_KEY:-$HOME/.ssh/id_ecdsa_monocs}
SFTP="sftp -q -b - -o BatchMode=yes -o IdentitiesOnly=yes -i $KEY monocs@$NOS_HOST"
$N cmd 'rm -rf /usr/bin.new /usr/sbin.new' > /dev/null
$N pushdir bin /usr/bin.new | tail -1
$N pushdir services /usr/sbin.new | tail -1
$N cmd 'rm -f /usr/sbin.new/services.txt' > /dev/null
$SFTP <<SFTP
-rename /usr/bin /usr/bin.old
rename /usr/bin.new /usr/bin
-rename /usr/sbin /usr/sbin.old
rename /usr/sbin.new /usr/sbin
put services/services.txt /etc/services.txt.tmp
-rm /etc/services.txt
rename /etc/services.txt.tmp /etc/services.txt
SFTP
$N cmd 'rm -rf /usr/bin.old /usr/sbin.old; ls /usr/bin | wc -l; ls /usr/sbin'
echo "update_bin: 入れ替えました。再起動します"
$N reboot | tail -1
