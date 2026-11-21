// PL011 UART ドライバ（GPIO14/15、3Mbps、8N1）
// 出典: BCM2835 ARM Peripherals 6 章（GPIO）、7 章（割り込み）、13 章（UART）
//
// 送信: 送信バッファに入れてすぐ戻り、送信の割り込み（コア 0）でハードウェアの FIFO に移す。
// PL011 の送信割り込みは FIFO が一定量以下の間は立ち続けるので、送るものがなくなったら止め（TXIM を外す）、
// 新しい文字が来たら FIFO に直接書いてから割り込みを戻す（Linux の amba-pl011.c と同じ考え方）。
// バッファがいっぱいのとき（割り込みを止めたまま大量に書く例外の表示など）は、その場で FIFO に書いて空ける。
// 線の速さ（3Mbps、毎秒約 300KB）は変わらないので、出力がバッファより多ければ、その分は待つ。
// 受信: 受信割り込み（FIFO が半分たまったとき、受信が途切れたとき）で FIFO を 64KB の受信バッファに移す
// （uart_poll_rx。3Mbps では 16 文字の FIFO が 53μs であふれるため。タイマー割り込みでも念のため移す）。

#include "../kernel/klog.h"
#include "uart.h"
#include "../libc/termios_os.h"
#include "../libc/signal_os.h"
#include "bcm2837.h"
#include "mmio.h"
#include "../kernel/sysreg.h"
#include "timer.h"

#include <stdatomic.h>

// UART0_FR のビット
#define FR_BUSY         (1U << 3)   // 送信中（FIFO とシフトレジスタが空になるまで）
#define FR_RXFE         (1U << 4)   // 受信 FIFO が空
#define DR_ERRORS       (0xFU << 8) // DR の OE・BE・PE・FE（13.4 節 DR）
#define FR_TXFF         (1U << 5)   // 送信 FIFO が満杯

// UART0_LCRH のビット
#define LCRH_FEN        (1U << 4)   // FIFO 有効
#define LCRH_WLEN_8     (3U << 5)   // 8 ビット

// UART0_IMSC・MIS・ICR のビット（13.4 節）
#define INT_RX          (1U << 4)
#define INT_TX          (1U << 5)
#define INT_RT          (1U << 6)   // 受信の途切れ（FIFO に残っているのに 32 ビット分届かない）

// UART0_IFLS: 送信 FIFO が 1/8（2 文字）以下、受信 FIFO が 1/2（8 文字）以上になったら割り込む
#define IFLS_TX_1_8     (0U << 0)
#define IFLS_RX_1_2     (2U << 3)

// UART0_CR のビット
#define CR_UARTEN       (1U << 0)
#define CR_TXE          (1U << 8)
#define CR_RXE          (1U << 9)

// GPFSEL の機能番号（6.2 節 表 6-2）
#define GPFSEL_ALT0     4U

// UART の基準クロック。config.txt の init_uart_clock=48000000 に合わせる。
// QEMU はボーレート設定を無視するので、この値は実機向け。
#define UART_CLOCK_HZ   48000000U
// 起動時のボーレート。上限の 3Mbps（分周比 1、誤差なし）。実機（ジャンパー線、Pi 5 の ttyAMA0）で
// 200KB の表示が化けずに届くことを確かめた（2026-11-06）。ファームウェアのログは 115200bps のまま。
// シェルの baud コマンドで変えられる
#define UART_BAUD       3000000U

static uint32_t cur_baud;

// ボーレートの分周比を設定する（UART を止めた状態で呼ぶ）。
// 分周比 = UART_CLOCK_HZ / (16 * baud)。小数部は 1/64 単位で四捨五入する（13.4 節 UART_IBRD・FBRD）。
// 例: 115200bps は IBRD = 26、FBRD = 3。上限は 1 倍（UART_CLOCK_HZ / 16 = 3Mbps）
static bool set_divisor(uint32_t baud)
{
    uint64_t div64 = ((uint64_t)UART_CLOCK_HZ * 4 + baud / 2) / baud;
    if (div64 < 64 || (div64 >> 6) > 0xFFFF)
        return false;
    mmio_write32(UART0_IBRD, (uint32_t)(div64 >> 6));
    mmio_write32(UART0_FBRD, (uint32_t)(div64 & 0x3F));
    cur_baud = baud;
    return true;
}

void uart_init(void)
{
    // 設定中は UART を止める
    mmio_write32(UART0_CR, 0);

    // GPIO14（TXD0）、GPIO15（RXD0）を ALT0 にする。GPFSEL1 は 1 ピン 3 ビット。
    uint32_t sel = mmio_read32(GPFSEL1);
    sel &= ~((7U << 12) | (7U << 15));
    sel |= (GPFSEL_ALT0 << 12) | (GPFSEL_ALT0 << 15);
    mmio_write32(GPFSEL1, sel);

    // GPIO14（TXD0）のプルアップ／プルダウンを無効にし、GPIO15（RXD0）はプルアップにする
    // （6.1 節 GPPUD・GPPUDCLKn の手順。GPPUD は 0 = なし、2 = プルアップ）。線を外して USB だけで
    // 使うとき、受信の端子が浮いて雑音を文字として受け取らないように（送らないときの線の状態は High）
    mmio_write32(GPPUD, 0);
    delay_cycles(150);
    mmio_write32(GPPUDCLK0, 1U << 14);
    delay_cycles(150);
    mmio_write32(GPPUDCLK0, 0);
    mmio_write32(GPPUD, 2);
    delay_cycles(150);
    mmio_write32(GPPUDCLK0, 1U << 15);
    delay_cycles(150);
    mmio_write32(GPPUDCLK0, 0);
    mmio_write32(GPPUD, 0);

    // 保留中の割り込みをすべてクリアする
    mmio_write32(UART0_ICR, 0x7FF);

    set_divisor(UART_BAUD);

    mmio_write32(UART0_LCRH, LCRH_FEN | LCRH_WLEN_8);
    mmio_write32(UART0_IMSC, 0);
    mmio_write32(UART0_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

// 送信バッファ。どのコアからも書くので、tx_lock（割り込みを止めて取るスピンロック）で守る
#define TX_BUF_SIZE     (64 * 1024)

static char tx_buf[TX_BUF_SIZE];
static uint32_t tx_head, tx_tail;   // 書く位置・読む位置
static bool tx_irq_on;              // 送信割り込みを使っている（TXIM を立てている）
static bool tx_buffered;            // バッファを使う（割り込みの準備ができてから。uart_enable_irq）
static atomic_flag tx_lock = ATOMIC_FLAG_INIT;

static uint64_t tx_lock_take(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    while (atomic_flag_test_and_set_explicit(&tx_lock, memory_order_acquire))
        __asm__ volatile("yield");
    return daif;
}

static void tx_lock_give(uint64_t daif)
{
    atomic_flag_clear_explicit(&tx_lock, memory_order_release);
    write_sysreg(daif, daif);
}

static bool tx_fifo_full(void)
{
    return (mmio_read32(UART0_FR) & FR_TXFF) != 0;
}

// バッファから FIFO へ、入るだけ移す（tx_lock の中で）
static void tx_fill_fifo(void)
{
    while (tx_tail != tx_head && !tx_fifo_full()) {
        mmio_write32(UART0_DR, (uint8_t)tx_buf[tx_tail]);
        tx_tail = (tx_tail + 1) % TX_BUF_SIZE;
    }
}

// UART に出す文字を、ほかの出力先（USB のコンソール、telnet）にも送る。fn はその出力先が使われている
// （端末が開かれている）とき true を返す。どれかが使われていれば、UART の送信バッファがいっぱいのとき
// UART に出す分を捨てる（UART の速さでほかの出力先の表示が遅くならないように）
#define MAX_MIRRORS 4
static bool (*tx_mirror[MAX_MIRRORS])(char c);
static void (*tx_mirror_flush[MAX_MIRRORS])(void);
static int n_mirrors;
static uint64_t tx_dropped;

void uart_add_tx_mirror(bool (*fn)(char c), void (*flush)(void))
{
    if (n_mirrors == MAX_MIRRORS)
        return;
    tx_mirror[n_mirrors] = fn;
    tx_mirror_flush[n_mirrors] = flush;
    n_mirrors++;
}

void uart_putc(char c)
{
    if (klog_capture_active())      // 起動中の表示と、例外などの表示を記録する（os/kernel/klog.c。dmesg）
        klog_console_char(c, tx_buffered);
    // 送信バッファを使う前（起動直後。コアは 1 つで、MMU を有効にする前のこともある）は、ロックを取らずに
    // 直接書く。キャッシュが無効なメモリでは排他アクセス（ldaxr・stxr）が成功しないことがあり、
    // 実機ではロックを取れずに止まった（QEMU では起きない）
    if (!tx_buffered) {
        while (tx_fifo_full())
            ;
        mmio_write32(UART0_DR, (uint8_t)c);
        return;
    }
    bool mirrored = false;
    for (int i = 0; i < n_mirrors; i++)
        mirrored |= tx_mirror[i](c);
    uint64_t daif = tx_lock_take();
    uint32_t next = (tx_head + 1) % TX_BUF_SIZE;
    if (next == tx_tail && mirrored) {
        tx_dropped++;
        tx_lock_give(daif);
        return;
    }
    if (next == tx_tail) {
        // バッファがいっぱい: その場で FIFO に書いて空ける（割り込みを止めた場所から書いていることがある）
        while (tx_tail != tx_head) {
            while (tx_fifo_full())
                ;
            tx_fill_fifo();
        }
    }
    tx_buf[tx_head] = c;
    tx_head = next;
    tx_fill_fifo();
    if (tx_tail != tx_head && !tx_irq_on) {
        // FIFO に入りきらなかった分は、送信割り込みで送る
        tx_irq_on = true;
        mmio_write32(UART0_IMSC, mmio_read32(UART0_IMSC) | INT_TX);
    }
    tx_lock_give(daif);
}

// 送信割り込み（コア 0 の irq_dispatch から）
void uart_handle_irq(void)
{
    if (mmio_read32(UART0_MIS) & (INT_RX | INT_RT)) {
        uart_poll_rx();
        mmio_write32(UART0_ICR, INT_RX | INT_RT);
    }
    uint64_t daif = tx_lock_take();
    if (mmio_read32(UART0_MIS) & INT_TX) {
        tx_fill_fifo();
        if (tx_tail == tx_head) {
            tx_irq_on = false;
            mmio_write32(UART0_IMSC, mmio_read32(UART0_IMSC) & ~INT_TX);
        }
        mmio_write32(UART0_ICR, INT_TX);
    }
    tx_lock_give(daif);
}

// 送信・受信の割り込みを使い始める（割り込みの準備ができてから、コア 0 で呼ぶ）
void uart_enable_irq(void)
{
    mmio_write32(UART0_IFLS, IFLS_TX_1_8 | IFLS_RX_1_2);
    mmio_write32(UART0_IMSC, mmio_read32(UART0_IMSC) | INT_RX | INT_RT);
    mmio_write32(IRQ_ENABLE_2, IRQ_UART);
    uint64_t daif = tx_lock_take();
    tx_buffered = true;
    tx_lock_give(daif);
}

// バッファの中身をすべて FIFO に移し、送り終わるまで待つ（止まる前の表示などで使う）
void uart_flush(void)
{
    if (!tx_buffered)
        return;
    uint64_t daif = tx_lock_take();
    while (tx_tail != tx_head) {
        while (tx_fifo_full())
            ;
        tx_fill_fifo();
    }
    tx_lock_give(daif);
    for (int i = 0; i < n_mirrors; i++)
        if (tx_mirror_flush[i])
            tx_mirror_flush[i]();
}

uint32_t uart_baud(void)
{
    return cur_baud;
}

// ボーレートを変える。送信バッファの中身を古い速さで送り切ってから切り替える。
// 分周比を変えた後は LCRH を書き直す（13.4 節: IBRD・FBRD は LCRH の書き込みで反映される）
bool uart_set_baud(uint32_t baud)
{
    uint64_t daif = 0;
    if (tx_buffered)
        daif = tx_lock_take();
    while (tx_tail != tx_head) {
        while (tx_fifo_full())
            ;
        tx_fill_fifo();
    }
    while (mmio_read32(UART0_FR) & FR_BUSY)
        ;
    uint32_t cr = mmio_read32(UART0_CR);
    mmio_write32(UART0_CR, 0);
    bool ok = set_divisor(baud);
    mmio_write32(UART0_LCRH, LCRH_FEN | LCRH_WLEN_8);
    mmio_write32(UART0_CR, cr);
    if (tx_buffered)
        tx_lock_give(daif);
    return ok;
}

// 受信バッファ。書くのは割り込み（uart_poll_rx）、読むのは uart_getc・uart_read。どのコアからも読むので、
// rx_lock（割り込みを止めて取るスピンロック）で守る。MMU を有効にする前は使わない（タイマーの後から）
#define RX_BUF_SIZE     (64 * 1024)
#define CTRL_C          0x03
#define CTRL_BACKSLASH  0x1C    // raw のときの中断（Linux の SIGQUIT と同じキー）
#define CTRL_S          0x13    // 出力を止める（XOFF。termios の IXON）
#define CTRL_Q          0x11    // 出力を再開する（XON）

static char rx_buf[RX_BUF_SIZE];
static uint32_t rx_head, rx_tail;
static uint64_t rx_dropped;
static uint64_t rx_errors;
static atomic_flag rx_lock = ATOMIC_FLAG_INIT;

static uint64_t rx_lock_take(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    while (atomic_flag_test_and_set_explicit(&rx_lock, memory_order_acquire))
        __asm__ volatile("yield");
    return daif;
}

static void rx_lock_give(uint64_t daif)
{
    atomic_flag_clear_explicit(&rx_lock, memory_order_release);
    write_sysreg(daif, daif);
}

static bool (*ctrl_c_handler)(void);
static volatile bool raw_keys;
static const char *interrupt_name = "^C";   // 最後にアプリを止めたキー（表示用）

void uart_set_raw(bool raw)
{
    raw_keys = raw;
}

const char *uart_interrupt_key_name(void)
{
    return interrupt_name;
}

static volatile bool interrupt_quit;     // 最後のキーが Ctrl-\（SIGQUIT にあたる。いつも止める）

// アプリを止めるキーか（普段は Ctrl-C と Ctrl-\、raw のときは Ctrl-\ だけ。raw の Ctrl-C は文字としてアプリに渡す）。
// Ctrl-C は、アプリが SIGINT に関数を登録していれば、止めずにそれを呼ぶ（os/kernel/app.c、os/libc/signal.c）。
// Ctrl-\ はいつも止める（Linux の SIGQUIT の既定と同じ。SIGINT を受けて計算し続けるアプリも止められるように）
// Ctrl-S・Ctrl-Q（2026-11-19。Linux の端末の IXON と同じ）: 流れている出力を止める・再開する。キーはアプリに渡さない。
// raw のとき（curses のアプリ。mi の Ctrl-S は保存）と、アプリが IXON を外したときは、ふつうのキー。
// 止めるのは、アプリとシェルの画面への出力だけ（os/libc/syscalls.c の screen_putc が待つ）。Ctrl-C などの中断のキーでも
// 再開する（Linux と同じ）
static volatile bool out_paused;

bool uart_output_paused(void)
{
    return out_paused;
}

// 流れの止め・再開のキーなら扱って true（受信バッファに入れない）
static bool flow_key(char c)
{
    if (raw_keys || (c != CTRL_S && c != CTRL_Q) || !term_ixon())
        return false;
    out_paused = c == CTRL_S;
    return true;
}

static bool is_interrupt_key(char c)
{
    if (c != CTRL_BACKSLASH && (raw_keys || c != CTRL_C))
        return false;
    interrupt_quit = c == CTRL_BACKSLASH;
    interrupt_name = interrupt_quit ? "^\\" : "^C";
    return true;
}

// アプリが動いていないときに押された Ctrl-C・Ctrl-\\ の数（シェルの for・while を止める。os/shell/shell.c）
static volatile uint32_t shell_interrupts;

uint32_t uart_shell_interrupts(void)
{
    return shell_interrupts;
}

bool uart_interrupt_is_quit(void)
{
    return interrupt_quit;
}

// Ctrl-\ を受けたのと同じにする（文字は入れない。SIGINT の関数があっても止める。ssh の exec の相手が
// いなくなったとき。os/kernel/rexec.c）
void uart_raise_interrupt(void)
{
    interrupt_quit = true;
    interrupt_name = "^\\";
    if (ctrl_c_handler && ctrl_c_handler())
        return;
    shell_interrupts++;
}

void uart_set_ctrl_c_handler(bool (*handler)(void))
{
    ctrl_c_handler = handler;
}

// FIFO を受信バッファに移す（rx_lock の中で）
static void rx_drain(void)
{
    while (!(mmio_read32(UART0_FR) & FR_RXFE)) {
        uint32_t dr = mmio_read32(UART0_DR);
        if (dr & DR_ERRORS) {
            rx_errors++;    // フレーミング・パリティ・ブレーク・オーバーラン（線を外したときの雑音など）は捨てる
            continue;
        }
        char c = (char)(dr & 0xFF);
        if (flow_key(c))
            continue;
        if (is_interrupt_key(c)) {
            out_paused = false;
            if (ctrl_c_handler && ctrl_c_handler())
                continue;
            shell_interrupts++;
        }
        uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next == rx_tail) {
            rx_dropped++;   // あふれた分は捨てる
            continue;
        }
        rx_buf[rx_head] = c;
        rx_head = next;
    }
}

void uart_rx_push(const char *p, size_t n)
{
    uint64_t daif = rx_lock_take();
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (flow_key(c))
            continue;
        if (is_interrupt_key(c)) {
            out_paused = false;
            if (ctrl_c_handler && ctrl_c_handler())
                continue;
            shell_interrupts++;
        }
        uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next == rx_tail) {
            rx_dropped++;
            continue;
        }
        rx_buf[rx_head] = c;
        rx_head = next;
    }
    rx_lock_give(daif);
}

void uart_rx_unread(const char *p, size_t n)
{
    // 受信バッファの先頭（次に読まれる位置の前）に、後ろから戻す
    uint64_t daif = rx_lock_take();
    for (size_t i = n; i-- > 0;) {
        uint32_t prev = (rx_tail + RX_BUF_SIZE - 1) % RX_BUF_SIZE;
        if (prev == rx_head)
            break;  // いっぱい
        rx_tail = prev;
        rx_buf[rx_tail] = p[i];
    }
    rx_lock_give(daif);
}

void uart_poll_rx(void)
{
    uint64_t daif = rx_lock_take();
    rx_drain();
    rx_lock_give(daif);
}

bool uart_has_input(void)
{
    return rx_head != rx_tail || !(mmio_read32(UART0_FR) & FR_RXFE);
}

void uart_wait_input(void)
{
    // 割り込みを止めて調べてから眠る（調べた後に届いた文字の割り込みでも起きる）
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    if (!uart_has_input())
        cpu_idle();
    write_sysreg(daif, daif);
    signal_point();     // 割り込みが入る状態に戻ったところで、届いたシグナルを渡す（os/libc/signal.c）
}

char uart_getc(void)
{
    for (;;) {
        uint64_t daif = rx_lock_take();
        rx_drain();
        if (rx_head != rx_tail) {
            char c = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
            rx_lock_give(daif);
            return c;
        }
        rx_lock_give(daif);
        uart_wait_input();
    }
}

size_t uart_read(void *buf, size_t n, uint64_t timeout_us)
{
    char *p = buf;
    size_t got = 0;
    uint64_t end = timer_now_us() + timeout_us;
    while (got < n) {
        size_t before = got;
        uint64_t daif = rx_lock_take();
        rx_drain();
        while (got < n && rx_head != rx_tail) {
            p[got++] = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        }
        rx_lock_give(daif);
        if (got > before)
            end = timer_now_us() + timeout_us;  // 届いている間は待ち続ける
        else if (timer_now_us() > end)
            break;
        else
            uart_wait_input();
    }
    return got;
}

uint64_t uart_rx_dropped(void)
{
    return rx_dropped;
}

void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
    }
}

void uart_puthex(uint64_t value)
{
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint32_t digit = (value >> shift) & 0xF;
        uart_putc(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
}

void uart_putdec(uint64_t value)
{
    char buf[20];
    int n = 0;
    do {
        buf[n++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (n > 0)
        uart_putc(buf[--n]);
}
