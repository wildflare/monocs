// PL011 UART（コンソール）
#pragma once

#include <stddef.h>
#include <stdint.h>

void uart_init(void);
// 1 文字送る（送信バッファに入れてすぐ戻る。uart_enable_irq の前は送り終わるのを待つ）
void uart_putc(char c);
// 送信・受信の割り込みを使い始める（irq_init の後にコア 0 で呼ぶ）
void uart_enable_irq(void);
// 送受信の割り込み（irq_dispatch から）
void uart_handle_irq(void);
// 送信バッファの中身をすべて送る（止まる前の表示などで使う）
void uart_flush(void);
// ボーレート（bps）。上限は 3000000（基準クロック 48MHz ÷ 16）。範囲外なら false
uint32_t uart_baud(void);
bool uart_set_baud(uint32_t baud);
char uart_getc(void);
// 最大 n バイトを受信する。timeout_us の間なにも届かなければ、それまでの分を返す
size_t uart_read(void *buf, size_t n, uint64_t timeout_us);
// 受信バッファがあふれて捨てた文字の数
uint64_t uart_rx_dropped(void);
// 受信済みの文字があれば true
bool uart_has_input(void);
// 受信済みの文字がなければ、次の割り込みまで眠る（コア 0。cpu_idle）
void uart_wait_input(void);
// 読んだ文字を受信バッファの先頭に戻す（次の uart_getc で、この順に読まれる。端末への問い合わせの答えを
// 待つ間に届いたキーを残すため。os/kernel/term.c）
void uart_rx_unread(const char *p, size_t n);
// 受信 FIFO を受信バッファに移す（タイマー割り込みと uart_getc から呼ぶ）
void uart_poll_rx(void);
// Ctrl-C を受けたときに呼ぶ関数を登録する。関数が true を返したら、Ctrl-C は受信バッファに入れない
void uart_set_ctrl_c_handler(bool (*handler)(void));
void uart_raise_interrupt(void);    // Ctrl-\ を受けたのと同じにする（文字は入れない）
// raw: Ctrl-C を文字としてアプリに渡し、代わりに Ctrl-\ でアプリを止める（curses の raw()。アプリが終わったら戻す）
void uart_set_raw(bool raw);
// アプリが動いていないときに押された Ctrl-C の数（シェルの繰り返しを止める）
uint32_t uart_shell_interrupts(void);
bool uart_interrupt_is_quit(void);  // 最後にアプリを止めるキーとして受けたのが Ctrl-\ か
// 最後にアプリを止めたキーの表示（"^C" か "^\\"）
const char *uart_interrupt_key_name(void);
// ほかの入力元（USB のコンソール）から受け取った文字を受信バッファに入れる（Ctrl-C も扱う）
void uart_rx_push(const char *p, size_t n);
// UART に出す文字を、ほかの出力先にも送る（送信バッファを使い始めた後の文字だけ）。
// fn がその出力先が使われていると返したら、UART の送信バッファがいっぱいのとき UART の分は捨てる
// flush は uart_flush から呼ぶ（止まる前に、その出力先にも送り切る）
void uart_add_tx_mirror(bool (*fn)(char c), void (*flush)(void));
void uart_puts(const char *s);
void uart_puthex(uint64_t value);
void uart_putdec(uint64_t value);

// Ctrl-S で出力が止められているか（Ctrl-Q で再開。os/libc/syscalls.c の screen_putc が待つ）
bool uart_output_paused(void);
