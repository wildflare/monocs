// ネットワーク（USB イーサネットの上の IPv4。TCP/IP は lwIP）
#pragma once

#include <stdint.h>

// Monocs のアドレス（Pi 5 側は 192.168.7.1/24。NetworkManager の monocs-usb）
#define NET_IP_ADDR     192, 168, 7, 2
#define NET_NETMASK     255, 255, 255, 0
#define NET_MAC_ADDR    { 0x02, 0x6E, 0x65, 0x77, 0x6F, 0x02 }

// lwIP を初期化する（起動時にコア 0 で 1 回）
void net_init(void);
// 受け取ったイーサネットのフレームを処理する（USB の割り込みから。USB のロックの外）
void net_input(const uint8_t *frame, uint32_t len);
// lwIP のタイマーを進める（タイマー割り込みから）
void net_timer(void);

// lwIP を呼ぶ間のロック（コア間のスピンロック。割り込みを止めて取る。同じコアなら重ねて取れる）
uint64_t net_lock(void);
void net_unlock(uint64_t daif);

// 状態の表示（シェルの net）
void net_status(void);

// 内蔵 Wi-Fi の口（os/drivers/wifi.c がつながったら）。アドレスは DHCP
void net_wifi_attach(const uint8_t mac[6]);
void net_wifi_link(bool up);
bool net_wifi_address(char *ip, char *mask, char *gw, int size);   // DHCP でもらえたら true

// どの口か（アプリには monocs.h の MONOCS_NETIF_*）
#define NET_IF_USB      0
#define NET_IF_WIFI     1
struct netif;
struct ip4_addr;
// addr を自分のアドレスにしている口（なければ NULL）。net_lock の中で
struct netif *net_netif_of_addr(const struct ip4_addr *addr);
// 今処理しているパケットが届いた口（lwIP のコールバックの中で。NET_IF_*）
int net_input_kind(void);
