// monocs_pwm.h: ハードウェアの PWM（BCM2835 の PWM、2 チャンネル）。共有のライブラリ /lib/pwm（自作。/lib/gpio を使う）
//
//   使い方: compile_flags.txt か cc に -lpwm。`#include <monocs_pwm.h>`
//
//     pwm_set(18, 1000, 0.25);      // GPIO18 に 1kHz・High が 25% の波
//     pwm_servo(13, 1500);          // GPIO13 にサーボの信号（50Hz、1.5ms のパルス）
//     pwm_off(18);                  // 止める（ピンは休みの値（Low）の出力のまま）
//
//   - 使えるピン: チャンネル 0 = GPIO12（ヘッダーの 32 番）・GPIO18（12 番）、チャンネル 1 = GPIO13（33 番）・GPIO19（35 番）。
//     同じチャンネルの 2 本は同じ波になる（後で設定したほうに変わる）。
//   - 波はハードウェアが出し続ける（CPU を使わない）。アプリが終わっても止まらない（pwm_off か、シェルの `pwm PIN off` で止める）。
//   - PWM のクロックは水晶（19.2MHz）の 1/2 = 9.6MHz に決める（両方のチャンネルで共通）。周期はその何個分か（2〜2^32-1）
//     なので、周波数は 1Hz〜4.8MHz。細かさは 9.6MHz ÷ 周波数 段（1kHz なら 9600 段、20kHz なら 480 段）。
//   - 失敗は -1 で、errno は EINVAL（ピン・値）。
//
// 出典: BCM2835 ARM Peripherals 9 章（PWM。9.6 のレジスタ）・6.3 章（クロックマネージャーのレジスタの形と手順）。
//       PWM のクロックの番地（CM_PWMCTL・CM_PWMDIV、0x7E1010A0・A4）は資料になく、Linux の drivers/clk/bcm/clk-bcm2835.c による。
#pragma once

#include <stdint.h>

#define PWM_CLOCK_HZ    9600000

// 周波数 freq_hz（1〜4800000）、duty（0.0〜1.0。High の割合）で出す。0 = 成功
int pwm_set(int pin, double freq_hz, double duty);
// Linux の /sys/class/pwm と同じ形: 周期と High の時間をナノ秒で
int pwm_set_ns(int pin, uint64_t period_ns, uint64_t duty_ns);
// サーボ: 50Hz で pulse_us マイクロ秒のパルス（ふつう 500〜2500）
int pwm_servo(int pin, int pulse_us);
// 極性を逆に（1: 休みが High、duty の間 Low）。0 で元に。次の pwm_set からではなく、すぐ効く
int pwm_set_polarity(int pin, int inverted);
// 止める（ピンは休みの値の出力にする）
int pwm_off(int pin);
// 今の設定を読む（レジスタから。ほかのアプリ・コマンドが設定したものも）。1 = 出している、0 = 止まっている、-1 = 失敗。
// freq_hz・duty は NULL 可
int pwm_get(int pin, double *freq_hz, double *duty);
