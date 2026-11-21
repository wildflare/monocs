// monocs_gpio.h: GPIO（40 ピンのヘッダーの GPIO0〜27）を使う。共有のライブラリ /lib/gpio（自作）
//
//   使い方: compile_flags.txt か cc に -lgpio。`#include <monocs_gpio.h>`
//
//     gpio_mode(17, GPIO_OUT);  gpio_write(17, 1);           // GPIO17 を High に
//     gpio_mode(27, GPIO_IN);   gpio_pull(27, GPIO_PULL_UP); // ボタン（押すと GND につながる）
//     if (gpio_read(27) == 0) ...
//     gpio_wait_edge(27, GPIO_EDGE_FALLING, 5000);           // 押されるまで（5 秒まで）待つ
//
//   - ピンの番号は BCM の GPIO の番号（ヘッダーのピンの番号ではない。ヘッダーの 11 番 = GPIO17）。
//   - 使えるのは GPIO0〜27 のうち、コンソールの UART（GPIO14・15）を除くもの。ほかは -1（errno は EINVAL・EBUSY）。
//   - 設定はアプリが終わってもそのまま残る（Linux の pinctrl・pigpio と同じ）。終わるときに戻したければ自分で戻す。
//   - プルの設定は読み出せない（BCM2837 にその仕組みがない）。電源を入れたときの既定は GPIO0〜8 がプルアップ、
//     9〜27 がプルダウン。
//   - レジスタを直接読み書きする（アプリも EL1）。割り込みは使わない（エッジは待つ間に調べ続ける）。
//
// 出典: BCM2835 ARM Peripherals 6 章（GPIO。BCM2837 も同じ）
#pragma once

#include <stdint.h>

// ピンの機能（GPFSELn の 3 ビットの値）
#define GPIO_IN     0
#define GPIO_OUT    1
#define GPIO_ALT0   4
#define GPIO_ALT1   5
#define GPIO_ALT2   6
#define GPIO_ALT3   7
#define GPIO_ALT4   3
#define GPIO_ALT5   2

// プル（GPPUD の値）
#define GPIO_PULL_OFF   0
#define GPIO_PULL_DOWN  1
#define GPIO_PULL_UP    2

// エッジ
#define GPIO_EDGE_RISING    1
#define GPIO_EDGE_FALLING   2
#define GPIO_EDGE_BOTH      3

#define GPIO_PIN_MAX    27      // ヘッダーに出ている最後の GPIO

// pin を使えるか（1 = 使える、0 = 使えない（範囲の外・OS が使っている））
int gpio_pin_ok(int pin);

// 機能を決める（GPIO_IN・GPIO_OUT・GPIO_ALTn）。0 = 成功、-1 = 失敗
int gpio_mode(int pin, int mode);
// 今の機能（GPIO_IN など）。-1 = 失敗。OS が使っているピンも読める（0〜53）
int gpio_get_mode(int pin);
// プルを決める（GPIO_PULL_OFF・DOWN・UP）。0 = 成功
int gpio_pull(int pin, int pull);

// 出力を決める（0 = Low、ほか = High）。出力にしていなくても値は覚えられ、出力にしたときに出る
int gpio_write(int pin, int value);
// ピンの今の値（0・1。出力のピンも読める）。-1 = 失敗。OS が使っているピンも読める（0〜53）
int gpio_read(int pin);
// 出力を反対にする（今の値を読んで逆を書く）。新しい値を返す。-1 = 失敗
int gpio_toggle(int pin);

// GPIO0〜31 の値をまとめて読む（ビット n が GPIOn）
uint32_t gpio_read_all(void);
// mask のビットのピンを、value の同じビットの値にまとめて書く（High にするものを先に書く）。
// mask に使えないピンがあれば何もせず -1
int gpio_write_mask(uint32_t mask, uint32_t value);

// エッジを待つ。GPIO_EDGE_RISING・GPIO_EDGE_FALLING（起きたもの）= 起きた、0 = 時間切れ、-1 = 失敗。
// timeout_ms が負ならいつまでも。BOTH のときは起きた後の値で向きを決める。
// ピンの読み取り（GPEDS）で拾うので、待つ間の短いパルスも落とさない。待つ前に起きていたエッジは数えない。
// 待つ間は CPU を使い続ける（Ctrl-C で止められる）
int gpio_wait_edge(int pin, int edge, int timeout_ms);
