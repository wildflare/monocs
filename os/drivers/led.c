// ACT LED（Zero 2 W では GPIO29、Low で点灯）
// 出典: Raspberry Pi OS の bcm2710-rpi-zero-2-w.dtb（/leds/led-act の gpios = <&gpio 29 GPIO_ACTIVE_LOW>）
//       BCM2835 ARM Peripherals 6 章（GPIO）
// QEMU では何も起きない。

#include "led.h"
#include "bcm2837.h"
#include "mmio.h"

#define LED_GPIO        29
#define GPFSEL_OUTPUT   1U
#define LED_FSEL_SHIFT  ((LED_GPIO - 20) * 3)   // GPFSEL2 の中の位置

static bool led_on;
static unsigned blink_half_period;
static unsigned blink_count;

void led_init(void)
{
    uint32_t sel = mmio_read32(GPFSEL2);
    sel &= ~(7U << LED_FSEL_SHIFT);
    sel |= GPFSEL_OUTPUT << LED_FSEL_SHIFT;
    mmio_write32(GPFSEL2, sel);
    led_set(false);
}

void led_set(bool on)
{
    led_on = on;
    mmio_write32(on ? GPCLR0 : GPSET0, 1U << LED_GPIO);
}

static void led_toggle(void)
{
    led_set(!led_on);
}

void led_tick(void)
{
    if (blink_half_period == 0)
        return;
    if (++blink_count >= blink_half_period) {
        blink_count = 0;
        led_toggle();
    }
}

void led_set_blink(unsigned half_period_ticks)
{
    blink_half_period = half_period_ticks;
    blink_count = 0;
}

unsigned led_get_blink(void)
{
    return blink_half_period;
}

void led_set_manual(bool on)
{
    blink_half_period = 0;
    led_set(on);
}
