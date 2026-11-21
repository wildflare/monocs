// 端末の画面の大きさを、端末に問い合わせて知る
//
// USB シリアルや UART には、telnet の NAWS（RFC 1073）や ssh の window-change（RFC 4254）のように
// 画面の大きさを伝える仕組みがない。そこで Linux の resize コマンドと同じく、カーソルを右下の端に動かして
// 位置を問い合わせる（ECMA-48 / VT100 の CUP「ESC [ 行 ; 列 H」と DSR「ESC [ 6 n」。端末は
// CPR「ESC [ 行 ; 列 R」で答える）。カーソルの位置は DECSC・DECRC（ESC 7、ESC 8）で元に戻す。
// コンソールの出力は UART と USB の両方に出るので、両方の端末が答えることがある。最初の答えを使い、
// 少しの間は 2 つ目の答えも読み捨てる（シェルの入力に残らないように）。
// telnet でつながっているときは、telnet の NAWS（RFC 1073）で知らされた大きさを使う
// （サービス services/telnetd.c がコンソールの写し（os/kernel/contap.c）に知らせる）

#include <stdint.h>
#include "term.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "contap.h"

#define REPLY_TIMEOUT_US    300000  // 答えを待つ時間
#define EXTRA_WAIT_US       50000   // 2 つ目の答えを読み捨てる時間

static void put(const char *s)
{
    for (; *s; s++)
        uart_putc(*s);
}

// 答えを待つ間に届いた、答え以外の文字（キー入力）。終わったら受信バッファの先頭に戻す
static char kept[256];
static int nkept;

static void keep(const char *p, int n)
{
    for (int i = 0; i < n && nkept < (int)sizeof(kept); i++)
        kept[nkept++] = p[i];
}

// CPR を 1 つ読む（deadline まで）。読めたら true。答えでない文字は kept に残す
static bool read_cpr(uint64_t deadline, int *rows, int *cols)
{
    int state = 0, a = 0, b = 0;
    char seq[32];   // 答えかもしれない途中のバイト
    int nseq = 0;
    while (timer_now_us() < deadline) {
        if (!uart_has_input()) {
            uart_wait_input();
            continue;
        }
        char c = uart_getc();
        if (nseq < (int)sizeof(seq))
            seq[nseq++] = c;
        int next = -1;
        switch (state) {
        case 0: next = c == 0x1B ? 1 : -1; break;               // ESC
        case 1: next = c == '[' ? 2 : -1; a = b = 0; break;
        case 2:                                                  // 行
            if (c >= '0' && c <= '9') {
                a = a * 10 + (c - '0');
                next = 2;
            } else {
                next = c == ';' ? 3 : -1;
            }
            break;
        case 3:                                                  // 列
            if (c >= '0' && c <= '9') {
                b = b * 10 + (c - '0');
                next = 3;
            } else if (c == 'R' && a > 0 && b > 0) {
                *rows = a;
                *cols = b;
                return true;
            }
            break;
        }
        if (next < 0) {
            // 答えではなかった: ここまでのバイトはキー入力として残す（ESC なら、そこから読み直す）
            if (c == 0x1B && nseq > 1) {
                keep(seq, nseq - 1);
                seq[0] = c;
                nseq = 1;
                state = 1;
            } else {
                keep(seq, nseq);
                nseq = 0;
                state = 0;
            }
        } else {
            state = next;
        }
    }
    keep(seq, nseq);
    return false;
}

static int known_cols;  // 最後にわかった桁数（0 はまだ）

int term_cols(void)
{
    int rows, cols;
    if (contap_size(&rows, &cols))
        return cols;
    if (!known_cols) {
        term_size(&rows, &cols);
        known_cols = cols > 0 ? cols : 80;
    }
    return known_cols;
}

bool term_size(int *rows, int *cols)
{
    // telnet でつながっていれば、知らされた大きさ（NAWS。ウィンドウの大きさを変えるたびに届く）
    if (contap_size(rows, cols))
        return true;
    *rows = 24;
    *cols = 80;
    nkept = 0;
    put("\x1b" "7" "\x1b[999;999H" "\x1b[6n");
    bool ok = read_cpr(timer_now_us() + REPLY_TIMEOUT_US, rows, cols);
    if (ok) {
        int r, c;
        read_cpr(timer_now_us() + EXTRA_WAIT_US, &r, &c);
    }
    put("\x1b" "8");
    uart_rx_unread(kept, (size_t)nkept);    // 待つ間に届いたキーを、次に読まれるように戻す
    if (ok)
        known_cols = *cols;
    return ok;
}
