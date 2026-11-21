// SD カードドライバ（SDHOST コントローラ、ポーリング、PIO）。2026-11-12 から SD カードはこちら
// 出典: SDHOST（0x7E202000）はペリフェラル資料に説明がない。レジスタの意味と手順は Linux の
//       drivers/mmc/host/bcm2835.c（raspberrypi/linux rpi-6.12.y）による（GPL のコードは写さず、レジスタと
//       手順の知識だけを使って書いた）。SD カードの手順は SD Physical Layer Simplified Specification
//       （4.2 節 初期化、4.7 節 コマンド）。ピンの割り当ては Zero 2 W のデバイスツリー（sdhost-gpio48）。
//
// SD カードは GPIO48〜53 を ALT0 にすると SDHOST につながる（ALT3 なら EMMC。EMMC は内蔵 Wi-Fi の SDIO に使う）。
// コマンド: SDARG に引数、SDCMD に番号と応答の種類と NEW を書き、NEW が消えたら終わり（FAIL ならエラー）。
// データ: SDHBCT（ブロックの大きさ）・SDHBLC（ブロックの数）を書いてから読み書きのコマンドを出し、SDEDM の
// FIFO の語数を見ながら SDDATA を 32 ビットずつ読み書きする。複数ブロックは最後に CMD12 で止める。
// 応答のあとカードがビジーになるコマンド（R1b: CMD7、CMD12）は、SDHSTS の BUSY を待つ。
// SD のクロック: SDCDIV = コアのクロック / SD のクロック - 2。コアのクロックはファームウェアが変えることがある
// （実機で 250MHz・400MHz を見た）ので、ファームウェアに合わせてもらう（mailbox_set_sdhost_clock）。
// QEMU にはそのタグがないので、そのときは自分で計算する。データモードの 3 ビットの分周では足りないので、
// SLOW_CARD でいつも 11 ビットの分周を使う（Linux と同じ）。

#include "../kernel/klog.h"
#include "sd.h"
#include "sd_backend.h"
#include "bcm2837.h"
#include "mailbox.h"
#include "mmio.h"
#include "timer.h"
#include "uart.h"

#define SDHOST_BASE     (PERIPHERAL_BASE + 0x202000)
#define SDCMD           (SDHOST_BASE + 0x00)    // コマンド
#define SDARG           (SDHOST_BASE + 0x04)    // 引数
#define SDTOUT          (SDHOST_BASE + 0x08)    // タイムアウトの数（SD のクロックの数）
#define SDCDIV          (SDHOST_BASE + 0x0C)    // クロックの分周（値 + 2 で割る）
#define SDRSP0          (SDHOST_BASE + 0x10)    // 応答（31:0）。長い応答は SDRSP0〜3
#define SDHSTS          (SDHOST_BASE + 0x20)    // 状態（1 を書いて消す）
#define SDVDD           (SDHOST_BASE + 0x30)    // カードの電源
#define SDEDM           (SDHOST_BASE + 0x34)    // 状態機械と FIFO の語数（Emergency Debug Mode）
#define SDHCFG          (SDHOST_BASE + 0x38)    // 設定
#define SDHBCT          (SDHOST_BASE + 0x3C)    // ブロックの大きさ（バイト）
#define SDDATA          (SDHOST_BASE + 0x40)    // データ
#define SDHBLC          (SDHOST_BASE + 0x50)    // ブロックの数

#define SDCMD_NEW           0x8000
#define SDCMD_FAIL          0x4000
#define SDCMD_BUSYWAIT      0x0800
#define SDCMD_NO_RESPONSE   0x0400
#define SDCMD_LONG_RESPONSE 0x0200
#define SDCMD_WRITE         0x0080
#define SDCMD_READ          0x0040

#define SDHSTS_BUSY         0x0400
#define SDHSTS_BLOCK        0x0200      // 1 ブロックの転送が終わった（書き込みはカードに書けたところまで）
#define SDHSTS_REW_TIMEOUT  0x0080
#define SDHSTS_CMD_TIMEOUT  0x0040
#define SDHSTS_CRC16_ERROR  0x0020
#define SDHSTS_CRC7_ERROR   0x0010
#define SDHSTS_FIFO_ERROR   0x0008
#define SDHSTS_ERROR_MASK   (SDHSTS_CMD_TIMEOUT | SDHSTS_REW_TIMEOUT | SDHSTS_CRC16_ERROR | SDHSTS_CRC7_ERROR | \
                             SDHSTS_FIFO_ERROR)
#define SDHSTS_CLEAR_ALL    0x7F8

#define SDHCFG_BUSY_IRPT_EN (1U << 10)  // ビジーの終わりを SDHSTS_BUSY に出す（割り込みは使わない）
#define SDHCFG_BLOCK_IRPT_EN (1U << 8)  // ブロックの終わりを SDHSTS_BLOCK に出す（割り込みは使わない）
#define SDHCFG_SLOW_CARD    (1U << 3)
#define SDHCFG_WIDE_EXT_BUS (1U << 2)   // 4 ビットのバス
#define SDHCFG_WIDE_INT_BUS (1U << 1)

#define SDEDM_FORCE_DATA_MODE   (1U << 19)
#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_READ_THRESHOLD_SHIFT  14
#define SDEDM_THRESHOLD_MASK    0x1F
#define SDEDM_FIFO_WORDS(edm)   (((edm) >> 4) & 0x1F)
#define SDEDM_FSM(edm)          ((edm) & 0xF)
#define FSM_IDENTMODE           0x0
#define FSM_DATAMODE            0x1
#define FSM_READWAIT            0x4
#define FSM_WRITESTART1         0xA

#define FIFO_WORDS          16
#define FIFO_THRESHOLD      4       // シリコンのバグのため、FIFO の閾値を小さくする（Linux と同じ）
#define FIFO_BURST          8       // 読むのは FIFO にこれだけ（残りが少なければ残り全部）たまってから（Linux と同じ）

// コマンドの番号と応答の種類（SD 仕様 4.7）
#define R_NONE      SDCMD_NO_RESPONSE
#define R_SHORT     0                           // R1、R3、R6、R7
#define R_LONG      SDCMD_LONG_RESPONSE         // R2
#define R_BUSY      SDCMD_BUSYWAIT              // R1b

#define IF_COND_ARG         0x1AA
#define OCR_VOLTAGE_WINDOW  0x00FF8000U
#define OCR_HCS             (1U << 30)
#define OCR_BUSY            (1U << 31)

#define CLOCK_ID_HZ         400000
#define CLOCK_NORMAL_HZ     25000000    // デフォルトスピードモード
#define CLOCK_HIGH_HZ       50000000    // High Speed モード（SD 仕様 4.3.10。CMD6 で切り替えたカードだけ）
#define TIMEOUT_CMD_US      1000000
#define TIMEOUT_BUSY_US     2000000     // 書き込みの後のビジー（カードによっては長い）
#define TIMEOUT_DATA_US     500000      // 1 ブロックの FIFO の待ち
#define TIMEOUT_INIT_US     1000000
#define MAX_BLOCKS_PER_CMD  0x1FF       // SDHBLC は 9 ビット

static uint32_t rca;
static bool high_capacity;
static bool ready;
static bool high_speed;         // カードを High Speed モードに切り替えた（CMD6）
static uint32_t clock_hz;       // 今の SD のクロック（set_clock が返した実際の値）
static uint32_t hcfg;
static struct sd_stats stats;
static uint32_t stale_words;    // 転送の後に FIFO に残っていた語（読み捨てた数。sdhost_debug）
static uint32_t block_timeouts; // 書き込みのブロックの終わり（SDHSTS_BLOCK）が来なかった数

// 試すために直しを外す（sdtest の 2 つ目の引数。1: FIFO を 8 語待たない、2: 書き込みでブロックの終わりを
// 待たない、4: 残った語を捨てない）。ふだんは 0
uint32_t sdhost_disable_fixes;

// 起動のときに High Speed（50MHz）にするか（2026-11-21。実機の sdtest hs 30 回で確かめて 1 に。読み出しが倍の速さ。
// カードが対応していなければ 25MHz のまま。0 にすると前と同じ 25MHz）
#define SD_HIGH_SPEED_AT_BOOT 1

static bool enable_high_speed(void);
bool sdhost_set_high_clock(bool on);

void sdhost_debug(uint32_t *stale, uint32_t *block_to)
{
    *stale = stale_words;
    *block_to = block_timeouts;
}

static void clear_errors(void)
{
    uint32_t hsts = mmio_read32(SDHSTS);
    if (hsts & SDHSTS_ERROR_MASK)
        mmio_write32(SDHSTS, hsts);
}

// 状態機械が読み書きの待ちで止まっていたら、データモードに戻す
static void kick_fsm(void)
{
    uint32_t edm = mmio_read32(SDEDM);
    uint32_t fsm = SDEDM_FSM(edm);
    if (fsm == FSM_READWAIT || fsm == FSM_WRITESTART1)
        mmio_write32(SDEDM, edm | SDEDM_FORCE_DATA_MODE);
}

static bool wait_not_new(uint64_t timeout_us, uint32_t *sdcmd)
{
    uint64_t end = timer_now_us() + timeout_us;
    uint32_t v;
    while ((v = mmio_read32(SDCMD)) & SDCMD_NEW) {
        if (timer_now_us() > end)
            return false;
    }
    if (sdcmd)
        *sdcmd = v;
    return true;
}

// コマンドを送る。resp は NULL 可（長い応答なら 4 語、短い応答なら resp[0]）。失敗すると false
static bool command(uint32_t index, uint32_t flags, uint32_t arg, uint32_t resp[4])
{
    if (!wait_not_new(100000, nullptr))
        return false;       // 前のコマンドが終わっていない
    clear_errors();
    if (flags & R_BUSY)
        mmio_write32(SDHSTS, SDHSTS_BUSY);
    mmio_write32(SDARG, arg);
    mmio_write32(SDCMD, (index & 0x3F) | flags | SDCMD_NEW);

    uint32_t sdcmd;
    if (!wait_not_new(TIMEOUT_CMD_US, &sdcmd))
        return false;
    if (sdcmd & SDCMD_FAIL) {
        uint32_t hsts = mmio_read32(SDHSTS);
        mmio_write32(SDHSTS, SDHSTS_ERROR_MASK);
        // R3（ACMD41 の応答）には CRC がないので、CRC7 の誤りだけなら成功として扱う
        if (!(index == 41 && (hsts & SDHSTS_ERROR_MASK) == SDHSTS_CRC7_ERROR)) {
            kick_fsm();
            return false;
        }
    }
    if (flags & R_BUSY) {
        uint64_t end = timer_now_us() + TIMEOUT_BUSY_US;
        while (!(mmio_read32(SDHSTS) & SDHSTS_BUSY)) {
            if (timer_now_us() > end)
                return false;
        }
        mmio_write32(SDHSTS, SDHSTS_BUSY);
    }
    if (resp) {
        resp[0] = mmio_read32(SDRSP0);
        if (flags & R_LONG)
            for (int i = 1; i < 4; i++)
                resp[i] = mmio_read32(SDRSP0 + 4 * i);
    }
    return true;
}

static bool app_command(uint32_t index, uint32_t flags, uint32_t arg, uint32_t resp[4])
{
    return command(55, R_SHORT, rca, nullptr) && command(index, flags, arg, resp);
}

static void set_clock(uint32_t hz)
{
    uint32_t actual = mailbox_set_sdhost_clock(hz);
    if (!actual) {
        // ファームウェアに頼めない（QEMU）: コアのクロックから分周を計算する
        uint32_t core = mailbox_get_clock_rate(MBOX_CLOCK_CORE);
        if (!core)
            core = 250000000;
        uint32_t div = core / hz;
        if (div < 2)
            div = 2;
        if (core / div > hz)
            div++;
        div -= 2;
        if (div > 0x7FF)
            div = 0x7FF;
        mmio_write32(SDCDIV, div);
        actual = core / (div + 2);
    }
    mmio_write32(SDTOUT, actual / 2);   // タイムアウトは 0.5 秒
    clock_hz = actual;
}

static void reset_host(void)
{
    mmio_write32(SDVDD, 0);
    mmio_write32(SDCMD, 0);
    mmio_write32(SDARG, 0);
    mmio_write32(SDTOUT, 0xF00000);
    mmio_write32(SDCDIV, 0);
    mmio_write32(SDHSTS, SDHSTS_CLEAR_ALL);
    mmio_write32(SDHCFG, 0);
    mmio_write32(SDHBCT, 0);
    mmio_write32(SDHBLC, 0);
    uint32_t edm = mmio_read32(SDEDM);
    edm &= ~((SDEDM_THRESHOLD_MASK << SDEDM_READ_THRESHOLD_SHIFT) |
             (SDEDM_THRESHOLD_MASK << SDEDM_WRITE_THRESHOLD_SHIFT));
    edm |= (FIFO_THRESHOLD << SDEDM_READ_THRESHOLD_SHIFT) | (FIFO_THRESHOLD << SDEDM_WRITE_THRESHOLD_SHIFT);
    mmio_write32(SDEDM, edm);
    timer_delay_us(20000);
    mmio_write32(SDVDD, 1);
    timer_delay_us(20000);
    hcfg = SDHCFG_BUSY_IRPT_EN | SDHCFG_BLOCK_IRPT_EN | SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS;
    mmio_write32(SDHCFG, hcfg);
}

static void gpio_init(void)
{
    // GPIO48〜53 を ALT0（SDHOST）にする。48、49 は GPFSEL4、50〜53 は GPFSEL5
    const uint32_t alt0 = 4;
    uint32_t sel4 = mmio_read32(GPFSEL4);
    sel4 &= ~((7U << 24) | (7U << 27));
    sel4 |= (alt0 << 24) | (alt0 << 27);
    mmio_write32(GPFSEL4, sel4);
    uint32_t sel5 = mmio_read32(GPFSEL5);
    sel5 &= ~0xFFFU;
    sel5 |= alt0 | (alt0 << 3) | (alt0 << 6) | (alt0 << 9);
    mmio_write32(GPFSEL5, sel5);

    // CMD、DAT0〜3（GPIO49〜53）をプルアップする（ペリフェラル資料 6.1 節 GPPUDCLKn の手順）
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
    uart_puts("sd (SDHOST): ");
    uart_puts(what);
    uart_puts(" failed\n");
    return false;
}

bool sdhost_sd_init(void)
{
    uint32_t resp[4];

    ready = false;
    gpio_init();
    reset_host();
    set_clock(CLOCK_ID_HZ);

    rca = 0;
    if (!command(0, R_NONE, 0, nullptr))
        return fail("CMD0");
    // CMD8 に応答しなければ、SD 仕様 1.x のカード（SDSC のみ）
    bool v2 = command(8, R_SHORT, IF_COND_ARG, resp);
    if (v2 && (resp[0] & 0xFFF) != IF_COND_ARG)
        return fail("CMD8 check pattern");
    if (!v2)
        clear_errors();

    uint32_t ocr;
    uint64_t end = timer_now_us() + TIMEOUT_INIT_US;
    for (;;) {
        if (!app_command(41, R_SHORT, OCR_VOLTAGE_WINDOW | (v2 ? OCR_HCS : 0), resp))
            return fail("ACMD41");
        ocr = resp[0];
        if (ocr & OCR_BUSY)
            break;
        if (timer_now_us() > end)
            return fail("ACMD41 timeout");
        timer_delay_us(10000);
    }
    high_capacity = (ocr & OCR_HCS) != 0;

    if (!command(2, R_LONG, 0, resp))
        return fail("CMD2");
    if (!command(3, R_SHORT, 0, resp))
        return fail("CMD3");
    rca = resp[0] & 0xFFFF0000;

    set_clock(CLOCK_NORMAL_HZ);
    if (!command(7, R_BUSY, rca, resp))
        return fail("CMD7");
    if (!high_capacity && !command(16, R_SHORT, SD_SECTOR_SIZE, resp))
        return fail("CMD16");
    // 4 ビットのバスにする（ACMD6 の引数 2 = 4 ビット）
    if (!app_command(6, R_SHORT, 2, resp))
        return fail("ACMD6");
    hcfg |= SDHCFG_WIDE_EXT_BUS;
    mmio_write32(SDHCFG, hcfg);

    high_speed = false;
    ready = true;
    if (SD_HIGH_SPEED_AT_BOOT)
        sdhost_set_high_clock(true);
    return true;
}

// High Speed（50MHz）にする・25MHz に戻す。初めて 50MHz にするとき、カードを High Speed モードに切り替える（CMD6）。
// 切り替えたカードは 25MHz でも動く（High Speed モードは 0〜50MHz）。カードが対応していなければ false（25MHz のまま）
bool sdhost_set_high_clock(bool on)
{
    if (!ready)
        return false;
    if (on && !high_speed) {
        if (!enable_high_speed()) {
            klog_printf("sd: the card does not switch to high speed; staying at %u MHz", (unsigned)(clock_hz / 1000000));
            return false;
        }
        high_speed = true;
    }
    set_clock(on ? CLOCK_HIGH_HZ : CLOCK_NORMAL_HZ);
    return true;
}

uint32_t sdhost_clock_hz(void)
{
    return ready ? clock_hz : 0;
}

// 1 ブロック（512 バイト）を FIFO とやりとりする。buf は 4 バイト境界でなくてもよい
// 読み出しは、FIFO に FIFO_BURST 語（残りがそれより少なければ残り全部）たまるまで待ってから読む（Linux の
// bcm2835_transfer_block_pio と同じ。SDEDM の語数は SD のクロックで数えるので、ARM から速く読むと少し遅れる）
static bool pio_bytes(bool write, uint8_t *buf, int bytes)
{
    int words = bytes / 4;
    uint64_t end = timer_now_us() + TIMEOUT_DATA_US;
    while (words > 0) {
        uint32_t edm = mmio_read32(SDEDM);
        int avail = write ? FIFO_WORDS - (int)SDEDM_FIFO_WORDS(edm) : (int)SDEDM_FIFO_WORDS(edm);
        int burst = words < FIFO_BURST ? words : FIFO_BURST;
        if (sdhost_disable_fixes & 1)
            burst = 1;
        if (avail < burst) {
            if ((mmio_read32(SDHSTS) & SDHSTS_ERROR_MASK) || timer_now_us() > end)
                return false;
            continue;
        }
        if (avail > words)
            avail = words;
        words -= avail;
        for (; avail > 0; avail--, buf += 4) {
            if (write) {
                mmio_write32(SDDATA, buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24));
            } else {
                uint32_t w = mmio_read32(SDDATA);
                buf[0] = (uint8_t)w;
                buf[1] = (uint8_t)(w >> 8);
                buf[2] = (uint8_t)(w >> 16);
                buf[3] = (uint8_t)(w >> 24);
            }
        }
    }
    return true;
}

static bool pio_block(bool write, uint8_t *buf)
{
    return pio_bytes(write, buf, SD_SECTOR_SIZE);
}

// 書き込みで、1 ブロックがカードに送り終わるのを待つ（Linux も PIO の書き込みでは、次のブロックを
// SDHSTS_BLOCK の後に渡す）。待たないと、複数ブロックの書き込みの最後のブロックを送り終わる前に CMD12 を
// 出して、そのブロックが書かれない（2026-11-13、実機の sdtest で再現。これで FAT を壊した）。最後のブロック
// だけ待つのでもだめ（前のブロックの SDHSTS_BLOCK が残っているので、すぐ抜ける）
static bool wait_block(void)
{
    uint64_t end = timer_now_us() + TIMEOUT_BUSY_US;
    while (!(mmio_read32(SDHSTS) & SDHSTS_BLOCK)) {
        if ((mmio_read32(SDHSTS) & SDHSTS_ERROR_MASK) || timer_now_us() > end) {
            block_timeouts++;
            return false;
        }
    }
    mmio_write32(SDHSTS, SDHSTS_BLOCK);
    return true;
}

// 読み出しの後に FIFO に残った語を捨てる（複数ブロックの読み出しでは、CMD12 が届くまでにカードが次の
// ブロックを送り始める）。残したままだと、次の読み出しの先頭に入って中身が 4 バイト単位でずれる
static void drain_fifo(void)
{
    if (sdhost_disable_fixes & 4)
        return;
    for (int i = 0; i < 4 * FIFO_WORDS; i++) {
        if (!SDEDM_FIFO_WORDS(mmio_read32(SDEDM))) {
            timer_delay_us(1);   // 遅れて数えられた語がないか、もう一度見る
            if (!SDEDM_FIFO_WORDS(mmio_read32(SDEDM)))
                return;
        }
        (void)mmio_read32(SDDATA);
        stale_words++;
    }
}

// 読み書きの後、状態機械が次のコマンドを受けられるようになるのを待つ（書き込みならカードのビジーの間も）
static bool wait_idle(bool write)
{
    uint32_t alternate = write ? FSM_WRITESTART1 : FSM_READWAIT;
    uint64_t end = timer_now_us() + TIMEOUT_BUSY_US;
    for (;;) {
        uint32_t edm = mmio_read32(SDEDM);
        uint32_t fsm = SDEDM_FSM(edm);
        if (fsm == FSM_IDENTMODE || fsm == FSM_DATAMODE)
            return true;
        if (fsm == alternate) {
            mmio_write32(SDEDM, edm | SDEDM_FORCE_DATA_MODE);
            return true;
        }
        if (timer_now_us() > end)
            return false;
    }
}

// CMD6（SWITCH_FUNC。SD 仕様 4.3.10）: 64 バイトの状態を読む。mode 0 は調べるだけ、1 は切り替える。
// 引数の下の 4 ビットが機能のグループ 1（アクセスモード）の番号（1 = High Speed）、ほかのグループは F（変えない）
static bool switch_func(bool set, uint8_t status[64])
{
    uint32_t arg = (set ? 0x80000000u : 0) | 0x00FFFFF1u;
    drain_fifo();
    mmio_write32(SDHBCT, 64);
    mmio_write32(SDHBLC, 1);
    bool ok = command(6, R_SHORT | SDCMD_READ, arg, nullptr) && pio_bytes(false, status, 64) && wait_idle(false);
    drain_fifo();
    if (!ok || (mmio_read32(SDHSTS) & SDHSTS_ERROR_MASK)) {
        clear_errors();
        kick_fsm();
        wait_idle(false);
        return false;
    }
    return true;
}

// カードが High Speed に対応していれば切り替える（クロックは変えない: sdhost_set_high_clock）。状態の 512 ビットは
// 上位から並ぶ。グループ 1 の対応する機能はビット 415:400（バイト 12・13）、選んだ機能はビット 379:376（バイト 16 の
// 下の 4 ビット。0xF は切り替えられない）
static bool enable_high_speed(void)
{
    uint8_t st[64];
    if (!switch_func(false, st) || !(st[13] & 0x02) || (st[16] & 0x0F) != 1)
        return false;
    if (!switch_func(true, st) || (st[16] & 0x0F) != 1)
        return false;
    timer_delay_us(10);     // 切り替えは状態を送り終わってから 8 クロックの内（SD 仕様 4.3.10.4）
    return true;
}

static bool transfer(bool write, uint32_t sector, uint32_t count, uint8_t *buf)
{
    uint32_t index = write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17);
    uint32_t addr = high_capacity ? sector : sector * SD_SECTOR_SIZE;

    uint64_t t0 = timer_now_us();
    if (!write)
        drain_fifo();
    mmio_write32(SDHSTS, SDHSTS_BLOCK);
    mmio_write32(SDHBCT, SD_SECTOR_SIZE);
    mmio_write32(SDHBLC, count);
    if (!command(index, R_SHORT | (write ? SDCMD_WRITE : SDCMD_READ), addr, nullptr))
        goto error;
    uint64_t t1 = timer_now_us();
    stats.cmd_us += t1 - t0;

    for (uint32_t i = 0; i < count; i++, buf += SD_SECTOR_SIZE) {
        if (!pio_block(write, buf))
            goto error;
        if (write && !(sdhost_disable_fixes & 2) && !wait_block())
            goto error;
    }
    uint64_t t2 = timer_now_us();
    stats.pio_us += t2 - t1;

    if (count > 1 && !command(12, R_BUSY, 0, nullptr))   // 複数ブロック: 止める（書き込みならビジーも待つ）
        goto error;
    if (!wait_idle(write))
        goto error;
    if (!write)
        drain_fifo();
    if (mmio_read32(SDHSTS) & SDHSTS_ERROR_MASK)
        goto error;
    stats.done_us += timer_now_us() - t2;
    return true;

error:
    clear_errors();
    kick_fsm();
    if (count > 1)
        command(12, R_BUSY, 0, nullptr);
    wait_idle(write);
    drain_fifo();
    clear_errors();
    return false;
}

// カードを手放す（CMD0 で待機の状態に戻す。EMMC に切り替える前に。sdtest）
void sdhost_sd_release(void)
{
    if (ready)
        command(0, R_NONE, 0, nullptr);
    ready = false;
}

void sdhost_sd_get_stats(struct sd_stats *st)
{
    *st = stats;
}

static bool sd_rw(bool write, uint32_t sector, uint32_t count, uint8_t *p)
{
    if (!ready)
        return false;
    uint64_t t0 = timer_now_us();
    if (write) {
        stats.writes++;
        stats.write_sectors += count;
    } else {
        stats.reads++;
        stats.read_sectors += count;
    }
    while (count > 0) {
        uint32_t n = count > MAX_BLOCKS_PER_CMD ? MAX_BLOCKS_PER_CMD : count;
        if (!transfer(write, sector, n, p)) {
            if (write)
                stats.write_errors++;
            else
                stats.read_errors++;
            klog_printf("sd: %s error at sector %u (%u sectors)", write ? "write" : "read", (unsigned)sector,
                        (unsigned)n);
            return false;
        }
        sector += n;
        count -= n;
        p += n * SD_SECTOR_SIZE;
    }
    if (write)
        stats.write_us += timer_now_us() - t0;
    else
        stats.read_us += timer_now_us() - t0;
    return true;
}

bool sdhost_sd_read(uint32_t sector, uint32_t count, void *buf)
{
    return sd_rw(false, sector, count, buf);
}

bool sdhost_sd_write(uint32_t sector, uint32_t count, const void *buf)
{
    return sd_rw(true, sector, count, (uint8_t *)buf);   // 書き込みではバッファを読むだけ
}
