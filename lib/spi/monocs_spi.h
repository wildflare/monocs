// monocs_spi.h: SPI のマスター（BCM2835 の SPI0）。共有のライブラリ /lib/spi（自作。/lib/gpio を使う）
//
//   使い方: compile_flags.txt か cc に -lspi。`#include <monocs_spi.h>`
//
//     spi_open(0, 1000000, SPI_MODE_0);                     // CE0 の機器を 1MHz・モード 0 で（省くとこの設定）
//     uint8_t id[3];
//     spi_write_read(0, (uint8_t[]){0x9F}, 1, id, 3);       // 0x9F を送ってから 3 バイト読む（その間 CS はつけたまま）
//     uint8_t tx[4] = {...}, rx[4];
//     spi_transfer(0, tx, rx, 4);                            // 送りながら受ける（全二重）
//
//   - ピン: GPIO11（SCLK、ヘッダーの 23 番）・GPIO10（MOSI、19 番）・GPIO9（MISO、21 番）・
//     GPIO8（CE0、24 番）・GPIO7（CE1、26 番）。1 つ目の引数 cs は 0（CE0）か 1（CE1）。
//   - SPI_NO_CS を付けると、CE のピンを SPI にしない（GPIO のまま）。ほかのピンを CS に使うときは、自分で gpio_write で動かす。
//   - mode は Linux の spidev と同じ値（SPI_CPHA・SPI_CPOL・SPI_MODE_0〜3、SPI_CS_HIGH、SPI_NO_CS）。
//     1 語は 8 ビット、上位のビットが先（SPI_LSB_FIRST はできない）。
//   - spi_open を呼ばずに使うと、その cs を 1MHz・モード 0 で開く。ピンの設定はアプリが終わっても残る（spi_close で入力に戻す）。
//   - 速さはコアのクロック（Monocs の config.txt の force_turbo=1 で 400MHz に固定）の偶数分の 1。近い遅いほうになる。
//   - 失敗は -1 で、errno は EINVAL（引数）・ETIMEDOUT（終わらない）。待つ間は CPU を使い続ける（割り込みは使わない）。
//
// 出典: BCM2835 ARM Peripherals 10 章（SPI。10.5 のレジスタ、10.6.1 のポーリングの手順）、Linux の
//       drivers/spi/spi-bcm2835.c（分周の決め方、CS の極性）。SPI1・2（補助の SPI）はない。
#pragma once

#include <stdint.h>

// Linux の <linux/spi/spi.h>・spidev と同じ値
#define SPI_CPHA        0x01
#define SPI_CPOL        0x02
#define SPI_MODE_0      0
#define SPI_MODE_1      SPI_CPHA
#define SPI_MODE_2      SPI_CPOL
#define SPI_MODE_3      (SPI_CPOL | SPI_CPHA)
#define SPI_CS_HIGH     0x04    // CS が High でつく
#define SPI_LSB_FIRST   0x08    // （できない。spi_open が EINVAL）
#define SPI_NO_CS       0x40    // CE のピンを使わない

#define SPI_HZ_MIN      7000        // 分周 65536（400MHz ÷ 65536 ≒ 6.1kHz）の上
#define SPI_HZ_MAX      125000000

// cs（0・1）の設定を決め、ピンを SPI にする。hz は 0 で 1MHz。0 = 成功、-1 = 失敗
int spi_open(int cs, int hz, int mode);
// 閉じる（ピンを入力に戻す。もう一方の cs が開いていれば SCLK・MOSI・MISO は残す）
int spi_close(int cs);
// 実際の速さ（Hz）。開いていなければ -1
int spi_get_hz(int cs);

// len バイトを送りながら受ける。tx が NULL なら 0x00 を送る、rx が NULL なら受けたものを捨てる。成功したら len
int spi_transfer(int cs, const void *tx, void *rx, int len);
int spi_write(int cs, const void *tx, int len);
int spi_read(int cs, void *rx, int len);
// txlen バイト送ってから rxlen バイト受ける（その間 CS をつけたまま。レジスタを読む形）。成功したら rxlen
int spi_write_read(int cs, const void *tx, int txlen, void *rx, int rxlen);
