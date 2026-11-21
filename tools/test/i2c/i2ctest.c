// /lib/i2c の試験。QEMU では tools/test/i2c.sh が、バス 1 に EEPROM（at24c、0x50、256 バイト）と温度センサー（tmp105、0x48、
// 0x48）をつなぐ。
//   cc -o i2ctest -li2c -lgpio i2ctest.c && ./i2ctest           QEMU（機器ありの試験）
//   cc -o i2ctest -li2c -lgpio i2ctest.c && ./i2ctest empty     実機（何もつないでいないバス 1 で、応答がないこと・バスの速さ・後始末だけ）
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <monocs.h>
#include <monocs_gpio.h>
#include <monocs_i2c.h>

static int fails;

static void check(const char *what, long got, long want)
{
    printf("%s %s: %ld\n", got == want ? "ok" : "NG", what, got);
    if (got != want)
        fails++;
}

// SCL（GPIO3）の立ち下がりを、NACK になるアドレスの読み出し 1 回の間だけ数え、時間からバスの速さを求める（kHz）
static long measure_khz(void)
{
    // アドレスの 8 クロックと ACK の 1 クロック = 9 回の立ち下がり。最初の立ち下がりから最後までの 8 周期を測る
    uint8_t v;
    uint32_t prev = 1;
    int n = 0;
    uint64_t first = 0, last = 0;
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) : : "memory");
    // 読み出しを始めて、終わるまで GPLEV を見続ける（i2c_read は待つ間に戻らないので、ここで自分で回す）
    volatile uint32_t *C = (volatile uint32_t *)0x3F804000UL, *S = C + 1, *DLEN = C + 2, *A = C + 3;
    *C = (1U << 15) | (3U << 4);
    *S = (1U << 9) | (1U << 8) | (1U << 1);
    *A = 0x13;
    *DLEN = 1;
    *C = (1U << 15) | (1U << 7) | 1U;
    uint64_t t0 = timer_now_us();
    while (!(*S & (1U << 1)) && timer_now_us() - t0 < 5000) {
        uint32_t lev = (uint32_t)gpio_read(3);
        if (prev && !lev) {
            uint64_t t = timer_now_us();
            if (n++ == 0)
                first = t;
            last = t;
        }
        prev = lev;
    }
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
    *S = (1U << 9) | (1U << 8) | (1U << 1);
    *C = (1U << 15) | (3U << 4);
    (void)v;
    printf("SCL falling edges: %d\n", n);
    if (n < 9 || last == first)
        return -1;
    return (long)((n - 1) * 1000 / (long)(last - first));
}

int main(int argc, char **argv)
{
    bool empty = argc > 1 && !strcmp(argv[1], "empty");
    uint8_t buf[128], w[64];

    check("bad bus", i2c_open(2, 0), -1);
    check("bad hz", i2c_open(1, 5), -1);
    check("open", i2c_open(1, 100000), 0);
    check("pins ALT0", gpio_get_mode(2) == GPIO_ALT0 && gpio_get_mode(3) == GPIO_ALT0, 1);
    errno = 0;
    check("bad addr", i2c_read(1, 0x80, buf, 1), -1);
    check("errno EINVAL", errno, EINVAL);
    check("write_read wlen 17", i2c_write_read(1, 0x50, w, 17, buf, 1), -1);
    errno = 0;
    check("no device read", i2c_read(1, 0x13, buf, 1), -1);
    check("errno ENXIO", errno, ENXIO);
    errno = 0;
    check("no device write", i2c_write(1, 0x13, "ab", 2), -1);
    check("errno ENXIO (w)", errno, ENXIO);
    errno = 0;
    check("no device write_read", i2c_read_reg(1, 0x13, 0), -1);
    check("errno ENXIO (wr)", errno, ENXIO);
    check("probe 0x13", i2c_probe(1, 0x13), 0);
    if (empty) {
        int found = 0;
        for (int a = 0x08; a <= 0x77; a++)
            found += i2c_probe(1, a) == 1;
        check("nothing on the bus", found, 0);
        long khz = measure_khz();
        check("100kHz (90..105)", khz >= 90 && khz <= 105, 1);
        printf("measured %ld kHz\n", khz);
        i2c_open(1, 400000);
        khz = measure_khz();
        check("400kHz (340..420)", khz >= 340 && khz <= 420, 1);
        printf("measured %ld kHz\n", khz);
        check("still works after", i2c_probe(1, 0x13), 0);
    } else {
        check("probe 0x50", i2c_probe(1, 0x50), 1);
        check("probe 0x48", i2c_probe(1, 0x48), 1);
        // 温度センサー: レジスタ 2（T_LOW）の 2 バイト。上位が先で、電源を入れたときは 75.0℃ = 0x4B 0x00
        check("tmp105 read_regs", i2c_read_regs(1, 0x48, 2, buf, 2), 2);
        check("tmp105 T_LOW", buf[0] << 8 | buf[1], 0x4B00);
        // EEPROM: 番地 0x10 に書いて、リピーテッドスタートで読み直す
        check("write_regs", i2c_write_regs(1, 0x50, 0x10, "hello", 5), 5);
        memset(buf, 0, sizeof buf);
        check("read_regs", i2c_read_regs(1, 0x50, 0x10, buf, 5), 5);
        check("hello", memcmp(buf, "hello", 5), 0);
        check("write_reg", i2c_write_reg(1, 0x50, 0x20, 0x5A), 0);
        check("read_reg", i2c_read_reg(1, 0x50, 0x20), 0x5A);
        // FIFO（16）より長いもの: 40 バイト書いて、100 バイト読む
        for (int k = 0; k < 40; k++)
            w[k] = (uint8_t)(k * 7 + 1);
        check("write 40", i2c_write_regs(1, 0x50, 0x40, w, 40), 40);
        check("read 100", i2c_read_regs(1, 0x50, 0x30, buf, 100), 100);
        check("40 bytes match", memcmp(buf + 0x10, w, 40), 0);
        check("0x20 kept", buf[0] == 0 && i2c_read_reg(1, 0x50, 0x20) == 0x5A, 1);
        // 番地だけ書いて、別の転送で読む（EEPROM は続きから返す）
        uint8_t a = 0x10;
        check("write addr", i2c_write(1, 0x50, &a, 1), 1);
        check("read", i2c_read(1, 0x50, buf, 5), 5);
        check("hello again", memcmp(buf, "hello", 5), 0);
        check("400kHz open", i2c_open(1, 400000), 0);
        check("read at 400kHz", i2c_read_reg(1, 0x50, 0x20), 0x5A);
    }
    check("close", i2c_close(1), 0);
    check("pins input", gpio_get_mode(2) == GPIO_IN && gpio_get_mode(3) == GPIO_IN, 1);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
