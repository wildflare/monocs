#!/bin/sh
# ユーザーの端末のエディタ mi（~/dev/tools/mini-editor、別のリポジトリ）を実機の /root/apps/mi に送る（ソースのまま）。
#   sh tools/apps/mi.sh [MI_DIR]
# 中身は tools/apps/push.sh（どのプログラムのフォルダでも送れる）。実機では  mi ファイル名
set -eu
cd "$(dirname "$0")/../.."
sh tools/apps/push.sh "${1:-$HOME/dev/tools/mini-editor}" mi
