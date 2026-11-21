// monocs_i2c_tools.h: /bin の i2cdetect・i2cget・i2cset が共有する小さな部品（static の関数だけ。i2c-tools と同じ書き方・文）
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// I2CBUS（0・1、i2c-0・i2c-1、/dev/i2c-1）。ないバスなら Linux と同じ文を出して -1
static int i2ct_bus(const char *s)
{
    const char *p = s;
    if (!strncmp(p, "/dev/", 5))
        p += 5;
    if (!strncmp(p, "i2c-", 4))
        p += 4;
    char *end;
    long n = strtol(p, &end, 0);
    if (end == p || *end || n < 0) {
        fprintf(stderr, "Error: I2C bus name doesn't match any bus present!\n");
        return -1;
    }
    if (n != 0 && n != 1) {
        fprintf(stderr, "Error: Could not open file `/dev/i2c-%ld' or `/dev/i2c/%ld': No such file or directory\n", n, n);
        return -1;
    }
    return (int)n;
}

// CHIP-ADDRESS（0x08〜0x77、-a なら 0x00〜0x7f）
static int i2ct_addr(const char *s, bool all)
{
    char *end;
    long a = strtol(s, &end, 0);
    if (end == s || *end) {
        fprintf(stderr, "Error: Chip address is not a number!\n");
        return -1;
    }
    if (a < (all ? 0 : 0x08) || a > (all ? 0x7f : 0x77)) {
        fprintf(stderr, "Error: Chip address out of range (0x%02x-0x%02x)!\n", all ? 0 : 0x08, all ? 0x7f : 0x77);
        return -1;
    }
    return (int)a;
}

// Continue? [Y/n] の答え。dflt は Enter だけのとき
static bool i2ct_confirm(bool dflt)
{
    fprintf(stderr, "Continue? [%s] ", dflt ? "Y/n" : "y/N");
    fflush(stderr);
    char line[64];
    if (!fgets(line, sizeof line, stdin))
        return false;
    if (line[0] == '\n' || line[0] == 0)
        return dflt;
    return line[0] == 'y' || line[0] == 'Y';
}
