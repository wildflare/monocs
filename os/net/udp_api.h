// アプリ・サービスから使える UDP の API（os/net/udp_api.c。アプリからは monocs_udp_*）
#pragma once

// port で受け取るソケットを開く（0 なら空いている番号）。ハンドル（0 以上）か -1
int udp_api_open(int port);
// ip（"192.168.7.1" の形か名前。名前は待たずに引く: まだ引けていなければ -1 で、少し後にもう一度）の port に送る。送ったバイト数か -1（1472 バイトまで）
int udp_api_sendto(int h, const char *ip, int port, const void *buf, int len);
// 1 つ受け取る（len より長ければ切り詰める）。ip（16 バイト以上。NULL 可）と port（NULL 可）に送り元。
// timeout_ms: 0 = 待たない、負 = いつまでも。受け取ったバイト数、-1 = 時間切れ、-2 = ハンドルが違う
int udp_api_recvfrom(int h, void *buf, int len, char *ip, int *port, int timeout_ms);
void udp_api_close(int h);
int udp_api_open_addr(const char *ip, int port);    // ip のアドレスで受ける（NULL ならどこでも）
int udp_api_ready(int h, bool *readable);           // 待たずに調べる（select・poll）。ハンドルが違えば -1
int udp_api_local(int h, char *ip, int *port);      // 自分のアドレスとポート。0 で成功
// owner（0 = アプリ、1〜 = サービス）が開いたままのソケットを閉じる
void udp_api_close_owner(int owner);
