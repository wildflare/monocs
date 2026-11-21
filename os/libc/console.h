// コンソール（stdin・stdout・stderr）とリダイレクト（os/libc/syscalls.c）
#pragma once

#include <stdbool.h>
#include <stddef.h>

// シェルのリダイレクト・パイプの間、stdout をファイルに向ける（append なら後ろに足す）。開けなければ false
bool console_redirect_out(const char *path, bool append);
// stderr をファイルに向ける（path が NULL なら stdout と同じところ: 2>&1）
bool console_redirect_err(const char *path, bool append);
// どれも /dev/null なら、書いたものを捨て、読むとすぐ終わり
// stdin をファイルから読む。開けなければ false
bool console_redirect_in(const char *path);
// 向け先を戻す（まとめて書いていた分を書き出して閉じる）
void console_redirect_end(void);
// 入れ子（アプリの中の system()）の前後: 今のリダイレクトを子に引き継ぐ土台にする・外側に戻す
bool console_redirect_push(void);
void console_redirect_rexec(void);     // ssh の exec の行の入出力（os/kernel/rexec.c）。この後で push して土台に
void console_redirect_pop(void);
// stdout がファイルに向いているか（ls が色を付けない、列に並べない。GNU と同じ）
bool console_out_is_file(void);
// stdin がファイルから読んでいるか（cat が Ctrl-D を終わりとみなすか）
bool console_in_is_file(void);
// stderr がファイル・/dev/null・ssh の exec に向いているか（isatty(2)。2026-11-21 まではいつも端末と答えていた）
bool console_err_is_file(void);

// fd 0〜2 の read・write（os/fs/vfs.c。stdout・stdin の向け先に従う）
int console_write(int fd, const void *buf, size_t count);
int console_read(int fd, void *buf, size_t count);
