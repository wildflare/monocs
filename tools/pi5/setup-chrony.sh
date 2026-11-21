#!/bin/sh
# Pi 5 を Monocs の時刻の配り手（NTP サーバー）にする。Monocs のサービス services/sntp.c が問い合わせる。
#   sh tools/pi5/setup-chrony.sh          入れる（chrony を入れ、Monocs 用の設定を足す）
#   sh tools/pi5/setup-chrony.sh remove   Monocs 用の設定を外す（chrony は残す）
#
# chrony を入れると systemd-timesyncd の代わりになる（Pi 5 自身の時刻合わせも chrony が続ける）。
# 元の設定（/etc/chrony/chrony.conf）は書き換えず、/etc/chrony/conf.d/monocs.conf に分ける
# （Debian の chrony.conf は confdir /etc/chrony/conf.d を読む）。
set -eu
CONF=/etc/chrony/conf.d/monocs.conf   # 2026-11-21 より前に作ったものは seedos.conf だった（Pi 5 のものは 2026-11-21 に直した）

if [ "${1:-}" = remove ]; then
    sudo rm -f "$CONF"
    sudo systemctl restart chrony
    echo "removed $CONF"
    exit 0
fi

dpkg -s chrony > /dev/null 2>&1 || sudo apt-get install -y chrony
sudo tee "$CONF" > /dev/null <<'EOF'
# Monocs（USB でつないだ Zero 2 W、192.168.7.0/24）に時刻を配る（Monocs のリポジトリの tools/pi5/setup-chrony.sh）
allow 192.168.7.0/24
# インターネットにつながらないときも、Pi 5 の時計で答える（階層 10: ほかに正しい時刻があればそちらが選ばれる）
local stratum 10
EOF
sudo systemctl restart chrony
echo "installed $CONF"
chronyc tracking | head -4
