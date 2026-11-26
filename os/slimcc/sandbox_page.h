// サンドボックスで動かすプログラム（C の Web のページ）の指定から、確かめたソースのファイルの一覧を作る（sandbox_fetch.c）
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "../kernel/sandbox.h"

#define SANDBOX_MAX_FILES   64
#define SANDBOX_PATH_MAX    300

// spec: FILE.c、DIR/（手元のフォルダ）、http://…/FILE.c、http://…/DIR/。どれも #sha256=… を付けられる。
// フォルダは目録 cweb.txt（sha256sum の形）のファイルを取ってきて（手元なら読んで）確かめ、その .c を返す（目録が
// なければ、手元のフォルダは中の .c を全部）。#sha256= はファイルならその中身、フォルダなら目録の中身のハッシュ。
// コンパイルする .c のパスを files に入れて数を返す。だめなら 0（理由を表示する）
int sandbox_prepare(const char *spec, char (*files)[SANDBOX_PATH_MAX], int max);
bool sandbox_fetch(const char *url, char *out, size_t out_size);    // http の 1 ファイル（中身を確かめて置く）
int sandbox_run_files(const char *const *paths, int npaths, int argc, char **argv, enum sandbox_result *res);
int sandbox_run_file(const char *path, int argc, char **argv, enum sandbox_result *res);
