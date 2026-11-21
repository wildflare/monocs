// サービスから頼まれたシェルの 1 行を動かす（ssh の exec。os/kernel/rexec.c）
#pragma once

#include <stdbool.h>

// ---- サービスの側（OS API の monocs_rexec_*） ----
// 動かす行を頼む。0 = 受け付けた、-1 = ほかの行を動かしている・長すぎる
int rexec_start(const char *cmd);
// 行の出力（標準出力と標準エラー）を最大 n バイト読む（待たない）。読んだバイト数
int rexec_read(void *buf, int n);
// 行の標準入力に渡す（待たない）。受け取ったバイト数（入れ物がいっぱいなら少ない）。eof で終わりを知らせる
int rexec_write(const void *p, int n);
void rexec_write_eof(void);
// 終わったか。-1 = まだ（待っている・動いている）、0〜255 = 終わった（終了の値。出力が残っていれば rexec_read で読む）
int rexec_status(void);
// 後始末（終わった後に出力を読み終えたら呼ぶ。動いている途中なら止める（Ctrl-\ と同じ）。出力は捨てる）
void rexec_release(void);
// 頼んだ行が、まだ始まっていない（シェルがコマンド・アプリを動かしていて、プロンプトに戻るのを待っている）
bool rexec_queued(void);

// ---- シェルの側（os/shell/shell.c、os/libc/syscalls.c） ----
// 頼まれた行があれば取り出して「動いている」にする（なければ NULL）
const char *rexec_take(void);
void rexec_done(int status);
bool rexec_running(void);
void rexec_putc(char c);        // 行の標準出力・標準エラー（入れ物がいっぱいなら読まれるまで待つ）
// lock を持って 1 文字入れる（輪がいっぱいなら lock を外して待つ。os/libc/syscalls.c の標準出力）
struct klock;
void rexec_putc_locked(char c, struct klock *lock);
int rexec_getc(void);           // 行の標準入力（届くまで待つ。終わりは -1）
int rexec_read_in(void *buf, int n);    // 行の標準入力のうち、届いている分（待たない）
void rexec_drain(int ms);       // 行の出力を、サービスが読み終えるまで待つ（ms まで。reboot・poweroff の表示を届ける）
