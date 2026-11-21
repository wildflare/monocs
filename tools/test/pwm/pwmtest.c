// /lib/pwm の試験。
//   cc -o pwmtest -lpwm -lgpio pwmtest.c && ./pwmtest        QEMU（PWM そのものは真似ていないので、引数・ピン・クロックマネージャーの設定だけ）
//   cc -o pwmtest -lpwm -lgpio pwmtest.c && ./pwmtest hw     実機（何もつながない）。ピンの値を読み続けて、周波数と High の割合を測る
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <monocs.h>
#include <monocs_gpio.h>
#include <monocs_pwm.h>

static int fails;

static void check(const char *what, long got, long want)
{
    printf("%s %s: %ld\n", got == want ? "ok" : "NG", what, got);
    if (got != want)
        fails++;
}

static inline uint64_t ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v) : : "memory");
    return v;
}

// ms ミリ秒、pin の値を読み続ける。最初と最後の立ち上がりの間の周期の数と、その間の High の割合
static void measure(int pin, int ms, double *freq, double *duty, double *high_us)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) : : "memory");
    uint64_t t0 = ticks(), end = t0 + f / 1000 * (uint64_t)ms;
    int prev = gpio_read(pin), rises = 0, highs = 0;
    uint64_t first = 0, last = 0, high_ticks = 0, rise_t = 0, high_sum = 0;
    for (;;) {
        uint64_t t = ticks();
        if (t >= end)
            break;
        int v = gpio_read(pin);
        if (v && !prev) {
            if (rises == 0)
                first = t;
            last = t;
            rises++;
            rise_t = t;
        } else if (!v && prev && rises > 0) {
            high_sum += t - rise_t;
            highs++;
        }
        prev = v;
    }
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
    (void)high_ticks;
    *freq = rises > 1 ? (double)(rises - 1) * (double)f / (double)(last - first) : 0;
    double period = rises > 1 ? (double)(last - first) / (rises - 1) : 0;
    *high_us = highs ? (double)high_sum / highs * 1e6 / (double)f : 0;
    *duty = period > 0 && highs ? (double)high_sum / highs / period : 0;
}

static void wave(const char *name, int pin, int ms, double want_f, double want_duty, double tol_duty)
{
    double f, d, h;
    measure(pin, ms, &f, &d, &h);
    printf("-- %s: %.2f Hz, duty %.4f, high %.1f us\n", name, f, d, h);
    char what[96];
    snprintf(what, sizeof what, "%s freq within 1%%", name);
    check(what, f > want_f * 0.99 && f < want_f * 1.01, 1);
    snprintf(what, sizeof what, "%s duty", name);
    check(what, d > want_duty - tol_duty && d < want_duty + tol_duty, 1);
}

int main(int argc, char **argv)
{
    bool hw = argc > 1 && !strcmp(argv[1], "hw");
    errno = 0;
    check("pin 5", pwm_set(5, 1000, 0.5), -1);
    check("errno EINVAL", errno, EINVAL);
    check("freq 0", pwm_set(18, 0, 0.5), -1);
    check("freq 5MHz", pwm_set(18, 5000000, 0.5), -1);
    check("duty 1.5", pwm_set(18, 1000, 1.5), -1);
    check("ns duty > period", pwm_set_ns(18, 1000, 2000), -1);
    check("set 18", pwm_set(18, 1000, 0.25), 0);
    check("GPIO18 ALT5", gpio_get_mode(18), GPIO_ALT5);
    volatile uint32_t *cm = (volatile uint32_t *)0x3F1010A0UL;
    check("clock: oscillator, enabled", (long)(cm[0] & 0x1F), 0x11);
    check("clock: divider 2", (long)(cm[1] >> 12 & 0xFFF), 2);
    check("set 13 (ALT0)", (pwm_servo(13, 1500), gpio_get_mode(13)), GPIO_ALT0);
    if (hw) {
        double f, d;
        check("get 18 on", pwm_get(18, &f, &d), 1);
        check("get 18 = 1000Hz 25%", f == 1000 && d == 0.25, 1);
        check("get 12 off (not muxed)", pwm_get(12, nullptr, nullptr), 0);
        wave("GPIO18 1kHz 25%", 18, 50, 1000, 0.25, 0.005);
        double h;
        measure(13, 200, &f, &d, &h);
        printf("-- GPIO13 servo: %.2f Hz, high %.1f us\n", f, h);
        check("servo 50Hz", f > 49.5 && f < 50.5, 1);
        check("servo 1500us", h > 1490 && h < 1510, 1);
        check("set 12 100kHz", pwm_set(12, 100000, 0.5), 0);
        wave("GPIO12 100kHz 50%", 12, 20, 100000, 0.5, 0.05);
        wave("GPIO18 too (same channel)", 18, 20, 100000, 0.5, 0.05);
        check("polarity", pwm_set_polarity(18, 1), 0);
        wave("GPIO18 inverted", 18, 20, 100000, 0.5, 0.05);
        pwm_set(18, 1000, 0.25);
        wave("GPIO18 inverted 25%", 18, 50, 1000, 0.75, 0.005);
        pwm_set_polarity(18, 0);
        check("set_ns 20us/5us", pwm_set_ns(19, 20000, 5000), 0);
        wave("GPIO19 50kHz 25%", 19, 20, 50000, 0.25, 0.05);
        wave("GPIO13 now 50kHz too", 13, 20, 50000, 0.25, 0.05);
        pwm_set(18, 1000, 0);
        measure(18, 20, &f, &d, &h);
        check("duty 0: no edges, low", f == 0 && gpio_read(18) == 0, 1);
        pwm_set(18, 1000, 1);
        timer_delay_us(2000);
        check("duty 1: high", gpio_read(18), 1);
    }
    int pins[] = {12, 13, 18, 19};
    for (int k = 0; k < 4; k++)
        pwm_off(pins[k]);
    check("off: GPIO18 output low", gpio_get_mode(18) == GPIO_OUT && gpio_read(18) == 0, 1);
    check("get 18 off", pwm_get(18, nullptr, nullptr), 0);
    for (int k = 0; k < 4; k++)
        gpio_mode(pins[k], GPIO_IN);
    printf("%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
