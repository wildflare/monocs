// 端末（コンソールの向こうの picocom など）の画面の大きさ
#pragma once

// 画面の行数と桁数を得る。わかれば true。わからなければ 24 行 80 桁にして false
bool term_size(int *rows, int *cols);
// 端末の桁数（telnet は知らされた大きさ。ほかは最後に問い合わせた値。まだなら一度だけ問い合わせる）
int term_cols(void);
