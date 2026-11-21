// gpio.c: GPIO を使う（monocs_gpio.h）。共有のライブラリ /lib/gpio
// 出典: BCM2835 ARM Peripherals 6 章（6.1 のレジスタの表と、GPPUD・GPPUDCLKn の手順）
#include "monocs_gpio.h"

#include <errno.h>
#include <stdint.h>
#include <monocs.h>

#define GPIO_BASE   0x3F200000UL    // 周辺機器 0x3F000000 + 0x200000（バスアドレス 0x7E200000）
#define GPFSEL0     0x00            // 機能の選択（10 ピンずつ、1 ピン 3 ビット）
#define GPSET0      0x1C            // 1 を書いたピンを High に
#define GPCLR0      0x28            // 1 を書いたピンを Low に
#define GPLEV0      0x34            // ピンの値
#define GPEDS0      0x40            // エッジ・レベルを見つけた印（1 を書いて消す）
#define GPREN0      0x4C            // 立ち上がりを見つける（同期。システムのクロックで 3 回見て決める）
#define GPFEN0      0x58            // 立ち下がりを見つける（同期）
#define GPPUD       0x94            // プルの指定（0 なし、1 下、2 上）
#define GPPUDCLK0   0x98            // GPPUD を当てるピン

#define GPIO_PINS   54

// OS が使うピン: コンソールの UART（GPIO14 TXD0・15 RXD0）。ヘッダーの外（28〜53）の LED（29）・
// Wi-Fi（34〜39、41）・SD カード（48〜53）は GPIO_PIN_MAX で外れる
#define OS_PINS     ((1U << 14) | (1U << 15))

static inline volatile uint32_t *reg(unsigned off)
{
    return (volatile uint32_t *)(GPIO_BASE + off);
}

int gpio_pin_ok(int pin)
{
    return pin >= 0 && pin <= GPIO_PIN_MAX && !(OS_PINS & (1U << pin));
}

static int check_pin(int pin)
{
    if (gpio_pin_ok(pin))
        return 0;
    errno = (pin >= 0 && pin <= GPIO_PIN_MAX) ? EBUSY : EINVAL;
    return -1;
}

int gpio_mode(int pin, int mode)
{
    if (check_pin(pin) < 0)
        return -1;
    if (mode < 0 || mode > 7) {
        errno = EINVAL;
        return -1;
    }
    volatile uint32_t *r = reg(GPFSEL0 + (unsigned)pin / 10 * 4);
    unsigned shift = (unsigned)pin % 10 * 3;
    // 同じレジスタの OS のピン（GPFSEL1 の 14・15）も読み直した値のまま書き戻す
    *r = (*r & ~(7U << shift)) | ((uint32_t)mode << shift);
    return 0;
}

int gpio_get_mode(int pin)
{
    if (pin < 0 || pin >= GPIO_PINS) {
        errno = EINVAL;
        return -1;
    }
    return (int)(*reg(GPFSEL0 + (unsigned)pin / 10 * 4) >> ((unsigned)pin % 10 * 3) & 7);
}

// 6.1 節の手順: GPPUD に書く → 150 サイクル待つ → GPPUDCLK に当てるピン → 150 サイクル待つ → 両方 0 に。
// 1µs（1000MHz で 1000 サイクル）ずつ待てば足りる
int gpio_pull(int pin, int pull)
{
    if (check_pin(pin) < 0)
        return -1;
    if (pull < GPIO_PULL_OFF || pull > GPIO_PULL_UP) {
        errno = EINVAL;
        return -1;
    }
    *reg(GPPUD) = (uint32_t)pull;
    timer_delay_us(1);
    *reg(GPPUDCLK0) = 1U << pin;
    timer_delay_us(1);
    *reg(GPPUD) = 0;
    *reg(GPPUDCLK0) = 0;
    return 0;
}

int gpio_write(int pin, int value)
{
    if (check_pin(pin) < 0)
        return -1;
    *reg(value ? GPSET0 : GPCLR0) = 1U << pin;
    return 0;
}

int gpio_read(int pin)
{
    if (pin < 0 || pin >= GPIO_PINS) {
        errno = EINVAL;
        return -1;
    }
    return (int)(*reg(GPLEV0 + (unsigned)pin / 32 * 4) >> ((unsigned)pin % 32) & 1);
}

int gpio_toggle(int pin)
{
    if (check_pin(pin) < 0)
        return -1;
    int v = !gpio_read(pin);
    gpio_write(pin, v);
    return v;
}

uint32_t gpio_read_all(void)
{
    return *reg(GPLEV0);
}

int gpio_write_mask(uint32_t mask, uint32_t value)
{
    uint32_t ok = ((1U << (GPIO_PIN_MAX + 1)) - 1) & ~OS_PINS;
    if (mask & ~ok) {
        errno = EBUSY;
        return -1;
    }
    if (mask & value)
        *reg(GPSET0) = mask & value;
    if (mask & ~value)
        *reg(GPCLR0) = mask & ~value;
    return 0;
}

int gpio_wait_edge(int pin, int edge, int timeout_ms)
{
    if (check_pin(pin) < 0)
        return -1;
    if (edge < GPIO_EDGE_RISING || edge > GPIO_EDGE_BOTH) {
        errno = EINVAL;
        return -1;
    }
    uint32_t bit = 1U << pin;
    // 見つける設定は待つ間だけ入れる（GPU の GPIO の割り込み 49〜52 は OS が許可していないので、印が立つだけ）
    if (edge & GPIO_EDGE_RISING)
        *reg(GPREN0) |= bit;
    if (edge & GPIO_EDGE_FALLING)
        *reg(GPFEN0) |= bit;
    *reg(GPEDS0) = bit;     // 前に立っていた印を消す
    uint64_t start = timer_now_us();
    uint64_t limit = timeout_ms < 0 ? 0 : (uint64_t)timeout_ms * 1000;
    int got = 0;
    for (;;) {
        if (*reg(GPEDS0) & bit) {
            *reg(GPEDS0) = bit;
            if (edge == GPIO_EDGE_BOTH)
                got = gpio_read(pin) ? GPIO_EDGE_RISING : GPIO_EDGE_FALLING;
            else
                got = edge;
            break;
        }
        if (timeout_ms >= 0 && timer_now_us() - start >= limit)
            break;
    }
    *reg(GPREN0) &= ~bit;
    *reg(GPFEN0) &= ~bit;
    *reg(GPEDS0) = bit;
    return got;
}
