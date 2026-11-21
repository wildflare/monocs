// USB（Synopsys DesignWare Hi-Speed USB 2.0 OTG、DWC2）を機器側（ガジェット）として使う
#pragma once

#include <stdint.h>

// コントローラを機器側で初期化し、USB シリアルのコンソールとしてつなぐ（irq_init の後にコア 0 で）
bool usb_init(void);
// 割り込み（irq_dispatch から）
void usb_handle_irq(void);
// USB イーサネット（CDC-ECM）。ホスト側のインターフェースの MAC アドレス（ECM の iMACAddress）。
// Monocs 側の MAC アドレスは os/net/net.c。どちらもローカルに管理するアドレス（先頭の 02）
#define USB_ETH_HOST_MAC_STR "026E65776F01"
// イーサネットのフレームを送る（ホストが使っていない・待ち行列がいっぱいなら false）。どのコアからも呼べる
bool usb_eth_send(const void *frame, uint32_t len);

// 送信中の分を送り終えてから、ホストから切り離す（再起動の前に。ホストがすぐに切断に気づく）
void usb_disconnect(void);
// 状態の表示（シェルの usb）
void usb_status(void);
