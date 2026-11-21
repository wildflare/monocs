// メールボックスのプロパティインターフェース（チャンネル 8）
#pragma once

#include <stddef.h>
#include <stdint.h>

// buf はメッセージ全体（先頭はバッファサイズ）。64 バイト境界に置くこと。
// 成功すると true を返し、buf に応答が書き戻される。
bool mailbox_call(volatile uint32_t *buf);

// クロック ID（Mailbox-property-interface の Clocks 節）
#define MBOX_CLOCK_EMMC     1
#define MBOX_CLOCK_ARM      3
#define MBOX_CLOCK_CORE     4

// タグ（Mailbox-property-interface。GET_THROTTLED は Linux の include/soc/bcm2835/raspberrypi-firmware.h）
#define MBOX_TAG_GET_CLOCK_RATE          0x00030002
#define MBOX_TAG_GET_MAX_CLOCK_RATE      0x00030004
#define MBOX_TAG_GET_TEMPERATURE         0x00030006
#define MBOX_TAG_GET_MIN_CLOCK_RATE      0x00030007
#define MBOX_TAG_GET_MAX_TEMPERATURE     0x0003000A
#define MBOX_TAG_GET_THROTTLED           0x00030046
#define MBOX_TAG_GET_CLOCK_RATE_MEASURED 0x00030047
#define MBOX_TAG_GET_VOLTAGE             0x00030003     // 値は µV。番号 1 = コア（ARM・VPU）
#define MBOX_VOLTAGE_CORE   1

// 値を 1 つ渡して 1 つ受け取るタグ（id と応答の 2 語）。失敗すると false
bool mailbox_get_value(uint32_t tag, uint32_t id, uint32_t *value);

// クロック周波数（Hz）を得る。失敗すると 0
uint32_t mailbox_get_clock_rate(uint32_t clock_id);
uint32_t mailbox_set_clock_rate(uint32_t clock_id, uint32_t hz, bool skip_turbo);   // 実際の値（Hz）。0 は失敗
bool mailbox_set_reboot_flags(uint32_t flags);
void mailbox_get_cmdline(char *out, size_t size);   // カーネルのコマンドライン（config.txt の cmdline=）  // 次の再起動の印（1 = tryboot）。ファームウェアが受け付けたら true

// SDHOST の SD のクロックを hz にしてもらう（Set SDHOST clock、0x00038042。出典: Linux の
// include/soc/bcm2835/raspberrypi-firmware.h、drivers/mmc/host/bcm2835.c の bcm2835_set_clock）。
// ファームウェアが分周（SDCDIV）を書き、コアのクロックが変わっても合わせ直す。成功なら実際のクロック（Hz）、
// 失敗（QEMU など、このタグがない）なら 0
uint32_t mailbox_set_sdhost_clock(uint32_t hz);

// 周辺機器の電源を入れる・切る（Set power state、0x00028001。完了を待つ）。state に応答の状態
bool mailbox_set_power(uint32_t device_id, bool on, uint32_t *state);

// ARM が使える RAM の範囲を得る
bool mailbox_get_arm_memory(uint32_t *base, uint32_t *size);
