// /lib/gpio の試験（QEMU と実機）。何もつながっていないピン（既定 GPIO5・6）を使う。
//   cc -o gpiotest -lgpio gpiotest.c && ./gpiotest [PIN_A PIN_B] [hw]
// hw を付けると、QEMU にない部分（プル、エッジ）も試す（実機だけ）。終わったら両方のピンを入力に戻す。
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs.h>
#include <monocs_gpio.h>

static int fails;
static int pin_b;

// コア 1 で: 5ms 後に High、さらに 5ms 後に Low
static void pulse(void *arg)
{
    (void)arg;
    timer_delay_us(5000);
    gpio_write(pin_b, 1);
    timer_delay_us(5000);
    gpio_write(pin_b, 0);
}

static void check(const char *what, long got, long want)
{
    printf("%s %s: %ld\n", got == want ? "ok" : "NG", what, got);
    if (got != want)
        fails++;
}

int main(int argc, char **argv)
{
    int a = 5, b = 6, hw = 0;
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "hw"))
            hw = 1;
        else if (n++ == 0)
            a = atoi(argv[i]);
        else
            b = atoi(argv[i]);
    }
    check("pin_ok a", gpio_pin_ok(a), 1);
    check("pin_ok 14", gpio_pin_ok(14), 0);
    check("pin_ok 28", gpio_pin_ok(28), 0);
    errno = 0;
    check("mode 15", gpio_mode(15, GPIO_OUT), -1);
    check("errno EBUSY", errno, EBUSY);
    errno = 0;
    check("write 40", gpio_write(40, 1), -1);
    check("errno EINVAL", errno, EINVAL);
    check("mode 14 stays ALT0", gpio_get_mode(14), GPIO_ALT0);
    check("bad mode", gpio_mode(a, 8), -1);

    int m14 = gpio_get_mode(14), m15 = gpio_get_mode(15);
    check("mode a out", gpio_mode(a, GPIO_OUT), 0);
    check("get_mode a", gpio_get_mode(a), GPIO_OUT);
    check("UART pins untouched", gpio_get_mode(14) == m14 && gpio_get_mode(15) == m15, 1);
    gpio_write(a, 1);
    check("read a high", gpio_read(a), 1);
    gpio_write(a, 0);
    check("read a low", gpio_read(a), 0);
    check("toggle", gpio_toggle(a), 1);
    check("read after toggle", gpio_read(a), 1);
    gpio_mode(b, GPIO_OUT);
    check("write_mask", gpio_write_mask((1U << a) | (1U << b), 1U << b), 0);
    check("read_all", (long)(gpio_read_all() & ((1U << a) | (1U << b))), (long)(1U << b));
    check("write_mask with 14", gpio_write_mask(1U << 14, 0), -1);
    check("alt mode", (gpio_mode(a, GPIO_ALT5), gpio_get_mode(a)), GPIO_ALT5);

    if (hw) {
        // プル: 入力にして何もつながっていなければ、プルの向きの値になる
        gpio_mode(a, GPIO_IN);
        gpio_pull(a, GPIO_PULL_UP);
        timer_delay_us(100);
        check("pull up", gpio_read(a), 1);
        gpio_pull(a, GPIO_PULL_DOWN);
        timer_delay_us(100);
        check("pull down", gpio_read(a), 0);
        // エッジ: 出力のピンを自分で動かしても印が立つ（ピンの値を見るため）。待つ前に動かしたものは数えない
        gpio_write(b, 0);
        gpio_mode(b, GPIO_OUT);
        check("no edge (timeout)", gpio_wait_edge(b, GPIO_EDGE_BOTH, 20), 0);
        // 待ちを始めてから動かすため、コア 1 に頼む
        pin_b = b;
        uint64_t t0 = timer_now_us();
        monocs_core_start(1, pulse, nullptr);
        check("rising", gpio_wait_edge(b, GPIO_EDGE_RISING, 1000), GPIO_EDGE_RISING);
        check("falling", gpio_wait_edge(b, GPIO_EDGE_FALLING, 1000), GPIO_EDGE_FALLING);
        monocs_core_wait(1);
        check("edge in time", timer_now_us() - t0 < 100000, 1);
        monocs_core_start(1, pulse, nullptr);
        check("both (rise)", gpio_wait_edge(b, GPIO_EDGE_BOTH, 1000), GPIO_EDGE_RISING);
        check("both (fall)", gpio_wait_edge(b, GPIO_EDGE_BOTH, 1000), GPIO_EDGE_FALLING);
        monocs_core_wait(1);
        // プルを電源を入れたときの既定に戻す（GPIO0〜8 は上、9〜27 は下）
        gpio_pull(a, a <= 8 ? GPIO_PULL_UP : GPIO_PULL_DOWN);
    }
    gpio_write_mask((1U << a) | (1U << b), 0);
    gpio_mode(a, GPIO_IN);
    gpio_mode(b, GPIO_IN);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
