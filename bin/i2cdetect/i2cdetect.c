// i2cdetect: I2C のバスの上の機器を探す（i2c-tools の i2cdetect と同じ書き方・出力。/lib/i2c）
//   i2cdetect [-y] [-a] [-r] I2CBUS [FIRST LAST]
//   i2cdetect -l
// I2CBUS は 1（GPIO2・3）か 0（GPIO0・1）。
// Linux と違うところ: どのアドレスも 1 バイト読んで調べる（-r と同じ。BCM2835 の I2C で長さ 0 の書き込み（quick write）が
// 正しく出るかを確かめていないため。-q は断る）。-F はない。UU（ドライバが使っている）は出ない。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_i2c.h>
#include <monocs_i2c_tools.h>

static void usage(void)
{
    fprintf(stderr, "Usage: i2cdetect [-y] [-a] [-r] I2CBUS [FIRST LAST]\n"
                    "       i2cdetect -l\n"
                    "  I2CBUS is 0 (GPIO0/1) or 1 (GPIO2/3)\n"
                    "  If provided, FIRST and LAST limit the probing range.\n");
}

int main(int argc, char **argv)
{
    bool yes = false, all = false, list = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'y': yes = true; break;
            case 'a': all = true; break;
            case 'r': break;
            case 'l': list = true; break;
            case 'q':
                fprintf(stderr, "Error: quick write is not supported on Monocs (use -r)\n");
                return 1;
            case 'h': usage(); return 0;
            default:
                fprintf(stderr, "Error: Unsupported option \"-%c\"!\n", *o);
                usage();
                return 1;
            }
        }
    }
    if (list) {
        printf("i2c-0\ti2c       \t3f205000.i2c (GPIO0/1)          \tI2C adapter\n");
        printf("i2c-1\ti2c       \t3f804000.i2c (GPIO2/3)          \tI2C adapter\n");
        return 0;
    }
    if (i >= argc) {
        fprintf(stderr, "Error: No i2c-bus specified!\n");
        usage();
        return 1;
    }
    int bus = i2ct_bus(argv[i++]);
    if (bus < 0)
        return 1;
    int first = all ? 0x00 : 0x08, last = all ? 0x7f : 0x77;
    if (i < argc) {
        char *end;
        long f = strtol(argv[i++], &end, 0);
        if (*end || f < first || f > last) {
            fprintf(stderr, "Error: FIRST argument out of range!\n");
            return 1;
        }
        first = (int)f;
        if (i >= argc) {
            fprintf(stderr, "Error: LAST argument missing!\n");
            return 1;
        }
        long l = strtol(argv[i++], &end, 0);
        if (*end || l < first || l > last) {
            fprintf(stderr, "Error: LAST argument out of range!\n");
            return 1;
        }
        last = (int)l;
    }
    if (!yes) {
        fprintf(stderr, "WARNING! This program can confuse your I2C bus, cause data loss and worse!\n");
        fprintf(stderr, "I will probe file /dev/i2c-%d using receive byte commands.\n", bus);
        fprintf(stderr, "I will probe address range 0x%02x-0x%02x.\n", first, last);
        if (!i2ct_confirm(true)) {
            fprintf(stderr, "Aborting on user request.\n");
            return 0;
        }
    }
    if (i2c_open(bus, 0) < 0) {
        perror("Error: i2c_open");
        return 1;
    }
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int row = 0; row < 128; row += 16) {
        printf("%02x: ", row);
        for (int a = row; a < row + 16; a++) {
            if (a < first || a > last) {
                printf("   ");
                continue;
            }
            int r = i2c_probe(bus, a);
            if (r < 0) {
                printf("\n");
                perror("Error: probe");
                return 1;
            }
            if (r)
                printf("%02x ", a);
            else
                printf("-- ");
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
