// softuart.c: ソフトウェアの UART（monocs_softuart.h）。共有のライブラリ /lib/softuart
// 時間は汎用タイマーの数（CNTPCT_EL0。実機 19.2MHz、1 カウント 52ns。周波数は CNTFRQ_EL0）で測り、
// ビットの境目は始めからの通し番号で計算する（ずれが積もらない）。受信はスタートビットの立ち下がりを見つけ、
// 各ビットの真ん中（1.5、2.5、… ビット後）で読む。
// 出典: Arm Architecture Reference Manual（CNTPCT_EL0・CNTFRQ_EL0）
#include "monocs_softuart.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <monocs.h>
#include <monocs_gpio.h>

#define RING 4096

typedef struct {
    int used;
    int tx, rx;
    uint64_t bit_q16;           // 1 ビットのカウント数 × 65536
    int core;                   // 受信のコア（-1 = なし）
    atomic_int stop;
    atomic_uint head, tail;     // head は受信の側だけ、tail は読む側だけが書く
    atomic_uint errors;
    uint8_t ring[RING];
} Port;

static Port ports[SOFTUART_MAX];

static inline uint64_t ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t tick_freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t at(Port *p, uint64_t t0, uint64_t half_bits)
{
    return t0 + ((half_bits * p->bit_q16) >> 17);  // half_bits / 2 ビット後
}

static inline void wait_until(uint64_t t)
{
    while ((int64_t)(ticks() - t) < 0)
        ;
}

static Port *get(int u)
{
    if (u < 0 || u >= SOFTUART_MAX || !ports[u].used) {
        errno = EINVAL;
        return nullptr;
    }
    return &ports[u];
}

static void push(Port *p, uint8_t v)
{
    unsigned h = atomic_load_explicit(&p->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&p->tail, memory_order_acquire);
    if (h - t >= RING) {
        atomic_fetch_add(&p->errors, 1);
        return;
    }
    p->ring[h % RING] = v;
    atomic_store_explicit(&p->head, h + 1, memory_order_release);
}

// 1 バイト受ける。until（カウント）までにスタートビットが来なければ 0。受けたら 1
static int rx_byte(Port *p, uint64_t until, bool use_until)
{
    int pin = p->rx;
    // 休み（High）を見てから、立ち下がりを待つ
    while (gpio_read(pin) == 0) {
        if ((use_until && (int64_t)(ticks() - until) >= 0) || atomic_load_explicit(&p->stop, memory_order_relaxed))
            return 0;
    }
    for (;;) {
        if (gpio_read(pin) == 0)
            break;
        if ((use_until && (int64_t)(ticks() - until) >= 0) || atomic_load_explicit(&p->stop, memory_order_relaxed))
            return 0;
    }
    uint64_t t0 = ticks();
    wait_until(at(p, t0, 1));                // スタートビットの真ん中
    if (gpio_read(pin) != 0)
        return 0;                            // 雑音（短すぎる Low）
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        wait_until(at(p, t0, 3 + 2 * (uint64_t)i));
        v |= (unsigned)gpio_read(pin) << i;
    }
    wait_until(at(p, t0, 19));               // ストップビットの真ん中
    if (gpio_read(pin) == 0) {
        atomic_fetch_add(&p->errors, 1);     // フレームの誤り（ブレークも）
        return 1;
    }
    push(p, (uint8_t)v);
    return 1;
}

static void rx_loop(void *arg)
{
    Port *p = arg;
    while (!atomic_load_explicit(&p->stop, memory_order_relaxed))
        rx_byte(p, 0, false);
}

int softuart_open(int tx_pin, int rx_pin, int baud)
{
    if (baud < SOFTUART_BAUD_MIN || baud > SOFTUART_BAUD_MAX || (tx_pin < 0 && rx_pin < 0) ||
        (tx_pin >= 0 && !gpio_pin_ok(tx_pin)) || (rx_pin >= 0 && !gpio_pin_ok(rx_pin))) {
        errno = EINVAL;
        return -1;
    }
    int u = 0;
    while (u < SOFTUART_MAX && ports[u].used)
        u++;
    if (u == SOFTUART_MAX) {
        errno = EMFILE;
        return -1;
    }
    Port *p = &ports[u];
    p->tx = tx_pin;
    p->rx = rx_pin;
    p->bit_q16 = (tick_freq() << 16) / (uint64_t)baud;
    p->core = -1;
    atomic_store(&p->stop, 0);
    atomic_store(&p->head, 0);
    atomic_store(&p->tail, 0);
    atomic_store(&p->errors, 0);
    if (tx_pin >= 0) {
        gpio_write(tx_pin, 1);              // 休みは High
        gpio_mode(tx_pin, GPIO_OUT);
    }
    if (rx_pin >= 0 && rx_pin != tx_pin) {
        gpio_mode(rx_pin, GPIO_IN);
        gpio_pull(rx_pin, GPIO_PULL_UP);   // つながっていないときに休み（High）に見えるよう
    }
    p->used = 1;
    if (rx_pin >= 0) {
        // 空いたコアを後ろから探す（前のコアは pthread・アプリが使うことが多い）
        for (int c = monocs_cores() - 1; c >= 1; c--) {
            if (monocs_core_start(c, rx_loop, p)) {
                p->core = c;
                break;
            }
        }
    }
    return u;
}

int softuart_close(int u)
{
    Port *p = get(u);
    if (!p)
        return -1;
    atomic_store(&p->stop, 1);
    if (p->core >= 0)
        monocs_core_wait(p->core);
    if (p->tx >= 0)
        gpio_mode(p->tx, GPIO_IN);
    if (p->rx >= 0 && p->rx != p->tx)
        gpio_pull(p->rx, p->rx <= 8 ? GPIO_PULL_UP : GPIO_PULL_DOWN);  // 電源を入れたときの既定に戻す
    p->used = 0;
    return 0;
}

int softuart_write(int u, const void *data, int len)
{
    Port *p = get(u);
    if (!p)
        return -1;
    if (p->tx < 0 || len < 0 || (len > 0 && !data)) {
        errno = EINVAL;
        return -1;
    }
    const uint8_t *d = data;
    for (int k = 0; k < len; k++) {
        // スタート（Low）、データ 8 ビット（下位から）、ストップ（High）。1 バイトの間は割り込みを止める
        unsigned frame = (unsigned)d[k] << 1 | 1U << 9;
        uint64_t daif;
        __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) : : "memory");
        uint64_t t0 = ticks();
        for (int i = 0; i < 10; i++) {
            gpio_write(p->tx, frame >> i & 1);
            wait_until(at(p, t0, 2 * (uint64_t)(i + 1)));
        }
        __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
    }
    return len;
}

int softuart_putc(int u, int c)
{
    uint8_t b = (uint8_t)c;
    return softuart_write(u, &b, 1) == 1 ? (int)b : -1;
}

int softuart_available(int u)
{
    Port *p = get(u);
    if (!p)
        return -1;
    return (int)(atomic_load_explicit(&p->head, memory_order_acquire) - atomic_load_explicit(&p->tail, memory_order_relaxed));
}

int softuart_read(int u, void *buf, int len, int timeout_ms)
{
    Port *p = get(u);
    if (!p)
        return -1;
    if (p->rx < 0 || len < 0 || (len > 0 && !buf)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t start = ticks();
    uint64_t limit = timeout_ms < 0 ? 0 : (uint64_t)timeout_ms * (tick_freq() / 1000);
    for (;;) {
        unsigned h = atomic_load_explicit(&p->head, memory_order_acquire);
        unsigned t = atomic_load_explicit(&p->tail, memory_order_relaxed);
        if (h != t || len == 0) {
            int n = 0;
            uint8_t *b = buf;
            while (n < len && t != h)
                b[n++] = p->ring[t++ % RING];
            atomic_store_explicit(&p->tail, t, memory_order_release);
            return n;
        }
        bool timed_out = timeout_ms >= 0 && ticks() - start >= limit;
        if (timed_out)
            return 0;
        if (p->core < 0) {
            // 裏のコアがない: ここで受ける（時間切れまで）
            rx_byte(p, start + limit, timeout_ms >= 0);
        }
    }
}

int softuart_getc(int u, int timeout_ms)
{
    uint8_t b;
    int n = softuart_read(u, &b, 1, timeout_ms);
    if (n == 1)
        return b;
    if (n == 0)
        errno = ETIMEDOUT;
    return -1;
}

unsigned softuart_errors(int u)
{
    Port *p = get(u);
    if (!p)
        return 0;
    return atomic_exchange(&p->errors, 0);
}

int softuart_rx_background(int u)
{
    Port *p = get(u);
    if (!p)
        return -1;
    return p->core >= 0;
}
