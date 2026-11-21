// SDIO（内蔵 Wi-Fi のチップとつなぐ。EMMC コントローラ、GPIO34〜39。os/drivers/sdio.c）
#pragma once

#include <stdint.h>

bool sdio_init(void);               // WL_ON でチップをリセットし直し、SDIO の初期化（4 ビット、25MHz）
const char *sdio_error(void);       // 失敗したところ
uint32_t sdio_ocr(void);            // CMD5 の応答（機能の数など）

// CMD52（1 バイト）。out は NULL 可（書くときに out を渡すと、書いた後の値を読み返す）
bool sdio_rw_direct(bool write, int fn, uint32_t addr, uint8_t in, uint8_t *out);
// CMD53。len はその機能のブロックの大きさ以下（バイトモード）か、その倍数（ブロックモード）。4 の倍数
bool sdio_rw_extended(bool write, int fn, uint32_t addr, bool incr, void *buf, uint32_t len);
bool sdio_set_block_size(int fn, uint32_t size);
bool sdio_enable_function(int fn, bool on, uint32_t timeout_us);

// カードの割り込み（チップが「読むものがある」と DAT1 で知らせる。SDIO 仕様 6.9 節の CCCR の Int Enable、8.1 節）
bool sdio_card_irq_setup(void);         // チップの側（CCCR）で、機能 1・2 と全体の割り込みを有効にする
void sdio_card_irq_signal(bool on);     // コントローラが割り込みを上げるか（EMMC の IRPT_EN の CARD）
void sdio_card_irq_off(void);            // 割り込みコントローラでも止める（上がり続けるとき）
bool sdio_card_irq_pending(void);       // 今、チップが割り込みを上げているか（EMMC の INTERRUPT の CARD）
