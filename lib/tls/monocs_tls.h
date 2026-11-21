// monocs_tls.h: TLS（HTTPS など）でつなぐ。共有のライブラリ /lib/tls（中身は BearSSL。/lib/bearssl）
//
//   使い方: compile_flags.txt か cc に -ltls。`#include <monocs_tls.h>`
//     MonocsTls *t = monocs_tls_connect("example.com", 443, 10000);
//     if (!t) { printf("%s\n", monocs_tls_error()); return 1; }
//     monocs_tls_send(t, req, strlen(req));
//     while ((n = monocs_tls_recv(t, buf, sizeof buf, 10000)) > 0) ...
//     monocs_tls_close(t);
//
// 相手の証明書は、/etc/ssl/certs/ca-certificates.crt（Linux と同じ場所。認証局の証明書を並べた PEM）で確かめ、
// 名前（host）が証明書と合うことも確かめる。日時が合っていないと確かめられないので、つなぐ前に時計が合って
// いること（SNTP のサービス）。TLS 1.2（BearSSL が対応する版。1.3 はない）。乱数は getentropy。
// つなぐたびに認証局の証明書を読むので、最初の接続は少しかかる（同じアプリの中の 2 回目からは覚えている）。
#pragma once

typedef struct MonocsTls MonocsTls;

// host（名前。証明書と比べる）の port につないで、TLS のやりとりを済ませる。timeout_ms は全体の上限
// （負ならいつまでも）。だめなら NULL（理由は monocs_tls_error）
MonocsTls *monocs_tls_connect(const char *host, int port, int timeout_ms);
// len バイトを送る。送れたら len、だめなら -1
int monocs_tls_send(MonocsTls *t, const void *buf, int len);
// 受け取る（monocs_tcp_recv と同じ）: 受け取ったバイト数、0 = 相手が閉じた、-1 = 時間切れ、-2 = エラー
int monocs_tls_recv(MonocsTls *t, void *buf, int len, int timeout_ms);
void monocs_tls_close(MonocsTls *t);
// 最後のエラーの説明（英語の 1 行）
const char *monocs_tls_error(void);
// 認証局の証明書のファイルを変える（既定は /etc/ssl/certs/ca-certificates.crt）。次の接続から
void monocs_tls_set_ca_file(const char *path);
