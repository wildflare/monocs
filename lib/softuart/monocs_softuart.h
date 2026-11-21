// monocs_softuart.h: ソフトウェアの UART（好きな GPIO 2 本で、CPU がビットを時間どおりに出し入れする）。
// 共有のライブラリ /lib/softuart（自作。/lib/gpio を使う）
//
//   使い方: compile_flags.txt か cc に -lsoftuart。`#include <monocs_softuart.h>`
//
//     int u = softuart_open(17, 27, 9600);         // TX = GPIO17、RX = GPIO27、9600bps（8N1）
//     softuart_write(u, "AT\r\n", 4);
//     char buf[64];
//     int n = softuart_read(u, buf, sizeof buf, 1000);   // 1 秒まで待つ。届いた分（0 = 時間切れ）
//     softuart_close(u);
//
//   - Zero 2 W の本物の UART（PL011・mini UART）は、ヘッダーに出せるのが GPIO14・15 だけで、そこは Monocs のコンソールが
//     使っている。そのため、ソフトウェアで作る。
//   - 形は 8N1（データ 8 ビット、パリティなし、ストップ 1 ビット）、下位のビットが先。300〜115200bps。電圧は 3.3V
//     （5V の機器の TX を直接つながない）。
//   - 受信は、空いたコア（1〜3）を 1 つ使い、裏で受け続ける（4KB の輪に入れる。あふれた分は捨てて数える）。空いたコアが
//     なければ、softuart_read・getc を呼んでいる間だけ受ける（その外で届いたものは落ちる）。
//   - 送信は呼んだコアで行い、1 バイト（10 ビット）の間は割り込みを止める（9600bps で約 1ms）。
//   - rx_pin・tx_pin は -1 で使わない。同じピンにすると、送ったものを自分で受ける（線なしの試験用）。
//   - アプリが終わると受信のコアも止まる。ピンの設定は残る（softuart_close で入力に戻す）。
//   - 失敗は -1 で、errno は EINVAL（引数・ピン）、EMFILE（4 つより多く開いた）、ETIMEDOUT。
#pragma once

#define SOFTUART_MAX        4
#define SOFTUART_BAUD_MIN   300
#define SOFTUART_BAUD_MAX   115200

// 開く。番号（0〜3）を返す。-1 = 失敗
int softuart_open(int tx_pin, int rx_pin, int baud);
int softuart_close(int u);

// 送る。送り終わってから戻る。成功したら len
int softuart_write(int u, const void *data, int len);
int softuart_putc(int u, int c);

// 受けたものを len バイトまで読む。何もなければ timeout_ms まで待つ（負ならいつまでも、0 なら待たない）。
// 読んだバイト数（0 = 時間切れ）、-1 = 失敗
int softuart_read(int u, void *buf, int len, int timeout_ms);
// 1 バイト読む（0〜255）。時間切れは -1（errno ETIMEDOUT）
int softuart_getc(int u, int timeout_ms);
// 読めるバイト数
int softuart_available(int u);
// 受信の誤りの数（ストップビットが Low だった・輪があふれた）。読むと 0 に戻る
unsigned softuart_errors(int u);
// 受信を裏のコアで行っているか（1 = 裏、0 = 呼んでいる間だけ）
int softuart_rx_background(int u);
