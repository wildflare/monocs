#!/bin/sh
# ソケット・名前・select・poll・alarm のうち、相手のいらないもの（tools/test/netapi.c）を、Linux（gcc・glibc）と
# 実機の Monocs で動かして比べる（QEMU は USB の機器がなく、ネットワークが動かないので実機で）。
#   sh tools/test/netapi.sh   （実機に新しいカーネルとヘッダーを入れてから）
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
N="python3 $R/tools/uart/nos.py"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -std=gnu2x -O1 -o "$T/netapi" "$R/tools/test/netapi.c"
"$T/netapi" > "$T/linux.txt"
$N push "$R/tools/test/netapi.c" /tmp/netapi.c > /dev/null
$N cmd 'cd /tmp; cc -o netapi netapi.c && ./netapi > netapi_out.txt' > /dev/null 2>&1
$N cmd 'cat /tmp/netapi_out.txt' > "$T/out.txt"
if diff "$T/linux.txt" "$T/out.txt"; then echo "ok: netapi = Linux ($(wc -l < "$T/out.txt") 行)"; else echo "DIFF (< Linux, > Monocs)"; exit 1; fi
