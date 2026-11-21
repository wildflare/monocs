// SD カード（SDHOST コントローラ。だめなら EMMC。ポーリング、PIO。os/drivers/sd.c）
#pragma once

#include <stdint.h>

#define SD_SECTOR_SIZE  512

bool sd_init(void);
bool sd_read(uint32_t sector, uint32_t count, void *buf);
bool sd_write(uint32_t sector, uint32_t count, const void *buf);

// 読み書きの回数（コマンド）・セクタ数・かかった時間（シェルの mem で表示する）
struct sd_stats {
    uint64_t reads, read_sectors, read_us;
    uint64_t writes, write_sectors, write_us;
    // 内訳（読み書きの合計）: コマンドを出して応答が来るまで、ブロックの準備を待つ間、
    // DATA レジスタとの受け渡し（PIO）、最後のブロックの後に転送の完了を待つ間
    uint64_t cmd_us, ready_us, pio_us, done_us;
    uint64_t read_errors, write_errors;
};
void sd_get_stats(struct sd_stats *st);

const char *sd_controller(void);    // 使っているコントローラ（"SDHOST"・"EMMC"）
bool sd_emmc_free(void);            // EMMC が空いているか（Wi-Fi の SDIO に使える）

// SDHOST を区画の外の空いているセクタで試す（シェルの sdtest。EMMC で動いているときだけ。os/drivers/sd_test.c）
// SD カードのほかの読み書きが起きないように、vfs_exclusive の中で呼ぶ。0 で成功
int sd_selftest(int rounds);
int sd_selftest_hs(int rounds);    // High Speed（50MHz）を SDHOST のまま、区画の最後の空きクラスタで試す（sdtest hs）
