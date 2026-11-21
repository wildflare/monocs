#!/bin/sh
# Pi 5 の usb0（Monocs の USB イーサネット）に固定のアドレス 192.168.7.1/24 を付ける（NetworkManager の接続 monocs-usb）。
#   sh tools/pi5/setup-usbnet.sh          作る（すでにあれば何もしない）
#   sh tools/pi5/setup-usbnet.sh remove   消す
# Monocs は 192.168.7.2（os/net/net.c）。既定の経路にはしない（Pi 5 のインターネットの経路は変えない）。
set -eu
NAME=monocs-usb   # 2026-11-21 より前に作ったものは new_os-usb だった（Pi 5 のものは 2026-11-21 に直した）

if [ "${1:-}" = remove ]; then
    sudo nmcli connection delete "$NAME"
    exit 0
fi
if nmcli -t -f NAME connection show | grep -qx "$NAME"; then
    echo "$NAME: already exists"
    exit 0
fi
sudo nmcli connection add type ethernet ifname usb0 con-name "$NAME" autoconnect yes \
    ipv4.method manual ipv4.addresses 192.168.7.1/24 ipv4.never-default yes ipv6.method link-local
echo "created $NAME"
