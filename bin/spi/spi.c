// spi: SPI の機器とバイトをやりとりする（Monocs のコマンド。/lib/spi）
//   spi [-c CS] [-s HZ] [-m MODE] [-H] [-N] [-r N] [BYTE...]
//     BYTE を送り、続けて -r の N バイト（0x00 を送る）を受ける。受けたものを全部 0x.. で 1 行に出す
//     -c 0|1   CE0（既定）・CE1          -s HZ    速さ（既定 1000000）
//     -m 0〜3  モード（既定 0）           -H       CS が High でつく
//     -N       CE のピンを使わない        BYTE     16 進（0x9f・9f）
//   例: spi 0x9f -r 3（SPI フラッシュの JEDEC ID）
// Linux の spidev_test・spi-tools とは違う、Monocs のもの。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_spi.h>

static void usage(void)
{
    fprintf(stderr, "Usage: spi [-c CS] [-s HZ] [-m MODE] [-H] [-N] [-r N] [BYTE...]\n"
                    "  sends BYTEs (hex) and N more 0x00 with -r, prints the received bytes\n"
                    "  -c 0|1 chip select (CE0 GPIO8, CE1 GPIO7)   -s HZ speed (default 1000000)\n"
                    "  -m 0-3 SPI mode   -H CS active high   -N do not use the CE pin\n");
}

static bool num(const char *s, long *v, int base)
{
    char *end;
    *v = strtol(s, &end, base);
    return end != s && !*end;
}

int main(int argc, char **argv)
{
    int cs = 0, mode = 0;
    long hz = 1000000, extra = 0, v;
    // オプションとバイトは、どの順に書いてもよい
    uint8_t *tx = calloc((size_t)argc + 65536, 1);
    int n = 0;
    if (!tx) {
        perror("spi");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        if (o[0] != '-' || !o[1]) {
            if (!num(o, &v, 16) || v < 0 || v > 255) {
                fprintf(stderr, "spi: bad byte \"%s\"\n", o);
                return 1;
            }
            tx[n++] = (uint8_t)v;
            continue;
        }
        bool arg = !strcmp(o, "-c") || !strcmp(o, "-s") || !strcmp(o, "-m") || !strcmp(o, "-r");
        if (arg && (i + 1 >= argc || !num(argv[i + 1], &v, 0))) {
            fprintf(stderr, "spi: %s needs a number\n", o);
            return 1;
        }
        if (!strcmp(o, "-c"))
            cs = (int)v;
        else if (!strcmp(o, "-s"))
            hz = v;
        else if (!strcmp(o, "-m")) {
            if (v < 0 || v > 3) {
                fprintf(stderr, "spi: mode must be 0-3\n");
                return 1;
            }
            mode = (mode & ~SPI_MODE_3) | (int)v;
        } else if (!strcmp(o, "-r"))
            extra = v;
        else if (!strcmp(o, "-H"))
            mode |= SPI_CS_HIGH;
        else if (!strcmp(o, "-N"))
            mode |= SPI_NO_CS;
        else if (!strcmp(o, "-h") || !strcmp(o, "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "spi: unknown option %s\n", o);
            usage();
            return 1;
        }
        if (arg)
            i++;
    }
    if (extra < 0 || extra > 65536 || n + extra == 0) {
        usage();
        return 1;
    }
    int total = n + (int)extra;
    uint8_t *rx = malloc((size_t)total);
    if (!rx) {
        perror("spi");
        return 1;
    }
    if (spi_open(cs, (int)hz, mode) < 0) {
        perror("spi: open");
        return 1;
    }
    if (spi_transfer(cs, tx, rx, total) < 0) {
        perror("spi: transfer");
        return 1;
    }
    for (int k = 0; k < total; k++)
        printf("0x%02x%c", rx[k], k == total - 1 ? '\n' : ' ');
    return 0;
}
