// SFTP のサーバー（services/sshd/sftp.c）。sshd が subsystem "sftp" のチャンネルで使う
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Sftp Sftp;

Sftp *sftp_new(void);
void sftp_free(Sftp *s);    // 開いたファイル・ディレクトリも閉じる
// 相手から届いたバイトをためる（入れ物は窓より大きいので、窓を守っていれば入る）。入らなければ false
bool sftp_feed(Sftp *s, const void *data, size_t n);
// ためたパケットを、答えの入れ物に空きがある間だけ処理する。処理したバイト数（窓を足し直す量）。壊れていれば *bad
size_t sftp_run(Sftp *s, bool *bad);
size_t sftp_pending_input(Sftp *s);     // ためていて、まだ処理していないバイト数
// 送る答え
const uint8_t *sftp_output(Sftp *s, size_t *len);
void sftp_consumed(Sftp *s, size_t n);
