// pwm.c: ハードウェアの PWM（monocs_pwm.h）。共有のライブラリ /lib/pwm
// 出典: BCM2835 ARM Peripherals 9 章（PWM）・6.3 章（クロックマネージャー）、Linux の drivers/clk/bcm/clk-bcm2835.c
//       （CM_PWMCTL・CM_PWMDIV の番地）、drivers/pwm/pwm-bcm2835.c（M/S のモードで周期と High の時間を書く）
#include "monocs_pwm.h"

#include <errno.h>
#include <stdint.h>
#include <monocs.h>
#include <monocs_gpio.h>

#define PWM_BASE    0x3F20C000UL    // バスアドレス 0x7E20C000
#define PWM_CTL     0x00
#define PWM_STA     0x04
#define PWM_RNG(c)  (0x10 + (c) * 0x10)
#define PWM_DAT(c)  (0x14 + (c) * 0x10)

// CTL のチャンネルごとのビット（チャンネル 1 は 8 ビット上）
#define CTL_PWEN    (1U << 0)       // 出す
#define CTL_POLA    (1U << 4)       // 極性を逆に
#define CTL_MSEN    (1U << 7)       // M/S（RNG のうち DAT 個だけ High。分けて散らさない）
#define CTL_MASK    0xFFU
#define STA_BERR    (1U << 8)

#define CM_BASE     0x3F101000UL
#define CM_PWMCTL   0xA0
#define CM_PWMDIV   0xA4
#define CM_PASSWD   (0x5AU << 24)
#define CM_BUSY     (1U << 7)
#define CM_ENAB     (1U << 4)
#define CM_SRC_OSC  1U              // 水晶 19.2MHz
#define OSC_HZ      19200000U
#define CLK_DIVI    2U              // 9.6MHz

static inline volatile uint32_t *pwm(unsigned off)
{
    return (volatile uint32_t *)(PWM_BASE + off);
}

static inline volatile uint32_t *cm(unsigned off)
{
    return (volatile uint32_t *)(CM_BASE + off);
}

// ピン → チャンネル（使えないピンは -1）
static int channel(int pin)
{
    if (pin == 12 || pin == 18)
        return 0;
    if (pin == 13 || pin == 19)
        return 1;
    errno = EINVAL;
    return -1;
}

static int alt_of(int pin)
{
    return pin < 14 ? GPIO_ALT0 : GPIO_ALT5;   // 12・13 は ALT0、18・19 は ALT5（6.2 の表）
}

// PWM のクロックを 9.6MHz に（もうそうなっていれば何もしない。もう一方のチャンネルを乱さないように）
static void clock_setup(void)
{
    uint32_t want_div = CLK_DIVI << 12;
    if ((*cm(CM_PWMCTL) & (CM_ENAB | 0xF)) == (CM_ENAB | CM_SRC_OSC) && (*cm(CM_PWMDIV) & 0xFFFFFF) == want_div)
        return;
    // 6.3: 動いている間に分周を変えない。ENAB を外して BUSY が消えるのを待ち、分周を書いてから ENAB を立てる
    uint32_t ctl = *pwm(PWM_CTL);
    *pwm(PWM_CTL) = 0;
    *cm(CM_PWMCTL) = CM_PASSWD | CM_SRC_OSC;
    uint64_t t0 = timer_now_us();
    while ((*cm(CM_PWMCTL) & CM_BUSY) && timer_now_us() - t0 < 10000)
        ;
    *cm(CM_PWMDIV) = CM_PASSWD | want_div;
    *cm(CM_PWMCTL) = CM_PASSWD | CM_SRC_OSC | CM_ENAB;
    t0 = timer_now_us();
    while (!(*cm(CM_PWMCTL) & CM_BUSY) && timer_now_us() - t0 < 10000)
        ;
    *pwm(PWM_CTL) = ctl;
}

// 周期と High の数（クロックの数）で出す
static int set_counts(int pin, uint32_t range, uint32_t data)
{
    int ch = channel(pin);
    if (ch < 0)
        return -1;
    if (range < 2 || data > range) {
        errno = EINVAL;
        return -1;
    }
    clock_setup();
    unsigned sh = (unsigned)ch * 8;
    uint32_t ctl = *pwm(PWM_CTL);
    *pwm(PWM_RNG(ch)) = range;
    timer_delay_us(1);              // 続けて書くとバスの誤り（BERR）になることがある
    *pwm(PWM_DAT(ch)) = data;
    timer_delay_us(1);
    ctl = (ctl & ~(CTL_MASK << sh)) | ((ctl & (CTL_POLA << sh)) | ((CTL_PWEN | CTL_MSEN) << sh));
    *pwm(PWM_CTL) = ctl;
    *pwm(PWM_STA) = STA_BERR;
    return gpio_mode(pin, alt_of(pin));
}

int pwm_set_ns(int pin, uint64_t period_ns, uint64_t duty_ns)
{
    if (duty_ns > period_ns || period_ns == 0) {
        errno = EINVAL;
        return -1;
    }
    // 1 クロック = 1e9 / 9.6MHz ns。四捨五入
    uint64_t range = (period_ns * (PWM_CLOCK_HZ / 1000) + 500000) / 1000000;
    uint64_t data = (duty_ns * (PWM_CLOCK_HZ / 1000) + 500000) / 1000000;
    if (range < 2 || range > 0xFFFFFFFFU) {
        errno = EINVAL;
        return -1;
    }
    if (data > range)
        data = range;
    return set_counts(pin, (uint32_t)range, (uint32_t)data);
}

int pwm_set(int pin, double freq_hz, double duty)
{
    if (!(freq_hz >= 1 && freq_hz <= PWM_CLOCK_HZ / 2) || !(duty >= 0 && duty <= 1)) {
        errno = EINVAL;
        return -1;
    }
    double r = PWM_CLOCK_HZ / freq_hz + 0.5;
    uint32_t range = (uint32_t)r;
    uint32_t data = (uint32_t)(range * duty + 0.5);
    return set_counts(pin, range, data > range ? range : data);
}

int pwm_servo(int pin, int pulse_us)
{
    if (pulse_us < 0 || pulse_us > 20000) {
        errno = EINVAL;
        return -1;
    }
    return pwm_set_ns(pin, 20000000, (uint64_t)pulse_us * 1000);
}

int pwm_set_polarity(int pin, int inverted)
{
    int ch = channel(pin);
    if (ch < 0)
        return -1;
    uint32_t bit = CTL_POLA << (ch * 8);
    uint32_t ctl = *pwm(PWM_CTL);
    *pwm(PWM_CTL) = inverted ? ctl | bit : ctl & ~bit;
    if (gpio_get_mode(pin) == GPIO_OUT)
        gpio_write(pin, inverted ? 1 : 0);      // 止めているときの休みの値も
    return 0;
}

int pwm_off(int pin)
{
    int ch = channel(pin);
    if (ch < 0)
        return -1;
    uint32_t ctl = *pwm(PWM_CTL);
    bool inv = ctl & (CTL_POLA << (ch * 8));
    *pwm(PWM_CTL) = ctl & ~(CTL_PWEN << (ch * 8));
    // ピンを休みの値の出力に（浮かせると、モーターのドライバーなどが勝手に動くことがある）
    gpio_write(pin, inv ? 1 : 0);
    gpio_mode(pin, GPIO_OUT);
    return 0;
}

int pwm_get(int pin, double *freq_hz, double *duty)
{
    int ch = channel(pin);
    if (ch < 0)
        return -1;
    uint32_t ctl = *pwm(PWM_CTL) >> (ch * 8);
    bool on = (ctl & CTL_PWEN) && gpio_get_mode(pin) == alt_of(pin);
    uint32_t range = *pwm(PWM_RNG(ch)), data = *pwm(PWM_DAT(ch));
    // クロックは今の設定から（ほかが変えていても正しく出す）
    uint32_t div = (*cm(CM_PWMDIV) >> 12) & 0xFFF;
    uint32_t src = *cm(CM_PWMCTL) & 0xF;
    double clk = src == CM_SRC_OSC && div ? (double)OSC_HZ / div : 0;
    if (freq_hz)
        *freq_hz = range && clk ? clk / range : 0;
    if (duty)
        *duty = range ? (double)(data > range ? range : data) / range : 0;
    return on;
}
