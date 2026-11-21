#!/bin/sh
# os/libc/termcap.c（Monocs の termcap の関数）を Linux でコンパイルし、ncurses（xterm-256color）と同じ答えか比べる。
# tgetflag・tgetnum・tgetstr はすべての名前、tgoto・tparm は文字列ごとにいくつかの引数で
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
REN="-DPC=u_PC -DUP=u_UP -DBC=u_BC -Dospeed=u_ospeed -Dtgetent=u_tgetent -Dtgetflag=u_tgetflag -Dtgetnum=u_tgetnum"
REN="$REN -Dtgetstr=u_tgetstr -Dtgoto=u_tgoto -Dtputs=u_tputs -Dtparm=u_tparm -Dtiparm=u_tiparm"
gcc -std=c2x -O1 -Wall -fsanitize=address,undefined $REN -c -o "$T/termcap.o" "$R/os/libc/termcap.c"
gcc -O1 -fsanitize=address,undefined -o "$T/t" "$R/tools/libc/test_termcap.c" "$T/termcap.o" -ltinfo
"$T/t"
