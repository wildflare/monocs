// i2c.c: I2C のマスター（monocs_i2c.h）。共有のライブラリ /lib/i2c
// 出典: BCM2835 ARM Peripherals 3 章（BSC。3.2 のレジスタ）、Linux の drivers/i2c/busses/i2c-bcm2835.c
#include "monocs_i2c.h"

#include <errno.h>
#include <stdint.h>
#include <monocs.h>
#include <monocs_gpio.h>

#define BSC0_BASE   0x3F205000UL    // バスアドレス 0x7E205000
#define BSC1_BASE   0x3F804000UL    // バスアドレス 0x7E804000（BSC2 0x7E805000 は HDMI 用で使わない）

// レジスタ（3.2）
#define BSC_C       0x00
#define BSC_S       0x04
#define BSC_DLEN    0x08
#define BSC_A       0x0C
#define BSC_FIFO    0x10
#define BSC_DIV     0x14
#define BSC_DEL     0x18
#define BSC_CLKT    0x1C

#define C_I2CEN     (1U << 15)
#define C_ST        (1U << 7)
#define C_CLEAR     (3U << 4)       // FIFO を空に
#define C_READ      (1U << 0)

#define S_CLKT      (1U << 9)       // SCL の引き伸ばしの時間切れ（1 を書いて消す）
#define S_ERR       (1U << 8)       // ACK が来なかった（1 を書いて消す）
#define S_RXD       (1U << 5)       // 受けた FIFO にデータがある
#define S_TXD       (1U << 4)       // 送る FIFO に空きがある
#define S_DONE      (1U << 1)       // 転送が終わった（1 を書いて消す）
#define S_TA        (1U << 0)       // 転送の途中

// BSC のクロック（コアのクロック。config.txt の force_turbo=1 で最大の 400MHz に固定。clock コマンドの CORE）
#define CORE_HZ     400000000U

typedef struct {
    uintptr_t base;
    int sda, scl;
    int hz;             // 0 = まだ開いていない（このアプリの中で）
} Bus;

static Bus buses[2] = {
    {BSC0_BASE, 0, 1, 0},
    {BSC1_BASE, 2, 3, 0},
};

static inline volatile uint32_t *reg(Bus *b, unsigned off)
{
    return (volatile uint32_t *)(b->base + off);
}

static Bus *get_bus(int bus)
{
    if (bus != 0 && bus != 1) {
        errno = EINVAL;
        return nullptr;
    }
    Bus *b = &buses[bus];
    if (b->hz == 0 && i2c_open(bus, 0) < 0)
        return nullptr;
    return b;
}

int i2c_open(int bus, int hz)
{
    if (bus != 0 && bus != 1) {
        errno = EINVAL;
        return -1;
    }
    if (hz == 0)
        hz = 100000;
    if (hz < 10000 || hz > 1000000) {
        errno = EINVAL;
        return -1;
    }
    Bus *b = &buses[bus];
    // Linux の bcm2835_i2c_set_divider と同じ: 切り上げて偶数に。立ち下がり・立ち上がりからデータを変える・読むまでの遅れは
    // 分周の 1/16・1/4（DEL の FEDL・REDL）。CLKT は 35ms 分の SCL のクロック
    uint32_t div = (CORE_HZ + (uint32_t)hz - 1) / (uint32_t)hz;
    if (div & 1)
        div++;
    if (div < 2)
        div = 2;
    if (div > 0xFFFE)
        div = 0xFFFE;
    uint32_t fedl = div / 16 > 1 ? div / 16 : 1;
    uint32_t redl = div / 4 > 1 ? div / 4 : 1;
    uint32_t real_hz = CORE_HZ / div;
    uint32_t tout = 35 * real_hz / 1000;
    if (tout > 0xFFFF)
        tout = 0xFFFF;
    *reg(b, BSC_C) = 0;
    *reg(b, BSC_DIV) = div;
    *reg(b, BSC_DEL) = (fedl << 16) | redl;
    *reg(b, BSC_CLKT) = tout;
    *reg(b, BSC_S) = S_CLKT | S_ERR | S_DONE;
    *reg(b, BSC_C) = C_CLEAR;
    // ピンを ALT0（SDA0・SCL0、SDA1・SCL1）に。プルアップも（バス 1 は基板にもある）
    gpio_pull(b->sda, GPIO_PULL_UP);
    gpio_pull(b->scl, GPIO_PULL_UP);
    if (gpio_mode(b->sda, GPIO_ALT0) < 0 || gpio_mode(b->scl, GPIO_ALT0) < 0)
        return -1;
    b->hz = (int)real_hz;
    return 0;
}

int i2c_close(int bus)
{
    if (bus != 0 && bus != 1) {
        errno = EINVAL;
        return -1;
    }
    Bus *b = &buses[bus];
    *reg(b, BSC_C) = C_CLEAR;
    *reg(b, BSC_S) = S_CLKT | S_ERR | S_DONE;
    gpio_mode(b->sda, GPIO_IN);
    gpio_mode(b->scl, GPIO_IN);
    b->hz = 0;
    return 0;
}

// 転送の時間の上限（マイクロ秒）: バイトごとに 9 クロックの 2 倍と、引き伸ばしの 35ms ＋ 余り
static uint64_t limit_us(Bus *b, int bytes)
{
    return (uint64_t)(bytes + 2) * 18 * 1000000 / (uint64_t)b->hz + 50000;
}

// 終わった後の始末と結果。got は送った・受けたバイト数
static int finish(Bus *b, uint32_t s, int timed_out, int got, int want)
{
    if (s & (S_ERR | S_CLKT) || timed_out) {
        // 止まるまで待つ（ERR・CLKT のときもコントローラは STOP を出して DONE になる）
        uint64_t t0 = timer_now_us();
        while ((*reg(b, BSC_S) & S_TA) && timer_now_us() - t0 < 10000)
            ;
    }
    *reg(b, BSC_C) = C_CLEAR;
    *reg(b, BSC_S) = S_CLKT | S_ERR | S_DONE;
    if (s & S_ERR) {
        errno = ENXIO;
        return -1;
    }
    if (s & S_CLKT || timed_out) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (got != want) {
        errno = EIO;
        return -1;
    }
    return want;
}

// 転送を始める。送るものは、始めた後に FIFO に入れる（送る FIFO は、始めると空きを待ってから送る。
// 始める前に入れても実機は送るが、QEMU の BSC は転送の外の FIFO への書き込みを捨てるので、どちらでも動く形にする）
static void start(Bus *b, int addr, int len, bool read)
{
    uint32_t rd = read ? C_READ : 0;
    *reg(b, BSC_C) = C_CLEAR | rd;      // 止めたまま FIFO を空に（I2CEN を立てない）
    *reg(b, BSC_S) = S_CLKT | S_ERR | S_DONE;
    *reg(b, BSC_A) = (uint32_t)addr;
    *reg(b, BSC_DLEN) = (uint32_t)len;
    *reg(b, BSC_C) = C_I2CEN | C_ST | rd;
}

static int check_args(int addr, int len, const void *p)
{
    if (addr < 0 || addr > 0x7F || len < 0 || len > 0xFFFF || (len > 0 && !p)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

// 送る。head が 0 以上なら、その 1 バイトを先に（レジスタの番号。間に STOP を入れない）
static int transmit(Bus *b, int addr, int head, const uint8_t *p, int len)
{
    int n = len + (head >= 0), i = 0;
    start(b, addr, n, false);
    if (head >= 0)
        *reg(b, BSC_FIFO) = (uint8_t)head;     // FIFO は空なので待たずに入る
    uint64_t t0 = timer_now_us(), lim = limit_us(b, n);
    uint32_t s;
    int timed_out = 0;
    for (;;) {
        s = *reg(b, BSC_S);
        if (s & (S_ERR | S_CLKT | S_DONE))
            break;
        while (i < len && (*reg(b, BSC_S) & S_TXD))
            *reg(b, BSC_FIFO) = p[i++];
        if (timer_now_us() - t0 > lim) {
            timed_out = 1;
            break;
        }
    }
    return finish(b, s, timed_out, i, len);
}

int i2c_write(int bus, int addr, const void *data, int len)
{
    Bus *b = get_bus(bus);
    if (!b || check_args(addr, len, data) < 0)
        return -1;
    return transmit(b, addr, -1, data, len);
}

// 受ける（start と C_ST|C_READ の後）
static int receive(Bus *b, uint8_t *p, int len)
{
    int i = 0;
    uint64_t t0 = timer_now_us(), lim = limit_us(b, len);
    uint32_t s;
    int timed_out = 0;
    for (;;) {
        s = *reg(b, BSC_S);
        while (i < len && (s & S_RXD)) {
            p[i++] = (uint8_t)*reg(b, BSC_FIFO);
            s = *reg(b, BSC_S);
        }
        if (s & (S_ERR | S_CLKT))
            break;
        if (s & S_DONE) {
            while (i < len && (*reg(b, BSC_S) & S_RXD))  // DONE の後に残ったもの
                p[i++] = (uint8_t)*reg(b, BSC_FIFO);
            break;
        }
        if (timer_now_us() - t0 > lim) {
            timed_out = 1;
            break;
        }
    }
    return finish(b, s, timed_out, i, len);
}

int i2c_read(int bus, int addr, void *data, int len)
{
    Bus *b = get_bus(bus);
    if (!b || check_args(addr, len, data) < 0)
        return -1;
    start(b, addr, len, true);
    return receive(b, data, len);
}

int i2c_write_read(int bus, int addr, const void *wdata, int wlen, void *rdata, int rlen)
{
    Bus *b = get_bus(bus);
    if (!b || check_args(addr, rlen, rdata) < 0)
        return -1;
    if (wlen < 1 || wlen > I2C_FIFO || !wdata || rlen < 1) {
        errno = EINVAL;
        return -1;
    }
    const uint8_t *w = wdata;
    // 書きが始まった（TA）のを見てから、終わる前に読みを始めると、STOP の代わりにリピーテッドスタートになる（3.3 の
    // 10 ビットのアドレスの読み出しの手順）。その間に割り込みで遅れて書きが終わってしまわないよう、割り込みを止める
    // （書くものは 16 バイトまでなので FIFO に全部入り、1 バイトでも 18 クロック以上かかる）
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) : : "memory");
    start(b, addr, wlen, false);
    uint64_t t0 = timer_now_us();
    uint32_t s;
    int ok = 0;
    for (;;) {
        s = *reg(b, BSC_S);
        if (s & (S_ERR | S_CLKT | S_DONE))
            break;              // 相手が応答しない（ERR）か、もう終わってしまった
        if (s & S_TA) {
            ok = 1;
            break;
        }
        if (timer_now_us() - t0 > 2000)    // 割り込みを止めている間なので短く（10kHz でも 1 クロックは 100µs）
            break;
    }
    if (ok) {
        for (int i = 0; i < wlen; i++)
            *reg(b, BSC_FIFO) = w[i];
        s = *reg(b, BSC_S);
        if (s & S_ERR)
            ok = 0;
    }
    if (ok) {
        *reg(b, BSC_DLEN) = (uint32_t)rlen;
        *reg(b, BSC_C) = C_I2CEN | C_ST | C_READ;
    }
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
    if (!ok) {
        // 書きのところで止まった（NACK・引き伸ばしの時間切れ・TA を見る前に終わった）。終わるのを待って理由を返す
        uint64_t t1 = timer_now_us();
        while (!(s & (S_ERR | S_CLKT | S_DONE)) && timer_now_us() - t1 < limit_us(b, wlen))
            s = *reg(b, BSC_S);
        return finish(b, s, !(s & (S_ERR | S_CLKT | S_DONE)), 0, rlen);
    }
    // 読みの前の書きで NACK だったときも、ERR が立つ
    return receive(b, rdata, rlen);
}

int i2c_read_reg(int bus, int addr, int reg_)
{
    uint8_t r = (uint8_t)reg_, v;
    if (i2c_write_read(bus, addr, &r, 1, &v, 1) < 0)
        return -1;
    return v;
}

int i2c_write_reg(int bus, int addr, int reg_, int value)
{
    uint8_t v = (uint8_t)value;
    return i2c_write_regs(bus, addr, reg_, &v, 1) < 0 ? -1 : 0;
}

int i2c_read_regs(int bus, int addr, int reg_, void *buf, int len)
{
    uint8_t r = (uint8_t)reg_;
    return i2c_write_read(bus, addr, &r, 1, buf, len);
}

int i2c_write_regs(int bus, int addr, int reg_, const void *buf, int len)
{
    Bus *b = get_bus(bus);
    if (!b || check_args(addr, len, buf) < 0)
        return -1;
    if (len == 0xFFFF) {
        errno = EINVAL;
        return -1;
    }
    return transmit(b, addr, reg_ & 0xFF, buf, len);
}

int i2c_probe(int bus, int addr)
{
    uint8_t v;
    if (i2c_read(bus, addr, &v, 1) == 1)
        return 1;
    return errno == ENXIO ? 0 : -1;
}
