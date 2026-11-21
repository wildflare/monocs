// /lib/spi の試験。
//   cc -o spitest -lspi -lgpio spitest.c && ./spitest        QEMU（tools/test/spi.sh が SPI フラッシュ w25q32 をつなぐ。最初に JEDEC ID を読む）
//   cc -o spitest -lspi -lgpio spitest.c && ./spitest hw     実機（何もつながない）。コア 1 が SCLK（GPIO11）・MOSI（10）・CE0（8）の値を読み続け、
//                                     波形から送ったバイト・モード・CS の極性・速さを確かめる
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <monocs.h>
#include <monocs_gpio.h>
#include <monocs_spi.h>

#define SPI_CS_REG (*(volatile uint32_t *)0x3F204000UL)

static int fails;

static void check(const char *what, long got, long want)
{
    printf("%s %s: %ld\n", got == want ? "ok" : "NG", what, got);
    if (got != want)
        fails++;
}

// コア 1 の記録: ピンの値が変わるたびに（値、時刻）
#define NREC 4000
static uint32_t rec_v[NREC];
static uint64_t rec_t[NREC];
static volatile int nrec, sampling, sampler_ready;

static void sampler(void *arg)
{
    (void)arg;
    const uint32_t mask = (1U << 8) | (1U << 10) | (1U << 11);
    uint32_t prev = gpio_read_all() & mask;
    int n = 0;
    rec_v[n] = prev;
    rec_t[n++] = timer_now_us();
    sampler_ready = 1;
    while (sampling) {
        uint32_t v = gpio_read_all() & mask;
        if (v != prev && n < NREC) {
            rec_v[n] = v;
            rec_t[n++] = timer_now_us();
            prev = v;
        }
    }
    nrec = n;
}

// mode で hz の 2 バイトを送り、波形を確かめる
static void wave(int mode, int hz)
{
    char name[64];
    uint8_t tx[2] = {0xA5, 0x3C}, rx[2] = {0xEE, 0xEE};
    bool cpol = mode & SPI_CPOL, cpha = mode & SPI_CPHA, cshigh = mode & SPI_CS_HIGH;
    spi_open(0, hz, mode);
    spi_write(0, "", 0);        // 一度動かして、SCLK・CE を休みの値に
    timer_delay_us(100);
    sampling = 1;
    sampler_ready = 0;
    monocs_core_start(1, sampler, nullptr);
    while (!sampler_ready)
        ;
    timer_delay_us(50);
    int r = spi_transfer(0, tx, rx, 2);
    timer_delay_us(50);
    sampling = 0;
    monocs_core_wait(1);
    snprintf(name, sizeof name, "mode %d%s %dHz", mode & 3, cshigh ? " cs-high" : "", hz);
    printf("-- %s (%d changes)\n", name, nrec);
    check("transfer", r, 2);
    check("MISO reads 0 (pull-down)", rx[0] | rx[1], 0);
    bool sclk0 = rec_v[0] >> 11 & 1, ce0 = rec_v[0] >> 8 & 1;
    check("SCLK idle", sclk0, cpol);
    check("CE0 idle", ce0, !cshigh);
    // CE がついている間に、サンプルする端（CPOL == CPHA なら立ち上がり、ほかは立ち下がり）で MOSI を読む
    bool rising = cpol == cpha;
    uint32_t bits = 0;
    int nb = 0;
    uint64_t first = 0, last = 0;
    bool ce_seen = false;
    for (int k = 1; k < nrec; k++) {
        bool s0 = rec_v[k - 1] >> 11 & 1, s1 = rec_v[k] >> 11 & 1;
        bool active = (rec_v[k] >> 8 & 1) != !cshigh;
        if (active)
            ce_seen = true;
        if (active && s0 != s1 && s1 == rising) {
            bits = bits << 1 | (rec_v[k] >> 10 & 1);
            if (nb++ == 0)
                first = rec_t[k];
            last = rec_t[k];
        }
    }
    check("CE asserted", ce_seen, 1);
    check("16 clocks", nb, 16);
    check("MOSI bytes", (long)bits, 0xA53C);
    long khz = nb > 1 && last > first ? (long)((nb - 1) * 1000 / (long)(last - first)) : -1;
    printf("measured %ld kHz\n", khz);
    check("speed within 10%", khz * 1000 >= hz * 9 / 10 && khz * 1000 <= hz * 11 / 10, 1);
    bool sclk_end = rec_v[nrec - 1] >> 11 & 1, ce_end = rec_v[nrec - 1] >> 8 & 1;
    check("SCLK idle after", sclk_end, cpol);
    check("CE0 released", ce_end, !cshigh);
}

int main(int argc, char **argv)
{
    bool hw = argc > 1 && !strcmp(argv[1], "hw");
    uint8_t id[3] = {0}, buf[1000];

    if (!hw) {
        // 最初に JEDEC ID（w25q32: Winbond 0xEF・0x40・0x16）。QEMU は CS を動かさないので、フラッシュには最初の命令だけが通じる
        check("write_read", spi_write_read(0, (uint8_t[]){0x9F}, 1, id, 3), 3);
        check("JEDEC id", id[0] << 16 | id[1] << 8 | id[2], 0xEF4016);
    }
    errno = 0;
    check("bad cs", spi_open(2, 0, 0), -1);
    check("errno EINVAL", errno, EINVAL);
    check("bad hz", spi_open(0, 1000, 0), -1);
    check("LSB first", spi_open(0, 0, SPI_LSB_FIRST), -1);
    check("open", spi_open(0, 0, SPI_MODE_0), 0);
    check("1MHz", spi_get_hz(0), 1000000);
    check("pins ALT0", gpio_get_mode(8) == GPIO_ALT0 && gpio_get_mode(9) == GPIO_ALT0 && gpio_get_mode(10) == GPIO_ALT0 &&
                           gpio_get_mode(11) == GPIO_ALT0, 1);
    check("CE1 untouched", gpio_get_mode(7), GPIO_IN);
    check("open 3MHz", spi_open(0, 3000000, SPI_MODE_3 | SPI_CS_HIGH), 0);
    check("3MHz = 400M/134", spi_get_hz(0), 400000000 / 134);
    check("transfer 1000", spi_transfer(0, nullptr, buf, 1000), 1000);
    uint32_t c = SPI_CS_REG;
    check("CS reg: CPOL CPHA CSPOL0, TA off", (long)(c & ((1U << 2) | (1U << 3) | (1U << 7) | (1U << 21))),
          (long)((1U << 2) | (1U << 3) | (1U << 21)));
    check("NO_CS on CE1", spi_open(1, 0, SPI_NO_CS), 0);
    check("CE1 still GPIO", gpio_get_mode(7), GPIO_IN);
    check("write cs1", spi_write(1, "abc", 3), 3);
    check("CS reg: cs1 selected, CSPOL0 kept", (long)(SPI_CS_REG & (3U | (1U << 21) | (1U << 22))), (long)(1U | (1U << 21)));
    check("read", spi_read(0, buf, 10), 10);
    check("write_read 0", spi_write_read(0, "x", 1, buf, 0), 0);
    check("close 1 keeps bus", (spi_close(1), gpio_get_mode(11)), GPIO_ALT0);
    if (hw) {
        wave(SPI_MODE_0, 100000);
        wave(SPI_MODE_1, 100000);
        wave(SPI_MODE_2, 100000);
        wave(SPI_MODE_3, 100000);
        wave(SPI_MODE_0 | SPI_CS_HIGH, 100000);
        wave(SPI_MODE_0, 500000);
        spi_open(0, 0, SPI_MODE_0);     // CE0 の極性を元（Low でつく）に戻してから閉じる
    }
    check("close", spi_close(0), 0);
    check("pins input", gpio_get_mode(8) == GPIO_IN && gpio_get_mode(9) == GPIO_IN && gpio_get_mode(10) == GPIO_IN &&
                            gpio_get_mode(11) == GPIO_IN, 1);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
