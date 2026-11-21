// /lib/softuart の試験（QEMU と実機。線はいらない）。TX と RX を同じピン（既定 GPIO5）にして、送ったものを裏のコアで受ける。
//   cc -o sutest -lsoftuart -lgpio sutest.c && ./sutest [PIN] [hw]
// hw なし（QEMU）は使い方だけ（受けたものの正しさは見ない）。hw を付けると 9600・38400・115200bps で 256 種類の値を
// 送って受け、送る時間（ビットの長さ）も確かめる（実機）。
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs.h>
#include <monocs_gpio.h>
#include <monocs_softuart.h>

static int fails;

static void check(const char *what, long got, long want)
{
    printf("%s %s: %ld\n", got == want ? "ok" : "NG", what, got);
    if (got != want)
        fails++;
}

static void loop_test(int pin, int baud, bool timing)
{
    char name[64];
    uint8_t tx[256], rx[300];
    for (int k = 0; k < 256; k++)
        tx[k] = (uint8_t)k;
    int u = softuart_open(pin, pin, baud);
    snprintf(name, sizeof name, "%d open", baud);
    check(name, u >= 0, 1);
    snprintf(name, sizeof name, "%d rx in background", baud);
    check(name, softuart_rx_background(u), 1);
    timer_delay_us(2000);
    uint64_t t0 = timer_now_us();
    softuart_write(u, tx, 256);
    uint64_t us = timer_now_us() - t0;
    int n = 0, r;
    while (n < 256 && (r = softuart_read(u, rx + n, (int)sizeof rx - n, 200)) > 0)
        n += r;
    snprintf(name, sizeof name, "%d received 256", baud);
    check(name, n, 256);
    snprintf(name, sizeof name, "%d all 256 values", baud);
    check(name, n == 256 && memcmp(tx, rx, 256) == 0, 1);
    snprintf(name, sizeof name, "%d no errors", baud);
    check(name, (long)softuart_errors(u), 0);
    if (timing) {
        long want = 256L * 10 * 1000000 / baud;
        printf("%d: 256 bytes in %llu us (want %ld)\n", baud, (unsigned long long)us, want);
        snprintf(name, sizeof name, "%d bit time within 1%%", baud);
        check(name, (long)us >= want * 99 / 100 && (long)us <= want * 101 / 100, 1);
    }
    softuart_close(u);
}

static void spin(void *arg)
{
    volatile int *stop = arg;
    while (!*stop)
        ;
}

int main(int argc, char **argv)
{
    int pin = 5;
    bool hw = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "hw"))
            hw = true;
        else
            pin = atoi(argv[i]);
    }
    errno = 0;
    check("bad baud", softuart_open(pin, pin, 100), -1);
    check("errno EINVAL", errno, EINVAL);
    check("console pin", softuart_open(14, -1, 9600), -1);
    check("no pins", softuart_open(-1, -1, 9600), -1);
    check("bad handle", softuart_write(3, "a", 1), -1);

    if (hw) {
        loop_test(pin, 9600, true);
        loop_test(pin, 38400, true);
        loop_test(pin, 115200, true);
    } else {
        // QEMU は仮想の CPU を交互に動かすので、2 つのコアの時間が合わず、1200bps でも受信が崩れる。開く・閉じるだけ
        int u = softuart_open(pin, pin, 9600);
        check("open (qemu)", u >= 0, 1);
        check("rx in background (qemu)", softuart_rx_background(u), 1);
        check("write (qemu)", softuart_write(u, "abc", 3), 3);
        softuart_close(u);
    }

    // ブレーク（Low が 1 バイトより長い）はフレームの誤りに数える
    int u = softuart_open(pin, pin, 9600);
    timer_delay_us(2000);
    gpio_write(pin, 0);
    timer_delay_us(3000);
    gpio_write(pin, 1);
    timer_delay_us(3000);
    check("break counted", softuart_errors(u) >= 1, 1);
    while (softuart_available(u) > 0)
        softuart_getc(u, 0);
    errno = 0;
    check("getc timeout", softuart_getc(u, 30), -1);
    check("errno ETIMEDOUT", errno, ETIMEDOUT);
    softuart_close(u);
    check("pin input after close", gpio_get_mode(pin), GPIO_IN);

    // 空いたコアがないときは、読んでいる間だけ受ける
    static volatile int stop;
    stop = 0;
    int started[4] = {0};
    for (int c = 1; c < monocs_cores(); c++)
        started[c] = monocs_core_start(c, spin, (void *)&stop);
    u = softuart_open(pin, pin, 9600);
    check("no free core: foreground", softuart_rx_background(u), 0);
    uint64_t t0 = timer_now_us();
    check("read times out", softuart_read(u, (char[4]){0}, 4, 50), 0);
    long ms = (long)(timer_now_us() - t0) / 1000;
    check("after ~50ms", ms >= 45 && ms <= 80, 1);
    softuart_close(u);
    stop = 1;
    for (int c = 1; c < 4; c++)
        if (started[c])
            monocs_core_wait(c);

    printf("%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
