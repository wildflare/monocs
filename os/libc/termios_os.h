// 端末の設定（termios）と、アプリの stdin の行の扱い（os/libc/termios.c）
#pragma once

bool term_input_ready(void);     // 標準入力が読めるか（select・poll）
#include <stdbool.h>

int term_read(void *buf, int count);    // 端末から読む（ICANON なら 1 行ずつ）。0 = EOF
int term_getc(void);                    // 1 バイト（EOF は -1）
bool term_ixon(void);                   // 入力の IXON（Ctrl-S・Ctrl-Q で出力を止める・再開する）
bool term_onlcr(void);                  // 出力の '\n' を "\r\n" にするか（OPOST・ONLCR）
void termios_reset(void);               // 既定に戻す（アプリの終わり）
bool termios_push(void);                // 入れ子のアプリ（system()）の前: 退避して既定から
void termios_pop(void);                 // 入れ子のアプリの後: 戻す
