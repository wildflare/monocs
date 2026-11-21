// slimcc（ファイル単位のコンパイル）と slimld（リンク）のコマンド。シェルと OS API から使う
#pragma once

// slimcc [-c] [-o OUT.o] [-I DIR]... [-D NAME[=VAL]]... [-std=STD] FILE.c|FILE.S|FILE.s
// 出力を省くと FILE.o。成功なら 0
int slimcc_cmd(int argc, char **argv);

// slimld [-v] [-Map FILE] -o kernel8.img FILE.o... LIB.a...
// Monocs のカーネル（os/boot/linker.ld と同じ配置）を作る。成功なら 0
int slimld_cmd(int argc, char **argv);
// アプリから（monocs_cc・monocs_ld）。カーネル用のヒープで動かす
int slimcc_cmd_app(int argc, char **argv);
int slimld_cmd_app(int argc, char **argv);
