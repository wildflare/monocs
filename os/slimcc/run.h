// slimcc-a64（libslimcc）で C のソースをメモリ上でコンパイルし、その場で実行する
#pragma once

// 実行ファイル path（make・cc -o が作ったもの）を読み込んで main を呼ぶ（2026-11-17 から、ソースは直接動かさない。
// Linux と同じ）。戻り値は main の戻り値（または exit の引数）。実行ファイルでなければ・失敗したら -1
int slimcc_run_program(const char *path, int argc, char **argv);
// run: ソース（.c・フォルダ）か src（path は表示の名前）をその場でコンパイルして動かす（ファイルは作らない。
// 2 回目からはコンパイルのキャッシュで速い）。flags は -std=・-D・-U・-I・-l・-f…。path が実行ファイルならそれを動かす
int slimcc_run_source(const char *path, const char *src, const char *const *flags, int argc, char **argv);

// プログラム（実行ファイルか、ソースの .c・フォルダ）を読み込んで、OS の関数とつなぐ（実行はしない。サービスの
// 読み込み、os/kernel/svc.c）。ソースはコンパイルする（キャッシュあり）。失敗したら NULL。解放は slimcc_free_image
typedef struct SlimccImage SlimccImage;
SlimccImage *slimcc_load_program(const char *path);
// 実行ファイル（make・cc -o が作る。Linux と同じく ./prog で動かす）: path（.c かフォルダ）をコンパイルして out に書く
bool slimcc_make_executable(const char *path, const char *const *flags, const char *out);
// cc -o OUT FILE.c...: いくつかの .c をコンパイルして 1 つの実行ファイルにする（flags は run と同じ）
bool slimcc_make_executable_files(const char *const *files, int nfiles, const char *const *flags, const char *out);
// path が Monocs の実行ファイルか（先頭の印で見る。FAT には実行の許可の印がない）
bool slimcc_is_executable(const char *path);
void *slimcc_program_symbol(SlimccImage *img, const char *name);
void slimcc_unload_program(SlimccImage *img);
