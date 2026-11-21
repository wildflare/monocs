// i2cget: I2C の機器のレジスタを読む（i2c-tools の i2cget と同じ書き方・出力。/lib/i2c）
//   i2cget [-y] [-a] I2CBUS CHIP-ADDRESS [DATA-ADDRESS [MODE [LENGTH]]]
//   MODE: b（read byte data、既定）、w（read word data）、c（write byte/read byte）、i（read I2C block data、LENGTH 1〜32）
//   DATA-ADDRESS を省くと、今の位置から 1 バイト読む（receive byte）。
// Linux と違うところ: s（SMBus block）と PEC（p）はない（BCM2835 の I2C ではできない）。-f は受け付けるだけ。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_i2c.h>
#include <monocs_i2c_tools.h>

static void usage(void)
{
    fprintf(stderr, "Usage: i2cget [-f] [-y] [-a] I2CBUS CHIP-ADDRESS [DATA-ADDRESS [MODE [LENGTH]]]\n"
                    "  I2CBUS is 0 (GPIO0/1) or 1 (GPIO2/3)\n"
                    "  ADDRESS is an integer (0x08 - 0x77, or 0x00 - 0x7f if -a is given)\n"
                    "  MODE is one of:\n"
                    "    b (read byte data, default)\n"
                    "    w (read word data)\n"
                    "    c (write byte/read byte)\n"
                    "    i (read I2C block data)\n"
                    "  LENGTH is the I2C block data length (between 1 and 32, default 32)\n");
}

int main(int argc, char **argv)
{
    bool yes = false, all = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'y')
                yes = true;
            else if (*o == 'a')
                all = true;
            else if (*o == 'f')
                ;
            else if (*o == 'h') {
                usage();
                return 0;
            } else {
                fprintf(stderr, "Error: Unsupported option \"-%c\"!\n", *o);
                usage();
                return 1;
            }
        }
    }
    if (argc - i < 2) {
        usage();
        return 1;
    }
    int bus = i2ct_bus(argv[i++]);
    if (bus < 0)
        return 1;
    int addr = i2ct_addr(argv[i++], all);
    if (addr < 0)
        return 1;
    int daddr = -1, mode = 'b', len = 32;
    if (i < argc) {
        char *end;
        long d = strtol(argv[i++], &end, 0);
        if (*end || d < 0 || d > 0xff) {
            fprintf(stderr, "Error: Data address invalid!\n");
            usage();
            return 1;
        }
        daddr = (int)d;
    }
    if (i < argc) {
        const char *m = argv[i++];
        if (strlen(m) != 1 || !strchr("bwci", m[0])) {
            if (m[0] == 's' || (m[0] && m[1] == 'p'))
                fprintf(stderr, "Error: Adapter does not have %s capability\n",
                        m[0] == 's' ? "SMBus block read" : "PEC");
            else {
                fprintf(stderr, "Error: Invalid mode!\n");
                usage();
            }
            return 1;
        }
        mode = m[0];
    }
    if (i < argc) {
        if (mode != 'i') {
            fprintf(stderr, "Error: Length only valid for I2C block data!\n");
            usage();
            return 1;
        }
        char *end;
        long l = strtol(argv[i++], &end, 0);
        if (*end || l < 1 || l > 32) {
            fprintf(stderr, "Error: Length invalid!\n");
            usage();
            return 1;
        }
        len = (int)l;
    }
    if (!yes) {
        static const char *const names[] = {['b'] = "read byte data", ['w'] = "read word data",
                                            ['c'] = "write byte/read byte", ['i'] = "read I2C block data"};
        fprintf(stderr, "WARNING! This program can confuse your I2C bus, cause data loss and worse!\n");
        if (daddr < 0)
            fprintf(stderr, "I will read from device file /dev/i2c-%d, chip address 0x%02x, current data\n"
                            "address, using read byte.\n", bus, addr);
        else
            fprintf(stderr, "I will read from device file /dev/i2c-%d, chip address 0x%02x, data address\n"
                            "0x%02x, using %s.\n", bus, addr, daddr, names[mode]);
        if (!i2ct_confirm(true)) {
            fprintf(stderr, "Aborting on user request.\n");
            return 0;
        }
    }
    uint8_t buf[32];
    int r;
    if (daddr < 0) {
        r = i2c_read(bus, addr, buf, 1);
        if (r == 1)
            printf("0x%02x\n", buf[0]);
    } else if (mode == 'b') {
        r = i2c_read_regs(bus, addr, daddr, buf, 1);
        if (r == 1)
            printf("0x%02x\n", buf[0]);
    } else if (mode == 'w') {
        r = i2c_read_regs(bus, addr, daddr, buf, 2);   // SMBus の word は下位のバイトが先
        if (r == 2)
            printf("0x%04x\n", buf[0] | buf[1] << 8);
    } else if (mode == 'c') {
        uint8_t d = (uint8_t)daddr;
        r = i2c_write(bus, addr, &d, 1);
        if (r == 1)
            r = i2c_read(bus, addr, buf, 1);
        if (r == 1)
            printf("0x%02x\n", buf[0]);
    } else {
        r = i2c_read_regs(bus, addr, daddr, buf, len);
        if (r == len)
            for (int k = 0; k < len; k++)
                printf("0x%02x%c", buf[k], k == len - 1 ? '\n' : ' ');
    }
    if (r < 0) {
        fprintf(stderr, "Error: Read failed\n");
        return 2;
    }
    return 0;
}
