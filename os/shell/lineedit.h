// シェルの 1 行の編集（os/shell/lineedit.c）
#pragma once

#include <stdbool.h>

#define LINE_EDIT_MAX       1024
#define LINE_EDIT_EOF       (-1)    // 行が空のときに Ctrl-D
#define LINE_EDIT_CANCEL    (-2)    // Ctrl-C

typedef struct {
    // i 番目のコマンドの名前（なければ NULL）。行の最初の語の補完に使う
    const char *(*command_name)(int i);
} LineCompleter;

// prompt を出して 1 行を読み、長さを返す（buf は size バイト。'\0' で終わる）。
// use_history なら ↑↓ で履歴を出す。lc があれば Tab で補う（ファイル名はいつも）
int line_edit(const char *prompt, char *buf, int size, const LineCompleter *lc, bool use_history);
// 入力を待つ間に呼ぶ関数（シェルのプロンプトの間だけ。ssh の exec の行を動かす。NULL で外す）
void line_edit_set_idle(void (*fn)(void));

// UTF-8 の文字列の表示幅（全角は 2）

// 履歴（古い順）。消す（/.shell_history も空にする）
int line_edit_history_count(void);
const char *line_edit_history_get(int i);
void line_edit_history_clear(void);

// 履歴に足す（/.shell_history にも書く）
void line_edit_add_history(const char *line);
