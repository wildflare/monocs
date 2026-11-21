// i2cset: I2C の機器のレジスタに書く（i2c-tools の i2cset と同じ書き方・出力。/lib/i2c）
//   i2cset [-y] [-m MASK] [-r] [-a] I2CBUS CHIP-ADDRESS DATA-ADDRESS [VALUE] ... [MODE]
//   MODE: c（DATA-ADDRESS の 1 バイトだけ。VALUE なし）、b（byte data、既定）、w（word data）、i（I2C block data、VALUE を並べる）
//   -m MASK: MASK の立っているビットだけ変える（読んでから書く）。-r: 書いた後に読み直して比べる。
// Linux と違うところ: s（SMBus block）と PEC（p）はない。-f は受け付けるだけ。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_i2c.h>
#include <monocs_i2c_tools.h>

static void usage(void)
{
    fprintf(stderr, "Usage: i2cset [-f] [-y] [-m MASK] [-r] [-a] I2CBUS CHIP-ADDRESS DATA-ADDRESS [VALUE] ... [MODE]\n"
                    "  I2CBUS is 0 (GPIO0/1) or 1 (GPIO2/3)\n"
                    "  ADDRESS is an integer (0x08 - 0x77, or 0x00 - 0x7f if -a is given)\n"
                    "  MODE is one of:\n"
                    "    c (byte, no value)\n"
                    "    b (byte data, default)\n"
                    "    w (word data)\n"
                    "    i (I2C block data)\n");
}

static bool parse_num(const char *s, long *v)
{
    char *end;
    *v = strtol(s, &end, 0);
    return end != s && !*end;
}

int main(int argc, char **argv)
{
    bool yes = false, all = false, readback = false;
    long mask = -1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-m")) {
            if (++i >= argc || !parse_num(argv[i], &mask)) {
                fprintf(stderr, "Error: Data value mask invalid!\n");
                return 1;
            }
            continue;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'y')
                yes = true;
            else if (*o == 'a')
                all = true;
            else if (*o == 'r')
                readback = true;
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
    if (argc - i < 3) {
        usage();
        return 1;
    }
    int bus = i2ct_bus(argv[i++]);
    if (bus < 0)
        return 1;
    int addr = i2ct_addr(argv[i++], all);
    if (addr < 0)
        return 1;
    long daddr;
    if (!parse_num(argv[i++], &daddr) || daddr < 0 || daddr > 0xff) {
        fprintf(stderr, "Error: Data address invalid!\n");
        usage();
        return 1;
    }
    // 終わりが文字なら MODE
    int end = argc, mode;
    const char *last = argv[argc - 1];
    if (end - i > 0 && !(last[0] >= '0' && last[0] <= '9')) {
        if (strlen(last) != 1 || !strchr("cbwi", last[0])) {
            if (last[0] == 's' || (last[0] && last[1] == 'p'))
                fprintf(stderr, "Error: Adapter does not have %s capability\n",
                        last[0] == 's' ? "SMBus block write" : "PEC");
            else {
                fprintf(stderr, "Error: Invalid mode '%s'!\n", last);
                usage();
            }
            return 1;
        }
        mode = last[0];
        end--;
    } else {
        mode = end - i == 0 ? 'c' : 'b';
    }
    int nval = end - i;
    if ((mode == 'c' && nval != 0) || ((mode == 'b' || mode == 'w') && nval != 1) || (mode == 'i' && (nval < 1 || nval > 32))) {
        fprintf(stderr, nval == 0 ? "Error: No data value specified!\n" : "Error: Too many arguments!\n");
        usage();
        return 1;
    }
    long maxv = mode == 'w' ? 0xffff : 0xff;
    uint8_t blk[32];
    long value = 0;
    for (int k = 0; k < nval; k++) {
        long v;
        if (!parse_num(argv[i + k], &v) || v < 0 || v > maxv) {
            fprintf(stderr, "Error: Data value invalid!\n");
            usage();
            return 1;
        }
        blk[k] = (uint8_t)v;
        value = v;
    }
    if (mask != -1 && (mode == 'c' || mode == 'i')) {
        fprintf(stderr, "Error: Mask not supported for this mode!\n");
        return 1;
    }
    if (mask != -1 && (mask < 0 || mask > maxv)) {
        fprintf(stderr, "Error: Data value mask invalid!\n");
        return 1;
    }
    if (!yes) {
        static const char *const names[] = {['c'] = "byte", ['b'] = "byte", ['w'] = "word", ['i'] = "i2c block"};
        fprintf(stderr, "WARNING! This program can confuse your I2C bus, cause data loss and worse!\n");
        fprintf(stderr, "I will write to device file /dev/i2c-%d, chip address 0x%02x,\n", bus, addr);
        if (mode == 'c')
            fprintf(stderr, "data address 0x%02lx, no data.\n", daddr);
        else if (mode == 'i')
            fprintf(stderr, "data address 0x%02lx, %d bytes, mode i2c block.\n", daddr, nval);
        else
            fprintf(stderr, "data address 0x%02lx, data 0x%0*lx%s, mode %s.\n", daddr, mode == 'w' ? 4 : 2, value,
                    mask != -1 ? " (masked)" : "", names[mode]);
        if (!i2ct_confirm(false)) {
            fprintf(stderr, "Aborting on user request.\n");
            return 0;
        }
    }
    uint8_t buf[2];
    if (mask != -1) {
        int n = mode == 'w' ? 2 : 1;
        if (i2c_read_regs(bus, addr, (int)daddr, buf, n) != n) {
            fprintf(stderr, "Error: Failed to read old value\n");
            return 1;
        }
        long old = mode == 'w' ? buf[0] | buf[1] << 8 : buf[0];
        value = (value & mask) | (old & ~mask);
    }
    int r;
    if (mode == 'c') {
        uint8_t d = (uint8_t)daddr;
        r = i2c_write(bus, addr, &d, 1);
    } else if (mode == 'b') {
        r = i2c_write_reg(bus, addr, (int)daddr, (int)value);
    } else if (mode == 'w') {
        buf[0] = (uint8_t)value;
        buf[1] = (uint8_t)(value >> 8);
        r = i2c_write_regs(bus, addr, (int)daddr, buf, 2);
    } else {
        r = i2c_write_regs(bus, addr, (int)daddr, blk, nval);
    }
    if (r < 0) {
        fprintf(stderr, "Error: Write failed\n");
        return 1;
    }
    if (!readback || mode == 'i')
        return 0;
    long got;
    if (mode == 'c') {
        r = i2c_read(bus, addr, buf, 1);
        got = buf[0];
        value = daddr;
    } else {
        int n = mode == 'w' ? 2 : 1;
        r = i2c_read_regs(bus, addr, (int)daddr, buf, n) == n ? 0 : -1;
        got = mode == 'w' ? buf[0] | buf[1] << 8 : buf[0];
    }
    if (r < 0) {
        fprintf(stderr, "Warning - readback failed\n");
        return 1;
    }
    if (got != value) {
        fprintf(stderr, "Warning - data mismatch - wrote 0x%0*lx, read back 0x%0*lx\n", mode == 'w' ? 4 : 2, value,
                mode == 'w' ? 4 : 2, got);
        return 1;
    }
    fprintf(stderr, "Value 0x%0*lx written, readback matched\n", mode == 'w' ? 4 : 2, value);
    return 0;
}
