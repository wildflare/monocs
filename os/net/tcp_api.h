// アプリから使える TCP の API（os/net/tcp_api.c。アプリからは monocs_tcp_*）
#pragma once

// 待ち受けを始める。ハンドル（0 以上）か -1
int tcp_api_listen(int port);
int tcp_api_listen_addr(const char *ip, int port);  // ip に届いたものだけ（NULL ならどこでも）
// 接続を受け付ける。timeout_ms: 0 = 待たない、負 = いつまでも。ハンドルか -1
int tcp_api_accept(int listener, int timeout_ms);
// つなぐ（ip は "192.168.7.1" の形）。ハンドルか -1
int tcp_api_connect(const char *ip, int port, int timeout_ms);
// つなぎ始める（待たない。ip は数字の形だけ）。ハンドルか -1。tcp_api_connect_state: 1 = つながった、0 = 途中、
// -1 = つながらなかった（ハンドルは tcp_api_close で閉じる）
int tcp_api_connect_start(const char *ip, int port);
int tcp_api_connect_state(int h);
// 待たずに調べる（select・poll）: 読めるか・書けるか。ハンドルが違えば -1
int tcp_api_ready(int h, bool *readable, bool *writable);
// 自分のアドレス（ip は 16 バイト以上。NULL 可）とポート（NULL 可）。0 で成功
int tcp_api_local(int h, char *ip, int *port);
bool tcp_api_refused(int h);        // エラーで終わった接続が、相手に断られた（RST）のか
int tcp_api_available(int h);       // 受信バッファの、まだ読んでいないバイト数（ハンドルが違えば -1）
// 送る側だけ閉じる（shutdown(SHUT_WR)）。0 で成功
int tcp_api_shutdown_write(int h);
// 受け取るが取り除かない（MSG_PEEK）。戻り値は tcp_api_recv と同じ
int tcp_api_recv_peek(int h, void *buf, int len, int timeout_ms);
// 送る。送れた（送信の待ち行列に入れた）バイト数。つながっていなければ -1
int tcp_api_send(int h, const void *buf, int len, int timeout_ms);
// 受け取る。受け取ったバイト数、0 = 相手が閉じた、-1 = 時間切れ、-2 = エラー
int tcp_api_recv(int h, void *buf, int len, int timeout_ms);
void tcp_api_close(int h);
// 受け付けた接続が届いた口（NET_IF_USB = 0、NET_IF_WIFI = 1。os/net/net.h）。ハンドルが違えば -1
int tcp_api_netif(int h);
// 相手のアドレス（ip は 16 バイト以上。NULL 可）とポート（NULL 可）。0 で成功
int tcp_api_peer(int h, char *ip, int *port);
// 使っている接続の一覧を表示する（シェルの usb）
void tcp_api_status(void);
// owner（0 = アプリ、1〜 = サービス）が開いたままの接続を閉じる
void tcp_api_close_owner(int owner);
