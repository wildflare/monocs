// コンパイルのキャッシュ（os/slimcc/cache.c）
#pragma once

#include <stdint.h>
#include "libslimcc.h"

// まとめてコンパイルする前後に呼ぶ（読んだファイルのハッシュを、その間だけ覚える）
void cache_begin(void);
void cache_end(void);
// VFS がファイルを書き換える・消す・名前を変える（key は絶対パス・小文字、NULL ならすべて）: 覚えたハッシュを忘れる
void cache_file_changed(const char *key);
// 覚えたハッシュのファイルが書き換えられた・忘れたときに進む番号（つないだ結果を使い回せるかを、これで見る）
unsigned cache_src_gen(void);
// つないだ結果の覚え（os/slimcc/run.c）: key（絶対パス・小文字、NULL ならすべて）のファイルが変わった
void image_cache_changed(const char *key);

// path をコンパイルする。前の結果が使えればそれを読み込む。optkey はオプションを表す文字列
// （オプションが違えば別のもの）。失敗したら NULL
SlimccObj *cache_compile(const char *path, const SlimccOptions *opt, const char *optkey);

// 起動してからの、使えた回数と使えなかった回数
void cache_stats(uint64_t *hits, uint64_t *misses);
// 今のカーネルのキャッシュの数と大きさ。すべて消す
void cache_usage(int *files, uint64_t *bytes);
void cache_clear(void);
// 1 つのプログラムのキャッシュだけ消す（abs はソースの絶対パス。dir ならフォルダの下のもの）。消した数
int cache_clear_program(const char *abs, bool dir);
// カーネルの識別（.text・.rodata のハッシュ。perf の /kernel8.sym が今のカーネルのものかも、これで確かめる）
