// slimld: Monocs のカーネル用の静的リンカ
//
// AArch64 の ELF の .o と .a（ar 形式）を読み、os/boot/linker.ld と同じ配置で
// kernel8.img（0x80000 から読み込む生のイメージ）を書き出す。
// 配置はリンカスクリプトを読まずに ld.c の表で持つ（linker.ld を変えたら ld.c も合わせる）。
#pragma once

#include <stdbool.h>

typedef struct {
  const char *const *inputs; // .o と .a のパス（NULL で終わる）。.a は全体で 1 つのグループとして扱う
  const char *output;        // kernel8.img
  const char *map;           // シンボルの一覧（「アドレス 名前」）。NULL なら書かない
  bool verbose;
} SlimldOptions;

// 成功なら 0。失敗したらエラーを stderr に出して 1
int slimld_link(const SlimldOptions *opt);
