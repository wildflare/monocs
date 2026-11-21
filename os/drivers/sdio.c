// SDIO（内蔵 Wi-Fi のチップ CYW43436 とつなぐ。EMMC コントローラ = Arasan SDHCI、ポーリング、PIO）
// 出典: SDIO Simplified Specification 3.00（3 章 初期化、5 章 CMD52・CMD53、6.9 節 CCCR）
//       SD Host Controller Simplified Specification 3.00、BCM2835 ARM Peripherals 5 章（EMMC）
//       ピン: Zero 2 W のデバイスツリー（GPIO34〜39 を ALT3、WL_ON は GPIO41）
//
// SD カードは SDHOST（os/drivers/sdhost.c）で使い、EMMC は GPIO34〜39 で Wi-Fi のチップにつなぐ。SD カードが
// EMMC の予備のドライバで動いているとき（sd_emmc_free() が false）は使えない。
// レジスタの書き方（書いた後に SD クロック 2 周期待つ）とクロックの決め方は os/drivers/sd_emmc.c と同じ。

#include <string.h>
#include "sdio.h"
#include "sd.h"
#include "bcm2837.h"
#include "mailbox.h"
#include "mmio.h"
#include "timer.h"

#define INT_CMD_DONE        (1U << 0)
#define INT_DATA_DONE       (1U << 1)
#define INT_WRITE_RDY       (1U << 4)
#define INT_READ_RDY        (1U << 5)
#define INT_CARD            (1U << 8)
#define INT_ERROR_MASK      0xFFFF8000U
#define SR_CMD_INHIBIT      (1U << 0)
#define SR_DAT_INHIBIT      (1U << 1)
#define C0_HCTL_DWIDTH      (1U << 1)
#define C1_CLK_INTLEN       (1U << 0)
#define C1_CLK_STABLE       (1U << 1)
#define C1_CLK_EN           (1U << 2)
#define C1_CLK_FREQ_MASK    0xFFE0U
#define C1_DATA_TOUNIT_MAX  (0xEU << 16)
#define C1_SRST_HC          (1U << 24)
#define C1_SRST_CMD         (1U << 25)
#define C1_SRST_DATA        (1U << 26)
#define TM_BLKCNT_EN        (1U << 1)
#define TM_DAT_DIR_READ     (1U << 4)
#define TM_MULTI_BLOCK      (1U << 5)
#define CMD_RSPNS_NONE      (0U << 16)
#define CMD_RSPNS_48        (2U << 16)
#define CMD_RSPNS_48BUSY    (3U << 16)
#define CMD_RSPNS_MASK      (3U << 16)
#define CMD_CRCCHK_EN       (1U << 19)
#define CMD_IXCHK_EN        (1U << 20)
#define CMD_ISDATA          (1U << 21)
#define CMD_INDEX(n)        ((uint32_t)(n) << 24)
#define HOST_SPEC_V3        2

#define RESP_R1             (CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define RESP_R1B            (CMD_RSPNS_48BUSY | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define RESP_R4             CMD_RSPNS_48                    // CRC・番号なし
#define RESP_R5             (CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN)

#define CMD_GO_IDLE_STATE   (CMD_INDEX(0) | CMD_RSPNS_NONE)
#define CMD_SEND_RCA        (CMD_INDEX(3) | RESP_R1)        // SDIO では R6
#define CMD_IO_SEND_OP_COND (CMD_INDEX(5) | RESP_R4)
#define CMD_SELECT_CARD     (CMD_INDEX(7) | RESP_R1B)
#define CMD_IO_RW_DIRECT    (CMD_INDEX(52) | RESP_R5)
#define CMD_IO_RW_EXTENDED  (CMD_INDEX(53) | RESP_R5 | CMD_ISDATA)

// CMD5 の応答（R4）: 31 = 準備完了、30:28 = 機能の数、23:0 = OCR
#define R4_READY            (1U << 31)
#define OCR_3V3             0x00300000U     // 3.2〜3.4V
// R5 の応答のフラグ（15:8）: COM_CRC_ERROR、ILLEGAL_COMMAND、ERROR、FUNCTION_NUMBER、OUT_OF_RANGE
#define R5_ERRORS           0xCB00U

// CCCR（機能 0。SDIO 仕様 6.9 節）
#define CCCR_IOEN           0x02
#define CCCR_IORDY          0x03
#define CCCR_IENX            0x04        // Int Enable: ビット 0 = 全体、ビット n = 機能 n
#define CCCR_IEN_MASTER     0x01
#define CCCR_IEN_FN(n)      (1U << (n))
#define CCCR_BUS_IF         0x07
#define CCCR_BUS_WIDTH_4    0x02
// Bus Speed Select（SDIO 仕様の CCCR 0x13。Linux の SDIO_CCCR_SPEED）: ビット 0 SHS = High Speed に対応、
// ビット 1 EHS = High Speed にする（SDR25、50MHz まで）
#define CCCR_SPEED          0x13
#define CCCR_SPEED_SHS      0x01
#define CCCR_SPEED_EHS      0x02
#define FBR_BLKSIZE(fn)     (0x100 * (fn) + 0x10)

#define WL_ON_GPIO          41
#define CLOCK_ID_HZ         400000
#define CLOCK_NORMAL_HZ     25000000
#define CLOCK_HIGH_HZ       50000000
#define TIMEOUT_CMD_US      100000
#define TIMEOUT_RESET_US    100000
#define TIMEOUT_INIT_US     1000000

static uint32_t base_clock;
static bool want_high_speed = true;  // 2026-11-22: High Speed（50MHz）を試す。失敗したら wifi.c が切ってやり直す
static uint32_t clock_hz;
void sdio_set_high_speed(bool on) { want_high_speed = on; }
uint32_t sdio_clock_hz(void) { return clock_hz; }
static uint32_t write_gap_us = 10;
static uint32_t rca;
static bool ready;
static uint32_t blksize[8];
static uint32_t ocr_resp;

static struct sdio_perf perf;
const struct sdio_perf *sdio_get_perf(void) { return &perf; }
void sdio_perf_reset(void) { memset(&perf, 0, sizeof perf); }

static void emmc_write(uintptr_t reg, uint32_t value)
{
    mmio_write32(reg, value);
    timer_delay_us(write_gap_us);
}

static bool wait_reg(uintptr_t reg, uint32_t mask, bool set, uint64_t timeout_us)
{
    uint64_t end = timer_now_us() + timeout_us;
    while (((mmio_read32(reg) & mask) != 0) != set) {
        if (timer_now_us() > end)
            return false;
    }
    return true;
}

static uint32_t wait_interrupt(uint32_t mask, uint64_t timeout_us)
{
    uint64_t end = timer_now_us() + timeout_us;
    uint32_t irq;
    while (!((irq = mmio_read32(EMMC_INTERRUPT)) & (mask | INT_ERROR_MASK))) {
        if (timer_now_us() > end)
            return 0;
    }
    if (irq & INT_ERROR_MASK)
        return 0;
    emmc_write(EMMC_INTERRUPT, irq & mask);
    return irq & mask;
}

static void reset_lines(uint32_t bits)
{
    emmc_write(EMMC_CONTROL1, mmio_read32(EMMC_CONTROL1) | bits);
    wait_reg(EMMC_CONTROL1, bits, false, TIMEOUT_RESET_US);
}

static bool command(uint32_t cmd, uint32_t arg, uint32_t *resp)
{
    uint32_t inhibit = SR_CMD_INHIBIT;
    if ((cmd & CMD_ISDATA) || (cmd & CMD_RSPNS_MASK) == CMD_RSPNS_48BUSY)
        inhibit |= SR_DAT_INHIBIT;
    if (!wait_reg(EMMC_STATUS, inhibit, false, TIMEOUT_CMD_US))
        return false;
    emmc_write(EMMC_INTERRUPT, 0xFFFFFFFF);
    emmc_write(EMMC_ARG1, arg);
    emmc_write(EMMC_CMDTM, cmd);
    if (!wait_interrupt(INT_CMD_DONE, TIMEOUT_CMD_US)) {
        emmc_write(EMMC_INTERRUPT, 0xFFFFFFFF);
        reset_lines(C1_SRST_CMD | ((cmd & CMD_ISDATA) ? C1_SRST_DATA : 0));
        return false;
    }
    if ((cmd & CMD_RSPNS_MASK) == CMD_RSPNS_48BUSY && !wait_interrupt(INT_DATA_DONE, TIMEOUT_CMD_US))
        return false;
    if (resp)
        *resp = mmio_read32(EMMC_RESP0);
    return true;
}

static bool set_clock(uint32_t hz)
{
    if (!wait_reg(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, false, TIMEOUT_CMD_US))
        return false;
    uint32_t c1 = mmio_read32(EMMC_CONTROL1) & ~C1_CLK_EN;
    emmc_write(EMMC_CONTROL1, c1);
    uint32_t version = (mmio_read32(EMMC_SLOTISR_VER) >> 16) & 0xFF;
    uint32_t bits, actual;
    if (version >= HOST_SPEC_V3) {
        uint32_t div = (base_clock + 2 * hz - 1) / (2 * hz);
        if (div > 0x3FF)
            div = 0x3FF;
        bits = ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
        actual = div ? base_clock / (2 * div) : base_clock;
    } else {
        uint32_t div = 1;
        while (div < 256 && base_clock / div > hz)
            div <<= 1;
        bits = (div >> 1) << 8;
        actual = base_clock / div;
    }
    c1 = (c1 & ~C1_CLK_FREQ_MASK) | bits | C1_CLK_INTLEN;
    emmc_write(EMMC_CONTROL1, c1);
    if (!wait_reg(EMMC_CONTROL1, C1_CLK_STABLE, true, TIMEOUT_RESET_US))
        return false;
    emmc_write(EMMC_CONTROL1, c1 | C1_CLK_EN);
    write_gap_us = (2 * 1000000 + actual - 1) / actual;
    clock_hz = actual;
    return true;
}

static void set_pull(uint32_t pud, uint32_t mask1)
{
    mmio_write32(GPPUD, pud);
    delay_cycles(150);
    mmio_write32(GPPUDCLK1, mask1);
    delay_cycles(150);
    mmio_write32(GPPUD, 0);
    mmio_write32(GPPUDCLK1, 0);
}

static void gpio_init(void)
{
    // GPIO34〜39 を ALT3（EMMC）に。GPFSEL3 の 3 ビットずつ（34 は 12 ビット目から）
    const uint32_t alt3 = 7;
    uint32_t sel3 = mmio_read32(GPFSEL3);
    for (int pin = 34; pin <= 39; pin++) {
        int sh = (pin - 30) * 3;
        sel3 = (sel3 & ~(7U << sh)) | (alt3 << sh);
    }
    mmio_write32(GPFSEL3, sel3);
    // CLK（34）はプルなし、CMD・DAT0〜3（35〜39）はプルアップ（デバイスツリーの sdio_ovl_pins と同じ）
    set_pull(0, 1U << (34 - 32));
    set_pull(2, 0x1FU << (35 - 32));

    // WL_ON（GPIO41）を出力にして、いったん Low にしてから High にする（チップをリセットし直す）
    uint32_t sel4 = mmio_read32(GPFSEL4);
    sel4 = (sel4 & ~(7U << 3)) | (1U << 3);
    mmio_write32(GPFSEL4, sel4);
    mmio_write32(GPCLR1, 1U << (WL_ON_GPIO - 32));
    timer_delay_us(20000);
    mmio_write32(GPSET1, 1U << (WL_ON_GPIO - 32));
    timer_delay_us(150000);     // Linux の mmc-pwrseq-simple の post-power-on-delay-ms（Pi のデバイスツリー）
}

bool sdio_rw_direct(bool write, int fn, uint32_t addr, uint8_t in, uint8_t *out)
{
    perf.cmd52++;
    if (!ready)
        return false;
    uint32_t arg = (write ? 1U << 31 : 0) | ((uint32_t)fn << 28) | ((addr & 0x1FFFF) << 9) | in;
    if (write && out)
        arg |= 1U << 27;    // RAW: 書いた後の値を読み返す
    uint32_t r;
    uint64_t t0 = timer_count();
    bool ok = command(CMD_IO_RW_DIRECT, arg, &r) && !(r & R5_ERRORS);
    perf.t_cmd += timer_count() - t0;
    if (!ok)
        return false;
    if (out)
        *out = (uint8_t)r;
    return true;
}

static bool cmd52_init(bool write, int fn, uint32_t addr, uint8_t in, uint8_t *out)
{
    bool saved = ready;
    ready = true;
    bool ok = sdio_rw_direct(write, fn, addr, in, out);
    ready = saved;
    return ok;
}

// CMD53。len がその機能のブロックの大きさより大きければブロックモード（len はその倍数）、そうでなければ
// バイトモード（1〜512 バイト）。incr は番地を進めるか（false は同じ番地 = FIFO）。len は 4 の倍数
bool sdio_rw_extended(bool write, int fn, uint32_t addr, bool incr, void *buf, uint32_t len)
{
    if (!ready || len == 0 || (len & 3))
        return false;
    uint32_t bs = blksize[fn] ? blksize[fn] : 64;
    bool block = len > bs;
    uint32_t size = block ? bs : len, count = block ? len / bs : 1;
    if (block && (len % bs || count > 511))
        return false;
    uint32_t arg = (write ? 1U << 31 : 0) | ((uint32_t)fn << 28) | (block ? 1U << 27 : 0) |
                   (incr ? 1U << 26 : 0) | ((addr & 0x1FFFF) << 9) | ((block ? count : size) & 0x1FF);
    uint32_t cmd = CMD_IO_RW_EXTENDED | (write ? 0 : TM_DAT_DIR_READ);
    if (block)
        cmd |= TM_BLKCNT_EN | (count > 1 ? TM_MULTI_BLOCK : 0);

    perf.cmd53++;
    perf.bytes += len;
    uint64_t t0 = timer_count();
    if (!wait_reg(EMMC_STATUS, SR_DAT_INHIBIT, false, TIMEOUT_CMD_US))
        return false;
    emmc_write(EMMC_BLKSIZECNT, (count << 16) | size);
    uint32_t r;
    if (!command(cmd, arg, &r) || (r & R5_ERRORS))
        goto error;
    uint64_t t1 = timer_count();
    perf.t_cmd += t1 - t0;
    uint8_t *p = buf;
    for (uint32_t b = 0; b < count; b++) {
        if (!wait_interrupt(write ? INT_WRITE_RDY : INT_READ_RDY, TIMEOUT_CMD_US))
            goto error;
        uint64_t t2 = timer_count();
        perf.t_ready += t2 - t1;
        for (uint32_t i = 0; i < size; i += 4, p += 4) {
            if (write) {
                mmio_write32(EMMC_DATA, p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
            } else {
                uint32_t w = mmio_read32(EMMC_DATA);
                p[0] = (uint8_t)w;
                p[1] = (uint8_t)(w >> 8);
                p[2] = (uint8_t)(w >> 16);
                p[3] = (uint8_t)(w >> 24);
            }
        }
        t1 = timer_count();
        perf.t_pio += t1 - t2;
    }
    if (!wait_interrupt(INT_DATA_DONE, TIMEOUT_CMD_US))
        goto error;
    perf.t_done += timer_count() - t1;
    return true;

error:
    emmc_write(EMMC_INTERRUPT, 0xFFFFFFFF);
    reset_lines(C1_SRST_CMD | C1_SRST_DATA);
    return false;
}

bool sdio_set_block_size(int fn, uint32_t size)
{
    if (!sdio_rw_direct(true, 0, FBR_BLKSIZE(fn), size & 0xFF, nullptr) ||
        !sdio_rw_direct(true, 0, FBR_BLKSIZE(fn) + 1, (size >> 8) & 0xFF, nullptr))
        return false;
    blksize[fn] = size;
    return true;
}

bool sdio_enable_function(int fn, bool on, uint32_t timeout_us)
{
    uint8_t en;
    if (!sdio_rw_direct(false, 0, CCCR_IOEN, 0, &en))
        return false;
    en = on ? en | (uint8_t)(1U << fn) : en & (uint8_t)~(1U << fn);
    if (!sdio_rw_direct(true, 0, CCCR_IOEN, en, nullptr))
        return false;
    if (!on)
        return true;
    uint64_t end = timer_now_us() + timeout_us;
    for (;;) {
        uint8_t rdy;
        if (sdio_rw_direct(false, 0, CCCR_IORDY, 0, &rdy) && (rdy & (1U << fn)))
            return true;
        if (timer_now_us() > end)
            return false;
        timer_delay_us(1000);
    }
}

static const char *init_error;

static bool fail(const char *what)
{
    init_error = what;
    return false;
}

bool sdio_init(void)
{
    ready = false;
    init_error = nullptr;
    if (!sd_emmc_free())
        return fail("EMMC is used by the SD card");
    base_clock = mailbox_get_clock_rate(MBOX_CLOCK_EMMC);
    if (base_clock == 0)
        return fail("get EMMC clock");
    gpio_init();

    write_gap_us = 10;
    emmc_write(EMMC_CONTROL0, 0);
    emmc_write(EMMC_CONTROL1, C1_SRST_HC);
    if (!wait_reg(EMMC_CONTROL1, C1_SRST_HC, false, TIMEOUT_RESET_US))
        return fail("reset");
    emmc_write(EMMC_CONTROL1, C1_CLK_INTLEN | C1_DATA_TOUNIT_MAX);
    if (!set_clock(CLOCK_ID_HZ))
        return fail("set clock");
    emmc_write(EMMC_IRPT_EN, 0);
    emmc_write(EMMC_IRPT_MASK, 0xFFFFFFFF);
    emmc_write(EMMC_INTERRUPT, 0xFFFFFFFF);
    timer_delay_us(1000);

    command(CMD_GO_IDLE_STATE, 0, nullptr);
    // CMD5 を引数 0 で送って OCR を聞き、3.3V を指定して準備ができるまで繰り返す（SDIO 仕様 3.1）
    uint32_t r;
    if (!command(CMD_IO_SEND_OP_COND, 0, &r))
        return fail("CMD5 (no SDIO card)");
    ocr_resp = r;
    uint64_t end = timer_now_us() + TIMEOUT_INIT_US;
    for (;;) {
        if (!command(CMD_IO_SEND_OP_COND, OCR_3V3, &r))
            return fail("CMD5");
        if (r & R4_READY)
            break;
        if (timer_now_us() > end)
            return fail("CMD5 timeout");
        timer_delay_us(10000);
    }
    ocr_resp = r;
    if (!command(CMD_SEND_RCA, 0, &r))
        return fail("CMD3");
    rca = r & 0xFFFF0000;
    if (!command(CMD_SELECT_CARD, rca, &r))
        return fail("CMD7");

    // 4 ビットのバス（CCCR の Bus Interface Control）、25MHz
    uint8_t bus;
    if (!cmd52_init(false, 0, CCCR_BUS_IF, 0, &bus) ||
        !cmd52_init(true, 0, CCCR_BUS_IF, (bus & ~3) | CCCR_BUS_WIDTH_4, nullptr))
        return fail("bus width");
    emmc_write(EMMC_CONTROL0, mmio_read32(EMMC_CONTROL0) | C0_HCTL_DWIDTH);
    // チップが対応していれば High Speed（EHS）にして 50MHz（2026-11-22）。コントローラの High Speed の印
    // （CONTROL0 の HCTL_HS_EN）は立てない（Linux の sdhci-iproc の bcm2835 は SDHCI_QUIRK_NO_HISPD_BIT）。
    // 誤りがあれば、wifi.c がファームウェアを読み直して確かめたところで気づき、25MHz でやり直す
    uint32_t hz = CLOCK_NORMAL_HZ;
    uint8_t speed;
    if (want_high_speed && cmd52_init(false, 0, CCCR_SPEED, 0, &speed) && (speed & CCCR_SPEED_SHS) &&
        cmd52_init(true, 0, CCCR_SPEED, speed | CCCR_SPEED_EHS, nullptr))
        hz = CLOCK_HIGH_HZ;
    if (!set_clock(hz))
        return fail("set clock");
    ready = true;
    return true;
}

// ---- カードの割り込み ----
// コントローラ: INTERRUPT・IRPT_MASK・IRPT_EN のビット 8 が CARD（BCM2835 ARM Peripherals の EMMC の章
// 「CARD: Card made interrupt request」。SDHCI の Card Interrupt）。IRPT_MASK は初期化で全部立ててあるので、
// INTERRUPT に状態が出る。IRPT_EN を立てると GPU 割り込み 62 番（デバイスツリーの mmc@7e300000 の
// interrupts = <2 30>）が上がる。カードが上げている間は立ったまま（書いても消えない）。
bool sdio_card_irq_setup(void)
{
    if (!sdio_rw_direct(true, 0, CCCR_IENX, CCCR_IEN_MASTER | CCCR_IEN_FN(1) | CCCR_IEN_FN(2), nullptr))
        return false;
    mmio_write32(IRQ_ENABLE_2, IRQ_EMMC);   // 割り込みコントローラで EMMC（62 番）を有効に（コア 0 に届く）
    return true;
}

// コントローラがカードの割り込みを見るか・上げるか。止めるときは、状態の有効（IRPT_MASK の CARD）も切る:
// SD Host Controller 仕様の Card Interrupt Status Enable の説明のとおり、切るとコントローラは割り込みの要求を
// 下ろして検出をやめ、入れると検出をやり直す（「意図しない割り込みを防ぐため、ドライバはカードの割り込みを
// 処理する前に切り、カードの要求がすべて消えてから入れ直してよい」）。INTERRUPT の CARD は書いても消えない。
// 切らずに信号（IRPT_EN）だけ止めると、自分の転送（4 ビットのバスでは DAT1 がデータにも使われる）で立った印が
// 残り、チップが上げていないのに割り込みが上がり続けた（実機。チップの intstatus・CCCR の保留は 0 だった）。
void sdio_card_irq_signal(bool on)
{
    emmc_write(EMMC_IRPT_MASK, on ? 0xFFFFFFFFU : ~INT_CARD);
    emmc_write(EMMC_IRPT_EN, on ? INT_CARD : 0);    // ほかの割り込みは使わない（ポーリング）
}

// 割り込みコントローラでも止める（上がり続けて止まらないときの最後の手）
void sdio_card_irq_off(void)
{
    mmio_write32(IRQ_DISABLE_2, IRQ_EMMC);
    emmc_write(EMMC_IRPT_EN, 0);
    emmc_write(EMMC_IRPT_MASK, ~INT_CARD);
}

// 今、チップが割り込みを上げているか。検出を入れ直して（割り込みは上げさせずに）、今の状態を見る
bool sdio_card_irq_pending(void)
{
    emmc_write(EMMC_IRPT_EN, 0);
    emmc_write(EMMC_IRPT_MASK, ~INT_CARD);
    emmc_write(EMMC_IRPT_MASK, 0xFFFFFFFFU);
    return (mmio_read32(EMMC_INTERRUPT) & INT_CARD) != 0;
}

const char *sdio_error(void)
{
    return init_error;
}

uint32_t sdio_ocr(void)
{
    return ocr_resp;
}
