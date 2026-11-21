// コンソールの写し（タップ）: サービス（telnet サーバーなど）がコンソールにつながる（os/kernel/contap.c）
#pragma once

#include <stddef.h>

// 写しを受け取り始める・やめる。open は、ほかが開いていれば -1
int contap_open(void);
void contap_close(void);
// 写し（コンソールに出した文字）を最大 n バイト読む（待たない）。読んだバイト数
int contap_read(void *buf, int n);
// シェルへの入力を差し込む（Ctrl-C も扱う）
void contap_input(const void *p, int n);
// 端末の画面の大きさを知らせる（0 なら知らない）。monocs_term_size が返す
void contap_set_size(int rows, int cols);
// シェルの exit・logout から: つながっているサービス（telnet・ssh）に「接続を閉じて」と頼む。頼めたら true。
// 写しが開いていない、または最後の入力が写しからでない（シリアルのコンソールで打った）ときは false
bool contap_request_hangup(void);
// サービスから: 閉じるように頼まれていたら true（1 回だけ。monocs_console_hangup）
bool contap_hangup(void);
bool contap_active(void);      // 写しが開いているか（telnet・ssh でつないでいる）
// 開いていて、画面の大きさを知らされていれば true（os/kernel/term.c）
bool contap_size(int *rows, int *cols);
