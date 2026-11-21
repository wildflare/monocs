// SD カード: SDHOST（os/drivers/sdhost.c）を使い、初期化に失敗したら EMMC（os/drivers/sd_emmc.c）を使う
// （2026-11-12。内蔵 Wi-Fi の SDIO に EMMC を空けるため、SD カードを SDHOST に移した。新しいドライバに不具合が
// あっても起動できるように、前の EMMC のドライバを予備に残す。予備で動いているときは、Wi-Fi は使えない）

#include "sd.h"
#include "sd_backend.h"
#include "uart.h"

static enum { SD_NONE, SD_SDHOST, SD_EMMC } backend;

// 起動で SDHOST を使うか。0 にすると EMMC で起動し、ファイルシステムを SDHOST で読み書きしない（SDHOST の
// ドライバを直すときは、0 にして sdtest で確かめてから 1 に戻す。2026-11-13: SDHOST の最初の版で、実機の FAT を
// 壊した。sdtest で原因を見つけて直した）
#define SD_BOOT_SDHOST 1

bool sd_init(void)
{
    if (!SD_BOOT_SDHOST && emmc_sd_init()) {
        backend = SD_EMMC;
        return true;
    }
    if (sdhost_sd_init()) {
        backend = SD_SDHOST;
        return true;
    }
    uart_puts("sd: SDHOST failed, using EMMC (Wi-Fi is not available)\n");
    if (emmc_sd_init()) {
        backend = SD_EMMC;
        return true;
    }
    backend = SD_NONE;
    return false;
}

const char *sd_controller(void)
{
    return backend == SD_SDHOST ? "SDHOST" : backend == SD_EMMC ? "EMMC" : "none";
}

bool sd_emmc_free(void)
{
    return backend != SD_EMMC;
}

bool sd_read(uint32_t sector, uint32_t count, void *buf)
{
    if (backend == SD_SDHOST)
        return sdhost_sd_read(sector, count, buf);
    if (backend == SD_EMMC)
        return emmc_sd_read(sector, count, buf);
    return false;
}

bool sd_write(uint32_t sector, uint32_t count, const void *buf)
{
    if (backend == SD_SDHOST)
        return sdhost_sd_write(sector, count, buf);
    if (backend == SD_EMMC)
        return emmc_sd_write(sector, count, buf);
    return false;
}

void sd_get_stats(struct sd_stats *st)
{
    if (backend == SD_EMMC)
        emmc_sd_get_stats(st);
    else
        sdhost_sd_get_stats(st);
}
