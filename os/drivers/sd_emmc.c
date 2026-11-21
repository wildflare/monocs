// SD カードドライバ（EMMC コントローラ = Arasan SDHCI、ポーリング、PIO）。予備（os/drivers/sd.c）
// 出典: BCM2835 ARM Peripherals 5 章（EMMC）
//       SD Host Controller Simplified Specification 3.00（2.2 節 レジスタ、3.2 節 クロック）
//       SD Physical Layer Simplified Specification（4.2 節 初期化、4.7 節 コマンド）
//
// SD カードは GPIO48〜53 の機能設定で、EMMC（ALT3）と SDHOST（ALT0）のどちらかにつながる。
// 2026-11-12 から SD カードは SDHOST（os/drivers/sdhost.c）で使い、EMMC は内蔵 Wi-Fi（SDIO、GPIO34〜39）用に
// 空ける。このファイルは、SDHOST の初期化に失敗したときの予備（そのときは Wi-Fi は使えない）。
// QEMU raspi3b も GPIO48〜53 の機能設定で、SD カードを EMMC と SDHOST のどちらかにつなぐ。

#include "../kernel/klog.h"
#include "sd.h"
#include "sd_backend.h"
#include "bcm2837.h"
#include "mailbox.h"
#include "mmio.h"
#include "timer.h"
#include "uart.h"

// INTERRUPT のビット
#define INT_CMD_DONE        (1U << 0)
#define INT_DATA_DONE       (1U << 1)
#define INT_WRITE_RDY       (1U << 4)
#define INT_READ_RDY        (1U << 5)
#define INT_ERROR_MASK      0xFFFF8000U // ERR(15) と各種エラー(16〜)

// STATUS のビット
#define SR_CMD_INHIBIT      (1U << 0)
#define SR_DAT_INHIBIT      (1U << 1)

// CONTROL0 のビット
#define C0_HCTL_DWIDTH      (1U << 1)   // 4 ビットバス

// CONTROL1 のビット
#define C1_CLK_INTLEN       (1U << 0)
#define C1_CLK_STABLE       (1U << 1)
#define C1_CLK_EN           (1U << 2)
#define C1_CLK_FREQ_MASK    0xFFE0U     // CLK_FREQ8(15:8)、CLK_FREQ_MS2(7:6)、CLK_GENSEL(5)
#define C1_DATA_TOUNIT_MAX  (0xEU << 16)
#define C1_SRST_HC          (1U << 24)
#define C1_SRST_CMD         (1U << 25)
#define C1_SRST_DATA        (1U << 26)

// CMDTM のビット
#define TM_BLKCNT_EN        (1U << 1)
#define TM_AUTO_CMD12       (1U << 2)
#define TM_DAT_DIR_READ     (1U << 4)
#define TM_MULTI_BLOCK      (1U << 5)
#define CMD_RSPNS_NONE      (0U << 16)
#define CMD_RSPNS_136       (1U << 16)
#define CMD_RSPNS_48        (2U << 16)
#define CMD_RSPNS_48BUSY    (3U << 16)
#define CMD_RSPNS_MASK      (3U << 16)
#define CMD_CRCCHK_EN       (1U << 19)
#define CMD_IXCHK_EN        (1U << 20)
#define CMD_ISDATA          (1U << 21)
#define CMD_INDEX(n)        ((uint32_t)(n) << 24)

// 応答の種類
#define RESP_R1             (CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define RESP_R1B            (CMD_RSPNS_48BUSY | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define RESP_R2             (CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define RESP_R3             CMD_RSPNS_48
#define RESP_R6             RESP_R1
#define RESP_R7             RESP_R1

// コマンド
#define CMD_GO_IDLE_STATE       (CMD_INDEX(0) | CMD_RSPNS_NONE)
#define CMD_ALL_SEND_CID        (CMD_INDEX(2) | RESP_R2)
#define CMD_SEND_RELATIVE_ADDR  (CMD_INDEX(3) | RESP_R6)
#define CMD_SELECT_CARD         (CMD_INDEX(7) | RESP_R1B)
#define CMD_SEND_IF_COND        (CMD_INDEX(8) | RESP_R7)
#define CMD_SET_BLOCKLEN        (CMD_INDEX(16) | RESP_R1)
#define CMD_READ_SINGLE_BLOCK   (CMD_INDEX(17) | RESP_R1 | CMD_ISDATA | TM_DAT_DIR_READ)
#define CMD_READ_MULTIPLE_BLOCK (CMD_INDEX(18) | RESP_R1 | CMD_ISDATA | TM_DAT_DIR_READ | \
                                 TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_WRITE_SINGLE_BLOCK  (CMD_INDEX(24) | RESP_R1 | CMD_ISDATA)
#define CMD_WRITE_MULTIPLE_BLOCK (CMD_INDEX(25) | RESP_R1 | CMD_ISDATA | \
                                 TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_APP_CMD             (CMD_INDEX(55) | RESP_R1)
#define ACMD_SET_BUS_WIDTH      (CMD_INDEX(6) | RESP_R1)
#define ACMD_SD_SEND_OP_COND    (CMD_INDEX(41) | RESP_R3)

// CMD8 の引数: 2.7〜3.6V、チェックパターン 0xAA
#define IF_COND_ARG             0x1AA
// ACMD41 の引数と応答
#define OCR_VOLTAGE_WINDOW      0x00FF8000U // 2.7〜3.6V
#define OCR_HCS                 (1U << 30)  // SDHC/SDXC に対応（引数）、大容量カード（応答の CCS）
#define OCR_BUSY                (1U << 31)  // 0 の間は初期化中

// SLOTISR_VER の SDVERSION（23:16）。2 = SD Host Controller 仕様 3.00
#define HOST_SPEC_V3            2

#define CLOCK_ID_HZ             400000      // 初期化中のクロック
#define CLOCK_NORMAL_HZ         25000000    // デフォルトスピードモード

#define TIMEOUT_CMD_US          1000000
#define TIMEOUT_RESET_US        100000
#define TIMEOUT_INIT_US         1000000

#define MAX_BLOCKS_PER_CMD      0xFFFF      // BLKSIZECNT の BLKCNT は 16 ビット

static uint32_t base_clock;
static uint32_t write_gap_us = 10;
static uint32_t rca;
static bool high_capacity;
static bool ready;

// BCM2835 の Arasan は、SD クロック 2 周期以内に続けて書いたレジスタ値を取りこぼすことがある。
// 出典: Linux drivers/mmc/host/sdhci-iproc.c（bcm2835 向けの書き込み処理のコメント）
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

// INTERRUPT に mask のどれか、またはエラーが立つのを待つ。エラーなら 0
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

// コマンドを送り、応答を resp に入れる（resp は NULL 可）。失敗すると false
static bool sd_command(uint32_t cmd, uint32_t arg, uint32_t resp[4])
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

    // R1b は、カードがビジーを解除すると転送完了（DATA_DONE）が立つ
    if ((cmd & CMD_RSPNS_MASK) == CMD_RSPNS_48BUSY && !wait_interrupt(INT_DATA_DONE, TIMEOUT_CMD_US))
        return false;

    if (resp) {
        resp[0] = mmio_read32(EMMC_RESP0);
        resp[1] = mmio_read32(EMMC_RESP1);
        resp[2] = mmio_read32(EMMC_RESP2);
        resp[3] = mmio_read32(EMMC_RESP3);
    }
    return true;
}

static bool sd_app_command(uint32_t cmd, uint32_t arg, uint32_t resp[4])
{
    return sd_command(CMD_APP_CMD, rca, nullptr) && sd_command(cmd, arg, resp);
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
        // 10 ビット分周: 周波数 = 基準 / (2 * N)。N = 0 は分周なし
        uint32_t div = (base_clock + 2 * hz - 1) / (2 * hz);
        if (div > 0x3FF)
            div = 0x3FF;
        bits = ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
        actual = div ? base_clock / (2 * div) : base_clock;
    } else {
        // 2 のべき乗分周: CLK_FREQ8 に 分周比 / 2 を入れる
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
    return true;
}

static void gpio_init(void)
{
    // GPIO48〜53 を ALT3（EMMC）にする。48、49 は GPFSEL4、50〜53 は GPFSEL5
    // 先にいったん入力にする（SDHOST から戻すとき。QEMU の bcm2835_gpio は、48〜53 が入力のときにカードを
    // EMMC につなぎ直すので。実機では ALT3 にするだけで EMMC につながる。入力の間はプルアップのまま）
    const uint32_t alt3 = 7;
    mmio_write32(GPFSEL4, mmio_read32(GPFSEL4) & ~((7U << 24) | (7U << 27)));
    mmio_write32(GPFSEL5, mmio_read32(GPFSEL5) & ~0xFFFU);
    uint32_t sel4 = mmio_read32(GPFSEL4);
    sel4 &= ~((7U << 24) | (7U << 27));
    sel4 |= (alt3 << 24) | (alt3 << 27);
    mmio_write32(GPFSEL4, sel4);
    uint32_t sel5 = mmio_read32(GPFSEL5);
    sel5 &= ~0xFFFU;
    sel5 |= alt3 | (alt3 << 3) | (alt3 << 6) | (alt3 << 9);
    mmio_write32(GPFSEL5, sel5);

    // CMD、DAT0〜3（GPIO49〜53）をプルアップする（6.1 節 GPPUDCLKn の手順）
    const uint32_t pull_up = 2;
    mmio_write32(GPPUD, pull_up);
    delay_cycles(150);
    mmio_write32(GPPUDCLK1, 0x1FU << (49 - 32));
    delay_cycles(150);
    mmio_write32(GPPUD, 0);
    mmio_write32(GPPUDCLK1, 0);
}

static bool fail(const char *what)
{
    uart_puts("sd (EMMC): ");
    uart_puts(what);
    uart_puts(" failed\n");
    return false;
}

bool emmc_sd_init(void)
{
    uint32_t resp[4];

    ready = false;
    base_clock = mailbox_get_clock_rate(MBOX_CLOCK_EMMC);
    if (base_clock == 0)
        return fail("get EMMC clock");

    gpio_init();

    // コントローラをリセットする
    write_gap_us = 10;
    emmc_write(EMMC_CONTROL0, 0);
    emmc_write(EMMC_CONTROL1, C1_SRST_HC);
    if (!wait_reg(EMMC_CONTROL1, C1_SRST_HC, false, TIMEOUT_RESET_US))
        return fail("reset");
    emmc_write(EMMC_CONTROL1, C1_CLK_INTLEN | C1_DATA_TOUNIT_MAX);
    if (!set_clock(CLOCK_ID_HZ))
        return fail("set clock");

    // 割り込み信号は使わず、INTERRUPT レジスタをポーリングする
    emmc_write(EMMC_IRPT_EN, 0);
    emmc_write(EMMC_IRPT_MASK, 0xFFFFFFFF);
    emmc_write(EMMC_INTERRUPT, 0xFFFFFFFF);

    rca = 0;
    if (!sd_command(CMD_GO_IDLE_STATE, 0, nullptr))
        return fail("CMD0");

    // CMD8 に応答しなければ、SD 仕様 1.x のカード（SDSC のみ）
    bool v2 = sd_command(CMD_SEND_IF_COND, IF_COND_ARG, resp);
    if (v2 && (resp[0] & 0xFFF) != IF_COND_ARG)
        return fail("CMD8 check pattern");

    uint32_t ocr = 0;
    uint64_t end = timer_now_us() + TIMEOUT_INIT_US;
    for (;;) {
        if (!sd_app_command(ACMD_SD_SEND_OP_COND, OCR_VOLTAGE_WINDOW | (v2 ? OCR_HCS : 0), resp))
            return fail("ACMD41");
        ocr = resp[0];
        if (ocr & OCR_BUSY)
            break;
        if (timer_now_us() > end)
            return fail("ACMD41 timeout");
        timer_delay_us(10000);
    }
    high_capacity = (ocr & OCR_HCS) != 0;

    if (!sd_command(CMD_ALL_SEND_CID, 0, resp))
        return fail("CMD2");
    if (!sd_command(CMD_SEND_RELATIVE_ADDR, 0, resp))
        return fail("CMD3");
    rca = resp[0] & 0xFFFF0000;

    if (!set_clock(CLOCK_NORMAL_HZ))
        return fail("set clock");
    if (!sd_command(CMD_SELECT_CARD, rca, resp))
        return fail("CMD7");
    if (!high_capacity && !sd_command(CMD_SET_BLOCKLEN, SD_SECTOR_SIZE, resp))
        return fail("CMD16");

    // 4 ビットバスにする（ACMD6 の引数 2 = 4 ビット）
    if (!sd_app_command(ACMD_SET_BUS_WIDTH, 2, resp))
        return fail("ACMD6");
    emmc_write(EMMC_CONTROL0, mmio_read32(EMMC_CONTROL0) | C0_HCTL_DWIDTH);

    ready = true;
    return true;
}

// 1 ブロック分を DATA レジスタとやりとりする。buf は 4 バイト境界でなくてもよい
static void pio_read_block(uint8_t *buf)
{
    for (int i = 0; i < SD_SECTOR_SIZE; i += 4) {
        uint32_t w = mmio_read32(EMMC_DATA);
        buf[i] = w;
        buf[i + 1] = w >> 8;
        buf[i + 2] = w >> 16;
        buf[i + 3] = w >> 24;
    }
}

static void pio_write_block(const uint8_t *buf)
{
    for (int i = 0; i < SD_SECTOR_SIZE; i += 4) {
        uint32_t w = buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
        mmio_write32(EMMC_DATA, w);
    }
}

static struct sd_stats stats;

static bool transfer(bool write, uint32_t sector, uint32_t count, uint8_t *buf)
{
    uint32_t cmd;
    if (write)
        cmd = count > 1 ? CMD_WRITE_MULTIPLE_BLOCK : CMD_WRITE_SINGLE_BLOCK;
    else
        cmd = count > 1 ? CMD_READ_MULTIPLE_BLOCK : CMD_READ_SINGLE_BLOCK;
    uint32_t addr = high_capacity ? sector : sector * SD_SECTOR_SIZE;
    uint32_t ready_bit = write ? INT_WRITE_RDY : INT_READ_RDY;

    uint64_t t0 = timer_now_us();
    if (!wait_reg(EMMC_STATUS, SR_DAT_INHIBIT, false, TIMEOUT_CMD_US))
        return false;
    emmc_write(EMMC_BLKSIZECNT, (count << 16) | SD_SECTOR_SIZE);
    if (!sd_command(cmd, addr, nullptr))
        return false;
    uint64_t t1 = timer_now_us();
    stats.cmd_us += t1 - t0;

    for (uint32_t i = 0; i < count; i++, buf += SD_SECTOR_SIZE) {
        if (!wait_interrupt(ready_bit, TIMEOUT_CMD_US))
            goto error;
        uint64_t t2 = timer_now_us();
        stats.ready_us += t2 - t1;
        if (write)
            pio_write_block(buf);
        else
            pio_read_block(buf);
        t1 = timer_now_us();
        stats.pio_us += t1 - t2;
    }
    if (!wait_interrupt(INT_DATA_DONE, TIMEOUT_CMD_US))
        goto error;
    stats.done_us += timer_now_us() - t1;
    return true;

error:
    emmc_write(EMMC_INTERRUPT, 0xFFFFFFFF);
    reset_lines(C1_SRST_CMD | C1_SRST_DATA);
    return false;
}

void emmc_sd_get_stats(struct sd_stats *st)
{
    *st = stats;
}

bool emmc_sd_read(uint32_t sector, uint32_t count, void *buf)
{
    if (!ready)
        return false;
    uint64_t t0 = timer_now_us();
    stats.reads++;
    stats.read_sectors += count;
    uint8_t *p = buf;
    while (count > 0) {
        uint32_t n = count > MAX_BLOCKS_PER_CMD ? MAX_BLOCKS_PER_CMD : count;
        if (!transfer(false, sector, n, p)) {
            stats.read_errors++;
            klog_printf("sd: read error at sector %u (%u sectors, EMMC)", (unsigned)sector, (unsigned)n);
            return false;
        }
        sector += n;
        count -= n;
        p += n * SD_SECTOR_SIZE;
    }
    stats.read_us += timer_now_us() - t0;
    return true;
}

bool emmc_sd_write(uint32_t sector, uint32_t count, const void *buf)
{
    if (!ready)
        return false;
    // transfer は書き込み時にバッファを読むだけ
    uint8_t *p = (uint8_t *)buf;
    uint64_t t0 = timer_now_us();
    stats.writes++;
    stats.write_sectors += count;
    while (count > 0) {
        uint32_t n = count > MAX_BLOCKS_PER_CMD ? MAX_BLOCKS_PER_CMD : count;
        if (!transfer(true, sector, n, p)) {
            stats.write_errors++;
            klog_printf("sd: write error at sector %u (%u sectors, EMMC)", (unsigned)sector, (unsigned)n);
            return false;
        }
        sector += n;
        count -= n;
        p += n * SD_SECTOR_SIZE;
    }
    stats.write_us += timer_now_us() - t0;
    return true;
}
