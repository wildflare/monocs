// Linux 用のプロジェクトの Makefile から、Monocs でコンパイルするのに要るものを読む（os/slimcc/makefile.c）
#pragma once
#include <stddef.h>

typedef struct {
    char **flags;       // コンパイラのオプション（-std= -D -U -I（絶対パス） -l名前（/lib にあるものだけ））
    int nflags;
    char **srcs;        // コンパイルする .c（絶対パス。Makefile の順）。わからなければ 0 個
    int nsrcs;
    char target[128];   // 既定のゴールからたどったプログラムの名前（fd、mi）。わからなければ ""
} MakeInfo;

// dir の Makefile（GNUmakefile、makefile、Makefile の順に探す）を読む。なければ false
// make の引数（目標 変数=値）を覚えるファイル（シェルの make が書く。make_read が読む）
#define MAKE_ARGS_FILE ".monocs-make"

bool make_read(const char *dir, MakeInfo *mi);
void make_free(MakeInfo *mi);

// dir の Makefile が作るプログラムの名前（シェルが DIR/名前 を DIR のプログラムとして動かすため）
bool make_program_name(const char *dir, char *name, size_t size);
