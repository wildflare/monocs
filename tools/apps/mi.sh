#!/bin/sh
# ユーザーの端末のエディタ mi（~/dev/tools/mini-editor、別のリポジトリ）を実機の /root/apps/mi に送る（ソースのまま）。
#   sh tools/apps/mi.sh [MI_DIR]
# 中身は tools/apps/push.sh -a（フォルダの中身を全部送る。Pi 5 と実機で同じ中身に。2026-11-27）。実機では  mi ファイル名
set -eu
cd "$(dirname "$0")/../.."
sh tools/apps/push.sh -a "${1:-$HOME/dev/tools/mini-editor}" mi
