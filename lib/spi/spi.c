// spi.c: SPI のマスター（monocs_spi.h）。共有のライブラリ /lib/spi
// 出典: BCM2835 ARM Peripherals 10 章（SPI）、Linux の drivers/spi/spi-bcm2835.c
#include "monocs_spi.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <monocs.h>
#include <monocs_gpio.h>

#define SPI0_BASE   0x3F204000UL    // バスアドレス 0x7E204000

// レジスタ（10.5）
#define SPI_CS      0x00
#define SPI_FIFO    0x04
#define SPI_CLK     0x08

#define CS_CPHA     (1U << 2)
#define CS_CPOL     (1U << 3)
#define CS_CLEAR    (3U << 4)       // 送る・受ける FIFO を空に
#define CS_CSPOL    (1U << 6)
#define CS_TA       (1U << 7)
#define CS_DONE     (1U << 16)
#define CS_RXD      (1U << 17)      // 受けた FIFO にデータがある
#define CS_TXD      (1U << 18)      // 送る FIFO に空きがある
#define CS_CSPOL0   (1U << 21)      // CE0 の極性（1 で High がつく）。CE1 は 22

#define FIFO_BYTES  16              // 先に送ってよい数（受ける FIFO をあふれさせない）

// コアのクロック（config.txt の force_turbo=1 で最大の 400MHz に固定。clock コマンドの CORE）
#define CORE_HZ     400000000U

#define PIN_CE1     7
#define PIN_CE0     8
#define PIN_MISO    9
#define PIN_MOSI    10
#define PIN_SCLK    11

typedef struct {
    int open;       // このアプリの中で開いたか
    int mode;
    uint32_t cdiv;
} Dev;

static Dev devs[2];

static inline volatile uint32_t *reg(unsigned off)
{
    return (volatile uint32_t *)(SPI0_BASE + off);
}

int spi_open(int cs, int hz, int mode)
{
    if (cs != 0 && cs != 1) {
        errno = EINVAL;
        return -1;
    }
    if (hz == 0)
        hz = 1000000;
    if (hz < SPI_HZ_MIN || hz > SPI_HZ_MAX || (mode & ~(SPI_MODE_3 | SPI_CS_HIGH | SPI_NO_CS))) {
        errno = EINVAL;
        return -1;
    }
    // Linux の bcm2835_spi_transfer_one と同じ: 切り上げて偶数に（2 から 65534。0 は 65536 で一番遅い）
    uint32_t cdiv;
    if ((uint32_t)hz >= CORE_HZ / 2) {
        cdiv = 2;
    } else {
        cdiv = (CORE_HZ + (uint32_t)hz - 1) / (uint32_t)hz;
        cdiv += cdiv % 2;
        if (cdiv >= 65536)
            cdiv = 0;
    }
    Dev *d = &devs[cs];
    d->mode = mode;
    d->cdiv = cdiv;
    d->open = 1;
    // 止めたまま、この CE の極性を先に決めておく（ピンを SPI にしたときに、つかないように）
    uint32_t c = *reg(SPI_CS) & (CS_CSPOL0 | CS_CSPOL0 << 1);
    c &= ~(CS_CSPOL0 << cs);
    if (mode & SPI_CS_HIGH)
        c |= CS_CSPOL0 << cs;
    *reg(SPI_CS) = c | CS_CLEAR;
    if (gpio_mode(PIN_SCLK, GPIO_ALT0) < 0 || gpio_mode(PIN_MOSI, GPIO_ALT0) < 0 ||
        gpio_mode(PIN_MISO, GPIO_ALT0) < 0)
        return -1;
    if (!(mode & SPI_NO_CS) && gpio_mode(cs ? PIN_CE1 : PIN_CE0, GPIO_ALT0) < 0)
        return -1;
    return 0;
}

int spi_close(int cs)
{
    if (cs != 0 && cs != 1) {
        errno = EINVAL;
        return -1;
    }
    Dev *d = &devs[cs];
    if (d->open && !(d->mode & SPI_NO_CS))
        gpio_mode(cs ? PIN_CE1 : PIN_CE0, GPIO_IN);
    d->open = 0;
    if (!devs[!cs].open) {
        gpio_mode(PIN_SCLK, GPIO_IN);
        gpio_mode(PIN_MOSI, GPIO_IN);
        gpio_mode(PIN_MISO, GPIO_IN);
    }
    return 0;
}

int spi_get_hz(int cs)
{
    if ((cs != 0 && cs != 1) || !devs[cs].open) {
        errno = EINVAL;
        return -1;
    }
    return (int)(CORE_HZ / (devs[cs].cdiv ? devs[cs].cdiv : 65536));
}

// total バイトをやりとりする。k バイト目は、k < txn なら tx[k]（tx が NULL なら 0）を送り、受けたものは
// k >= rxskip なら rx[k - rxskip] に入れる（rx が NULL なら捨てる）。10.6.1 のポーリングの手順
static int xfer(int cs, const uint8_t *tx, int txn, uint8_t *rx, int rxskip, int total)
{
    if (cs != 0 && cs != 1) {
        errno = EINVAL;
        return -1;
    }
    Dev *d = &devs[cs];
    if (!d->open && spi_open(cs, 0, SPI_MODE_0) < 0)
        return -1;
    // CE の極性は両方の cs のものを残す（つかっていない側がつかないように）
    uint32_t c = (*reg(SPI_CS) & (CS_CSPOL0 | CS_CSPOL0 << 1)) | (uint32_t)cs;
    if (d->mode & SPI_CPHA)
        c |= CS_CPHA;
    if (d->mode & SPI_CPOL)
        c |= CS_CPOL;
    if (d->mode & SPI_CS_HIGH)
        c |= CS_CSPOL;
    *reg(SPI_CLK) = d->cdiv;
    *reg(SPI_CS) = c | CS_CLEAR;    // TA の前に CPOL を決める（SCLK の休みの値を先に）
    *reg(SPI_CS) = c | CS_TA;
    uint32_t div = d->cdiv ? d->cdiv : 65536;
    uint64_t limit = (uint64_t)total * 8 * 2 * div / (CORE_HZ / 1000000) + 50000;
    uint64_t t0 = timer_now_us();
    int ti = 0, ri = 0;
    while (ri < total) {
        while (ti < total && ti - ri < FIFO_BYTES && (*reg(SPI_CS) & CS_TXD)) {
            *reg(SPI_FIFO) = ti < txn && tx ? tx[ti] : 0;
            ti++;
        }
        while (ri < total && (*reg(SPI_CS) & CS_RXD)) {
            uint8_t v = (uint8_t)*reg(SPI_FIFO);
            if (rx && ri >= rxskip)
                rx[ri - rxskip] = v;
            ri++;
        }
        if (timer_now_us() - t0 > limit)
            break;
    }
    while (ri == total && !(*reg(SPI_CS) & CS_DONE) && timer_now_us() - t0 <= limit)
        ;
    int ok = ri == total && (*reg(SPI_CS) & CS_DONE);
    *reg(SPI_CS) = c | CS_CLEAR;    // TA を外す（CS が離れる）
    if (!ok) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int check_len(int len, int max)
{
    if (len < 0 || len > max) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int spi_transfer(int cs, const void *tx, void *rx, int len)
{
    if (check_len(len, 0x7FFFFFFF) < 0 || xfer(cs, tx, len, rx, 0, len) < 0)
        return -1;
    return len;
}

int spi_write(int cs, const void *tx, int len)
{
    return spi_transfer(cs, tx, nullptr, len);
}

int spi_read(int cs, void *rx, int len)
{
    return spi_transfer(cs, nullptr, rx, len);
}

int spi_write_read(int cs, const void *tx, int txlen, void *rx, int rxlen)
{
    if (check_len(txlen, 0x3FFFFFFF) < 0 || check_len(rxlen, 0x3FFFFFFF) < 0)
        return -1;
    if (xfer(cs, tx, txlen, rx, txlen, txlen + rxlen) < 0)
        return -1;
    return rxlen;
}
