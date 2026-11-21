// SD カードのドライバの中身（os/drivers/sd.c が、どちらかを選ぶ）
#pragma once

#include "sd.h"

// SDHOST（os/drivers/sdhost.c。ふだん使う）
bool sdhost_sd_init(void);
bool sdhost_sd_read(uint32_t sector, uint32_t count, void *buf);
bool sdhost_sd_write(uint32_t sector, uint32_t count, const void *buf);
void sdhost_sd_get_stats(struct sd_stats *st);
void sdhost_sd_release(void);
bool sdhost_set_high_clock(bool on);   // High Speed（50MHz）にする・25MHz に戻す（初めてなら CMD6 でカードも切り替える）
uint32_t sdhost_clock_hz(void);       // 今の SD のクロック（使えなければ 0）   // カードを待機の状態に戻して手放す（EMMC に切り替える前に）
extern uint32_t sdhost_disable_fixes;   // 試すために直しを外す（sdhost.c）
void sdhost_debug(uint32_t *stale, uint32_t *block_to);  // 読み捨てた FIFO の語、ブロックの終わりの待ちの失敗

// EMMC（os/drivers/sd_emmc.c。SDHOST が使えないときの予備。そのときは Wi-Fi の SDIO に EMMC を使えない）
bool emmc_sd_init(void);
bool emmc_sd_read(uint32_t sector, uint32_t count, void *buf);
bool emmc_sd_write(uint32_t sector, uint32_t count, const void *buf);
void emmc_sd_get_stats(struct sd_stats *st);
