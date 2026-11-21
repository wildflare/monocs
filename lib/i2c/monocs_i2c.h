// monocs_i2c.h: I2C のマスター（BCM2835 の BSC）。共有のライブラリ /lib/i2c（自作。/lib/gpio を使う）
//
//   使い方: compile_flags.txt か cc に -li2c。`#include <monocs_i2c.h>`
//
//     i2c_open(1, 400000);                          // バス 1（GPIO2 SDA・GPIO3 SCL）を 400kHz で（省くと 100kHz）
//     int id = i2c_read_reg(1, 0x76, 0xD0);         // 0x76 の機器のレジスタ 0xD0 を 1 バイト読む
//     i2c_write_reg(1, 0x76, 0xF4, 0x27);           // レジスタ 0xF4 に 0x27 を書く
//     uint8_t buf[6];
//     i2c_read_regs(1, 0x76, 0xF7, buf, 6);         // 0xF7 から 6 バイト（リピーテッドスタートで）
//
//   - バス 1: GPIO2（SDA1、ヘッダーの 3 番）・GPIO3（SCL1、5 番）。基板に 1.8kΩ のプルアップがある。
//     バス 0: GPIO0（SDA0、27 番）・GPIO1（SCL0、28 番）。HAT の ID の EEPROM 用で、基板のプルアップはない
//     （中のプルアップ約 50kΩ を入れるが弱いので、外に 2.2〜10kΩ を付ける）。
//   - アドレスは 7 ビット（0x00〜0x7F）。
//   - i2c_open を呼ばずに使うと、そのバスを 100kHz で開く。ピンの設定はアプリが終わっても残る（i2c_close で入力に戻す）。
//   - 失敗は -1 で、errno は ENXIO（相手が応答しない・NACK）、ETIMEDOUT（SCL が引き伸ばされたまま・終わらない）、
//     EINVAL（引数）、EIO（途中で止まった）。
//   - BCM2835 の I2C は、相手が SCL を引き伸ばす（クロックストレッチ）のを正しく扱えない（ハードウェアの誤り。Linux でも
//     同じ）。引き伸ばす機器（一部のセンサー・マイコン）は、遅いクロックにするとよい。
//   - 待つ間は CPU を使い続ける（割り込みは使わない）。i2c_write_read はリピーテッドスタートのために、書く間だけ割り込みを止める。
//   - バスの速さはコアのクロック（Monocs の config.txt の force_turbo=1 で 400MHz に固定）を分けて作る。
//
// 出典: BCM2835 ARM Peripherals 3 章（BSC）。リピーテッドスタートの手順は 3.3 の 10 ビットのアドレスの読み出しの説明と、
//       Linux の drivers/i2c/busses/i2c-bcm2835.c の説明（DIV・DEL・CLKT の決め方も）
#pragma once

#include <stdint.h>

#define I2C_FIFO    16      // i2c_write_read で先に書ける長さ

// バスを開く（ピンを I2C にし、速さを決める）。hz は 0 で 100kHz（10kHz〜1MHz）。0 = 成功、-1 = 失敗
int i2c_open(int bus, int hz);
// バスを閉じる（コントローラを止め、ピンを入力に戻す）
int i2c_close(int bus);

// 書く・読む。成功したら長さ（0〜65535）、失敗は -1
int i2c_write(int bus, int addr, const void *data, int len);
int i2c_read(int bus, int addr, void *data, int len);
// 書いてから、止めずに（リピーテッドスタートで）読む。wlen は 1〜I2C_FIFO、rlen は 1 以上。成功したら rlen
int i2c_write_read(int bus, int addr, const void *wdata, int wlen, void *rdata, int rlen);

// よく使う形（SMBus の read byte data・write byte data と同じ）
int i2c_read_reg(int bus, int addr, int reg);                          // 0〜255、失敗は -1
int i2c_write_reg(int bus, int addr, int reg, int value);              // 0 = 成功
int i2c_read_regs(int bus, int addr, int reg, void *buf, int len);     // reg から len バイト。成功したら len
int i2c_write_regs(int bus, int addr, int reg, const void *buf, int len);

// 相手がいるか（アドレスを送って 1 バイト読む。i2cdetect -r と同じ）。1 = いる、0 = いない、-1 = 失敗
int i2c_probe(int bus, int addr);
