// cc --report（2026-11-26。docs/sandbox.md の 6 の 2）: プログラムが使う OS の関数を種類ごとに並べる
#pragma once
#include <stdbool.h>

void report_begin(void);                    // ここから、つなぐときに OS の表から引いた名前を覚える（run.c の resolve）
void report_note(const char *name);         // run.c の resolve から
void report_print(const char *const *files, int nfiles, const char *const *flags);   // 一覧を出して、覚えたものを捨てる
