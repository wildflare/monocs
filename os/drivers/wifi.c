// 内蔵 Wi-Fi（CYW43436 = BCM43430 系、SDIO）のドライバ: チップの初期化とファームウェアの読み込み、
// 制御のコマンド（SDPCM・BCDC）、イベント、スキャン、接続、データの送受信。
// つなぐ手順（設定ファイル、自動の接続）は wifi_auto.c、SDIO のバスは sdio.c、lwIP とのつなぎは os/net/net.c
// 出典: Linux drivers/net/wireless/broadcom/brcm80211/brcmfmac（sdio.c・bcmsdh.c・chip.c、ISC ライセンス。
//       rpi-6.12.y）のレジスタと手順。コードは写さず、手順を見て書いた
//
// チップの中は「バックプレーン」（32 ビットの番地の空間）で、SDIO の機能 1 から 32KB の窓を通して読み書きする。
// 窓の位置は機能 1 の SBADDRLOW・MID・HIGH（番地の 15〜31 ビット）、窓の中は機能 1 の番地 0x8000 から
// （4 バイトの読み書きは SBSDIO_SB_ACCESS_2_4B_FLAG を立てる）。

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi.h"
#include "sdio.h"
#include "timer.h"
#include "../kernel/sysreg.h"
#include "../kernel/klog.h"

// 機能 1 のレジスタ（brcmfmac の sdio.h）
#define SBSDIO_FUNC1_SBADDRLOW      0x1000A
#define SBSDIO_FUNC1_CHIPCLKCSR     0x1000E
#define SBSDIO_FUNC1_SDIOPULLUP     0x1000F
#define SBSDIO_SB_OFT_ADDR_MASK     0x07FFF
#define SBSDIO_SB_ACCESS_2_4B_FLAG  0x08000
#define SBSDIO_SBWINDOW_MASK        0xFFFF8000U

// CHIPCLKCSR のビット（brcmfmac の sdio.c）
#define SBSDIO_FORCE_ALP            0x01
#define SBSDIO_ALP_AVAIL_REQ        0x08
#define SBSDIO_FORCE_HW_CLKREQ_OFF  0x20
#define SBSDIO_ALP_AVAIL            0x40
#define SBSDIO_HT_AVAIL             0x80
#define INIT_CLKCTL1                (SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_ALP_AVAIL_REQ)

#define F1_BLOCKSIZE                64
#define F2_BLOCKSIZE                512
#define CHIPCOMMON_BASE             0x18000000U     // チップの番号（chipid）はこの先頭
#define PMU_MAX_TRANSITION_US       1000000

static uint32_t window = 0xFFFFFFFFU;

static bool set_window(uint32_t addr)
{
    uint32_t w = addr & SBSDIO_SBWINDOW_MASK;
    if (w == window)
        return true;
    for (int i = 0; i < 3; i++)
        if (!sdio_rw_direct(true, 1, SBSDIO_FUNC1_SBADDRLOW + i, (uint8_t)(w >> (8 * (i + 1))), nullptr)) {
            window = 0xFFFFFFFFU;
            return false;
        }
    window = w;
    return true;
}

// バックプレーン（チップの中の 32 ビットの番地）の読み書き
static bool bp_read32(uint32_t addr, uint32_t *val)
{
    uint8_t b[4];
    if (!set_window(addr) ||
        !sdio_rw_extended(false, 1, (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG, true, b, 4))
        return false;
    *val = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool bp_write32(uint32_t addr, uint32_t val)
{
    uint8_t b[4] = {(uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
    return set_window(addr) &&
           sdio_rw_extended(true, 1, (addr & SBSDIO_SB_OFT_ADDR_MASK) | SBSDIO_SB_ACCESS_2_4B_FLAG, true, b, 4);
}

static struct wifi_info info;

// ---- チップの中のコア（brcmfmac の chip.c。AXI のバックプレーン）----
// ChipCommon の eromptr（0xFC）が指す EROM に、コアの番号・版・レジスタの番地・ラッパーの番地が並ぶ
#define CC_EROMPTR                  0xFC
#define CID_TYPE(id)                (((id) >> 28) & 0xF)    // 1 = AXI（AI）
#define DMP_DESC_TYPE_MSK           0x0F
#define DMP_DESC_VALID              0x01
#define DMP_DESC_COMPONENT          0x01
#define DMP_DESC_MASTER_PORT        0x03
#define DMP_DESC_ADDRESS            0x05
#define DMP_DESC_ADDRSIZE_GT32      0x08
#define DMP_DESC_EOT                0x0F
#define DMP_COMP_PARTNUM(v)         (((v) >> 8) & 0xFFF)
#define DMP_COMP_REVISION(v)        (((v) >> 24) & 0xFF)
#define DMP_COMP_NUM_SWRAP(v)       (((v) >> 19) & 0x1F)
#define DMP_COMP_NUM_MWRAP(v)       (((v) >> 14) & 0x1F)
#define DMP_SLAVE_ADDR_BASE         0xFFFFF000U
#define DMP_SLAVE_TYPE(v)           (((v) >> 6) & 3)
#define DMP_SLAVE_TYPE_SLAVE        0
#define DMP_SLAVE_TYPE_SWRAP        2
#define DMP_SLAVE_TYPE_MWRAP        3
#define DMP_SLAVE_SIZE_TYPE(v)      (((v) >> 4) & 3)
#define DMP_SLAVE_SIZE_4K           0
#define DMP_SLAVE_SIZE_8K           1
#define DMP_SLAVE_SIZE_DESC         3

#define CORE_INTERNAL_MEM           0x80E   // SOCRAM
#define CORE_80211                  0x812
#define CORE_PMU                    0x827
#define CORE_SDIO_DEV               0x829
#define CORE_ARM_CM3                0x82A
#define CORE_GCI                    0x840

// ラッパーのレジスタ（Linux の include/linux/bcma/bcma_regs.h）
#define BCMA_IOCTL                  0x0408
#define BCMA_IOCTL_CLK              0x0001
#define BCMA_IOCTL_FGC              0x0002
#define BCMA_RESET_CTL              0x0800
#define BCMA_RESET_CTL_RESET        0x0001
#define D11_IOCTL_PHYCLOCKEN        0x0004
#define D11_IOCTL_PHYRESET          0x0008

// SOCRAM のレジスタ（chip.c の sbsocramregs）
#define SOCRAM_COREINFO             0x00
#define SOCRAM_BANKIDX              0x10
#define SOCRAM_BANKINFO             0x40
#define SOCRAM_BANKPDA              0x44
#define SRCI_SRNB(v)                (((v) >> 4) & 0xF)
#define SRCI_SRBSZ(v)               ((v) & 0xF)
#define SRCI_LSS(v)                 (((v) >> 20) & 0xF)
#define SR_BSZ_BASE                 14
#define SOCRAM_BANKINFO_SZMASK      0x7F
#define SOCRAM_BANKINFO_SZBASE      8192

// SDIO のコアのレジスタ（sdio.c の sdpcmd_regs）
#define SD_INTSTATUS                0x20
#define SD_HOSTINTMASK              0x24
#define SD_TOSBMAILBOXDATA          0x48
#define I_HMB_SW_MASK               0x000000F0
#define I_CHIPACTIVE                (1U << 29)
#define SDPCM_PROT_VERSION          4
#define SMB_DATA_VERSION_SHIFT      16

#define SBSDIO_FORCE_HT             0x02
#define SBSDIO_HT_AVAIL_REQ         0x10
#define SBSDIO_WATERMARK            0x10008
#define DEFAULT_F2_WATERMARK        0x08
#define F2_READY_TIMEOUT_US         3000000

struct core { uint32_t id, rev, base, wrap; };
static uint32_t sdio_core;          // SDIO のコアのレジスタの番地
static uint8_t tx_seq, tx_max;      // SDPCM の送る番号と、送ってよい番号の上限（チップが知らせる）
static struct core cores[24];
static int ncores;

static struct core *find_core(uint32_t id)
{
    for (int i = 0; i < ncores; i++)
        if (cores[i].id == id)
            return &cores[i];
    return nullptr;
}

static uint32_t rd(uint32_t addr)
{
    uint32_t v = 0xFFFFFFFFU;
    bp_read32(addr, &v);
    return v;
}

static void wr(uint32_t addr, uint32_t v)
{
    bp_write32(addr, v);
}

// コアのレジスタの番地とラッパーの番地を探す（chip.c の brcmf_chip_dmp_get_regaddr）
static bool erom_regaddr(uint32_t *ep, uint32_t *base, uint32_t *wrap)
{
    *base = *wrap = 0;
    uint32_t v = rd(*ep);
    *ep += 4;
    uint32_t desc = v & DMP_DESC_TYPE_MSK;
    if ((desc & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
        desc = DMP_DESC_ADDRESS;
    uint32_t wraptype;
    if (desc == DMP_DESC_MASTER_PORT) {
        wraptype = DMP_SLAVE_TYPE_MWRAP;
    } else if (desc == DMP_DESC_ADDRESS) {
        *ep -= 4;
        wraptype = DMP_SLAVE_TYPE_SWRAP;
    } else {
        *ep -= 4;
        return false;
    }
    do {
        do {
            v = rd(*ep);
            *ep += 4;
            desc = v & DMP_DESC_TYPE_MSK;
            if ((desc & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
                desc = DMP_DESC_ADDRESS;
            if (desc == DMP_DESC_EOT) {
                *ep -= 4;
                return false;
            }
        } while (desc != DMP_DESC_ADDRESS && desc != DMP_DESC_COMPONENT);
        if (desc == DMP_DESC_COMPONENT) {
            *ep -= 4;
            return true;
        }
        if (v & DMP_DESC_ADDRSIZE_GT32)
            *ep += 4;
        uint32_t sz = DMP_SLAVE_SIZE_TYPE(v);
        if (sz == DMP_SLAVE_SIZE_DESC) {
            uint32_t szd = rd(*ep);
            *ep += 4;
            if (szd & DMP_DESC_ADDRSIZE_GT32)
                *ep += 4;
        }
        if (sz != DMP_SLAVE_SIZE_4K && sz != DMP_SLAVE_SIZE_8K)
            continue;
        uint32_t st = DMP_SLAVE_TYPE(v);
        if (*base == 0 && st == DMP_SLAVE_TYPE_SLAVE)
            *base = v & DMP_SLAVE_ADDR_BASE;
        if (*wrap == 0 && st == wraptype)
            *wrap = v & DMP_SLAVE_ADDR_BASE;
    } while (*base == 0 || *wrap == 0);
    return true;
}

// EROM をたどってコアを数え上げる（chip.c の brcmf_chip_dmp_erom_scan）
static void erom_scan(void)
{
    ncores = 0;
    uint32_t ep = rd(CHIPCOMMON_BASE + CC_EROMPTR);
    for (int guard = 0; guard < 4096 && ncores < (int)(sizeof cores / sizeof cores[0]); guard++) {
        uint32_t v = rd(ep);
        ep += 4;
        uint32_t type = v & DMP_DESC_TYPE_MSK;
        if (type == DMP_DESC_EOT)
            break;
        if (!(v & DMP_DESC_VALID) || type != DMP_DESC_COMPONENT)
            continue;
        uint32_t id = DMP_COMP_PARTNUM(v);
        uint32_t v2 = rd(ep);
        ep += 4;
        if ((v2 & DMP_DESC_TYPE_MSK) != DMP_DESC_COMPONENT)
            return;
        if (DMP_COMP_NUM_MWRAP(v2) + DMP_COMP_NUM_SWRAP(v2) == 0 && id != CORE_PMU && id != CORE_GCI)
            continue;
        uint32_t base, wrap;
        if (!erom_regaddr(&ep, &base, &wrap))
            continue;
        cores[ncores++] = (struct core){id, DMP_COMP_REVISION(v2), base, wrap};
    }
}

static bool core_is_up(struct core *c)
{
    return (rd(c->wrap + BCMA_IOCTL) & (BCMA_IOCTL_FGC | BCMA_IOCTL_CLK)) == BCMA_IOCTL_CLK &&
           !(rd(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET);
}

static void core_disable(struct core *c, uint32_t prereset, uint32_t reset)
{
    if (!(rd(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET)) {
        wr(c->wrap + BCMA_IOCTL, prereset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
        rd(c->wrap + BCMA_IOCTL);
        wr(c->wrap + BCMA_RESET_CTL, BCMA_RESET_CTL_RESET);
        timer_delay_us(20);
        uint64_t end = timer_now_us() + 300;
        while (rd(c->wrap + BCMA_RESET_CTL) != BCMA_RESET_CTL_RESET && timer_now_us() < end)
            ;
    }
    wr(c->wrap + BCMA_IOCTL, reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
    rd(c->wrap + BCMA_IOCTL);
}

static void core_reset(struct core *c, uint32_t prereset, uint32_t reset, uint32_t postreset)
{
    core_disable(c, prereset, reset);
    for (int n = 0; (rd(c->wrap + BCMA_RESET_CTL) & BCMA_RESET_CTL_RESET) && n <= 50; n++) {
        wr(c->wrap + BCMA_RESET_CTL, 0);
        timer_delay_us(50);
    }
    wr(c->wrap + BCMA_IOCTL, postreset | BCMA_IOCTL_CLK);
    rd(c->wrap + BCMA_IOCTL);
}

// ARM（Cortex-M3）を止め、無線の部分と RAM をリセットする（chip.c の brcmf_chip_cm3_set_passive）
static void set_passive(void)
{
    struct core *arm = find_core(CORE_ARM_CM3), *d11 = find_core(CORE_80211), *sr = find_core(CORE_INTERNAL_MEM);
    if (arm)
        core_disable(arm, 0, 0);
    if (d11)
        core_reset(d11, D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN);
    if (sr) {
        core_reset(sr, 0, 0, 0);
        if (info.chip == 43430) {   // バンク 3 の付け替えを外す（43430・43439 だけ）
            wr(sr->base + SOCRAM_BANKIDX, 3);
            wr(sr->base + SOCRAM_BANKPDA, 0);
        }
    }
}

// RAM の大きさ（chip.c の brcmf_chip_socram_ramsize）
static uint32_t socram_size(struct core *sr)
{
    if (!core_is_up(sr))
        core_reset(sr, 0, 0, 0);
    uint32_t ci = rd(sr->base + SOCRAM_COREINFO);
    uint32_t nb = SRCI_SRNB(ci), size = 0;
    if (sr->rev <= 7 || sr->rev == 12) {
        uint32_t lss = SRCI_LSS(ci);
        if (lss)
            nb--;
        size = nb * (1U << (SRCI_SRBSZ(ci) + SR_BSZ_BASE));
        if (lss)
            size += 1U << (lss - 1 + SR_BSZ_BASE);
    } else {
        for (uint32_t i = 0; i < nb; i++) {
            wr(sr->base + SOCRAM_BANKIDX, i);
            size += ((rd(sr->base + SOCRAM_BANKINFO) & SOCRAM_BANKINFO_SZMASK) + 1) * SOCRAM_BANKINFO_SZBASE;
        }
    }
    return size;
}

// チップの RAM を読み書きする（窓をずらしながら、F1 の CMD53 で。bcmsdh.c の brcmf_sdiod_ramrw）
static bool ram_rw(bool write, uint32_t addr, uint8_t *data, uint32_t size)
{
    while (size) {
        uint32_t off = addr & SBSDIO_SB_OFT_ADDR_MASK;
        uint32_t n = 0x8000 - off;
        if (n > size)
            n = size;
        const uint32_t max_block = F1_BLOCKSIZE * 511;
        if (n > max_block)
            n = max_block;
        if (n > F1_BLOCKSIZE)
            n -= n % F1_BLOCKSIZE;
        if (!set_window(addr) ||
            !sdio_rw_extended(write, 1, off | SBSDIO_SB_ACCESS_2_4B_FLAG, true, data, n))
            return false;
        addr += n;
        data += n;
        size -= n;
    }
    return true;
}

static bool fail(const char *what)
{
    info.error = what;
    return false;
}

bool wifi_probe(void)
{
    info = (struct wifi_info){0};
    window = 0xFFFFFFFFU;
    uint64_t t0 = timer_now_us();
    if (!sdio_init())
        return fail(sdio_error());
    info.ocr = sdio_ocr();
    if (!sdio_set_block_size(1, F1_BLOCKSIZE) || !sdio_set_block_size(2, F2_BLOCKSIZE))
        return fail("block size");
    if (!sdio_enable_function(1, true, 1000000))
        return fail("enable function 1");

    // ALP（チップの遅いクロック）を頼み、来るまで待つ（brcmfmac の brcmf_sdio_buscoreprep と同じ）
    uint8_t clk;
    if (!sdio_rw_direct(true, 1, SBSDIO_FUNC1_CHIPCLKCSR, INIT_CLKCTL1, nullptr) ||
        !sdio_rw_direct(false, 1, SBSDIO_FUNC1_CHIPCLKCSR, 0, &clk) ||
        (clk & ~(SBSDIO_ALP_AVAIL | SBSDIO_HT_AVAIL)) != INIT_CLKCTL1)
        return fail("ChipClkCSR");
    uint64_t end = timer_now_us() + PMU_MAX_TRANSITION_US;
    while (!(clk & SBSDIO_ALP_AVAIL)) {
        if (timer_now_us() > end || !sdio_rw_direct(false, 1, SBSDIO_FUNC1_CHIPCLKCSR, 0, &clk))
            return fail("ALP clock");
        timer_delay_us(100);
    }
    if (!sdio_rw_direct(true, 1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_FORCE_ALP, nullptr) ||
        !sdio_rw_direct(true, 1, SBSDIO_FUNC1_SDIOPULLUP, 0, nullptr))
        return fail("ChipClkCSR (force ALP)");

    uint32_t id;
    if (!bp_read32(CHIPCOMMON_BASE, &id))
        return fail("read chip id");
    info.chipid_reg = id;
    info.chip = id & 0xFFFF;
    info.rev = (id >> 16) & 0xF;
    info.probe_us = timer_now_us() - t0;
    info.found = true;
    return true;
}

const struct wifi_info *wifi_get_info(void)
{
    return &info;
}

// ---- ファームウェアの読み込み（sdio.c の brcmf_sdio_download_firmware と brcmf_sdio_firmware_callback）----

static uint8_t *read_file(const char *path, uint32_t *size, uint32_t extra)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? calloc(1, (size_t)n + extra + 4) : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = nullptr;
    }
    fclose(f);
    *size = buf ? (uint32_t)n : 0;
    return buf;
}

static bool is_nvram_char(char c)
{
    return c != '#' && c >= 0x20 && c < 0x7F;
}

// nvram の文字（「名前=値」の行）を、ファームウェアに渡す形にする（firmware.c の brcmf_fw_nvram_strip と同じ:
// 行ごとに NUL で区切り、# からと空の行は捨てる。boardrev がなければ足す。4 バイトにそろえ、最後に長さの印）
static uint32_t nvram_strip(const char *in, uint32_t len, uint8_t *out)
{
    uint32_t o = 0, i = 0;
    bool boardrev = false;
    while (i < len) {
        char c = in[i];
        if (c == '#' || c == '\n') {
            while (i < len && in[i] != '\n')
                i++;
            i++;
            continue;
        }
        if (!is_nvram_char(c) || c == ' ') {
            i++;
            continue;
        }
        uint32_t start = i;
        while (i < len && is_nvram_char(in[i]) && in[i] != '=' && in[i] != ' ')
            i++;
        if (i >= len || in[i] != '=') {     // 「=」がない: その行を捨てる
            while (i < len && in[i] != '\n')
                i++;
            continue;
        }
        if (i - start == 4 && !memcmp(in + start, "RAW1", 4)) {
            while (i < len && in[i] != '\n')
                i++;
            continue;
        }
        if (i - start == 8 && !memcmp(in + start, "boardrev", 8))
            boardrev = true;
        while (i < len && is_nvram_char(in[i]))
            i++;
        memcpy(out + o, in + start, i - start);
        o += i - start;
        out[o++] = 0;
    }
    if (!boardrev) {
        memcpy(out + o, "boardrev=0xff", 14);
        o += 14;
    }
    uint32_t padded = (o + 1 + 3) & ~3U;
    while (o < padded)
        out[o++] = 0;
    uint32_t token = padded / 4;
    token = (~token << 16) | (token & 0xFFFF);
    memcpy(out + o, &token, 4);
    return o + 4;
}

static bool ram_write_verify(uint32_t addr, uint8_t *data, uint32_t size)
{
    if (!ram_rw(true, addr, data, size))
        return false;
    uint8_t *check = malloc(4096);
    if (!check)
        return false;
    bool ok = true;
    for (uint32_t off = 0; ok && off < size; off += 4096) {
        uint32_t n = size - off < 4096 ? size - off : 4096;
        ok = ram_rw(false, addr + off, check, n) && !memcmp(check, data + off, n);
    }
    free(check);
    return ok;
}

bool wifi_start(const char *fw_path, const char *nvram_path)
{
    if (!info.found && !wifi_probe())
        return false;
    uint64_t t0 = timer_now_us();
    if (CID_TYPE(info.chipid_reg) != 1)
        return fail("not an AXI chip");
    erom_scan();
    info.cores = ncores;
    struct core *arm = find_core(CORE_ARM_CM3), *sr = find_core(CORE_INTERNAL_MEM), *sd = find_core(CORE_SDIO_DEV);
    if (!arm || !sr || !sd)
        return fail("cores (ARM CM3, SOCRAM, SDIO) not found");
    set_passive();
    info.ramsize = socram_size(sr);
    if (!info.ramsize || info.ramsize > 4 * 1024 * 1024)
        return fail("RAM size");

    uint32_t fwsize, nvsize;
    uint8_t *fw = read_file(fw_path, &fwsize, 0);
    if (!fw)
        return fail("cannot read the firmware");
    uint8_t *nvtext = read_file(nvram_path, &nvsize, 0);
    if (!nvtext) {
        free(fw);
        return fail("cannot read the nvram");
    }
    uint8_t *nv = calloc(1, nvsize + 64);
    uint32_t nvlen = nv ? nvram_strip((const char *)nvtext, nvsize, nv) : 0;
    free(nvtext);
    info.fw_size = fwsize;
    info.nvram_size = nvlen;

    bool ok = nv && fwsize + nvlen < info.ramsize;
    if (!ok)
        info.error = "firmware too large";
    if (ok && !(ok = ram_write_verify(0, fw, (fwsize + 3) & ~3U)))
        info.error = "write firmware";
    if (ok && !(ok = ram_write_verify(info.ramsize - nvlen, nv, nvlen)))
        info.error = "write nvram";
    free(fw);
    free(nv);
    if (!ok)
        return false;
    info.download_us = timer_now_us() - t0;

    // ARM を動かす（chip.c の brcmf_chip_cm3_set_active。SDIO のコアの割り込みの印を消してから）
    if (!core_is_up(sr))
        return fail("SOCRAM is down");
    wr(sd->base + SD_INTSTATUS, 0xFFFFFFFF);
    core_reset(arm, 0, 0, 0);

    // HT（速いクロック）を頼んで待ち、機能 2 を使えるようにする
    uint8_t clk = 0;
    if (!sdio_rw_direct(true, 1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_HT_AVAIL_REQ, nullptr))
        return fail("HT request");
    uint64_t end = timer_now_us() + PMU_MAX_TRANSITION_US;
    do {
        if (!sdio_rw_direct(false, 1, SBSDIO_FUNC1_CHIPCLKCSR, 0, &clk))
            return fail("ChipClkCSR read");
        if ((clk & (SBSDIO_ALP_AVAIL | SBSDIO_HT_AVAIL)) == (SBSDIO_ALP_AVAIL | SBSDIO_HT_AVAIL))
            break;
        timer_delay_us(100);
    } while (timer_now_us() < end);
    if (!(clk & SBSDIO_HT_AVAIL))
        return fail("HT clock (firmware did not start?)");
    sdio_rw_direct(true, 1, SBSDIO_FUNC1_CHIPCLKCSR, clk | SBSDIO_FORCE_HT, nullptr);
    wr(sd->base + SD_TOSBMAILBOXDATA, SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT);
    if (!sdio_enable_function(2, true, F2_READY_TIMEOUT_US))
        return fail("function 2 not ready");
    wr(sd->base + SD_HOSTINTMASK, I_HMB_SW_MASK | I_CHIPACTIVE);
    sdio_core = sd->base;
    tx_seq = 0xFF;      // brcmfmac と同じ（最初の 1 つは送れる）
    tx_max = 0;
    sdio_rw_direct(true, 1, SBSDIO_WATERMARK, DEFAULT_F2_WATERMARK, nullptr);

    // ファームウェアは RAM の最後の語（nvram の長さの印）を、自分の情報（sdpcm_shared）の番地に書き換える
    info.shared_addr = rd(info.ramsize - 4);
    info.start_us = timer_now_us() - t0;
    info.running = true;
    return true;
}

// ファームウェアのログ（sdpcm_shared の console_addr が指す rte_console の中の、輪になったバッファ）を
// 古いものから out に写す（sdio.c の brcmf_sdio_readconsole と同じ構造）。写した長さ。読めなければ 0
static void bus_lock(void);
static void bus_unlock(void);
static uint32_t read_console(char *out, uint32_t size);

uint32_t wifi_read_console(char *out, uint32_t size)
{
    if (!info.running || size == 0)
        return 0;
    bus_lock();     // 割り込み・タイマーからの読み出しと重ならないように
    uint32_t n = read_console(out, size);
    bus_unlock();
    return n;
}

static uint32_t read_console(char *out, uint32_t size)
{
    uint32_t sh = info.shared_addr;
    if (sh >= info.ramsize)
        return 0;
    uint32_t flags = rd(sh), console = rd(sh + 20);
    info.shared_flags = flags;
    if (console >= info.ramsize)
        return 0;
    uint32_t buf = rd(console + 8), bsize = rd(console + 12), idx = rd(console + 16);
    if (buf >= info.ramsize || bsize == 0 || bsize > 65536 || idx >= bsize)
        return 0;
    uint8_t *tmp = malloc((bsize + 3) & ~3U);
    if (!tmp || !ram_rw(false, buf, tmp, (bsize + 3) & ~3U)) {
        free(tmp);
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t k = 0; k < bsize && n + 1 < size; k++) {
        char c = (char)tmp[(idx + k) % bsize];
        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F))
            out[n++] = c;
    }
    out[n] = 0;
    free(tmp);
    return n;
}

// ---- SDPCM（機能 2 の上の枠）と BCDC（制御のコマンド）。sdio.c・bcdc.c と同じ形 ----
// 枠: ハードの頭（長さ、長さの反転。各 16 ビット）、ソフトの頭（番号 8・チャンネル 4・次の長さ 8・データの位置 8、
// 流れの制御 8・送ってよい番号の上限 8）、データ。チャンネル 0 = 制御、1 = イベント、2 = データ
#define SDPCM_HDRLEN            12
#define SDPCM_CONTROL_CHANNEL   0
#define SDPCM_EVENT_CHANNEL     1
#define SDPCM_DATA_CHANNEL      2
#define SDPCM_GLOM_CHANNEL      3       // まとめた枠（superframe）の案内
#define FIRSTREAD               64
#define MAX_FRAME               2048
#define SD_TOSBMAILBOX          0x40
#define SD_TOHOSTMAILBOXDATA    0x4C
#define I_HMB_FRAME_IND         (1U << 6)
#define I_HMB_HOST_INT          (1U << 7)
#define SMB_INT_ACK             (1U << 1)
#define F2_ADDR                 (SBSDIO_SB_ACCESS_2_4B_FLAG)   // 機能 2 の FIFO（brcmfmac は ChipCommon の窓で）

#define BCDC_DCMD_ERROR         0x01
#define BCDC_DCMD_SET           0x02
#define BCDC_DCMD_ID_SHIFT      16
#define BCDC_DCMD_LEN           16
#define WLC_GET_VAR             262
#define WLC_SET_VAR             263

// ---- バスのロック: SDIO（EMMC）を使うのは 1 か所ずつ。シェルなどは待って取る（同じコアなら重ねて取れる）。
// タイマー割り込み（wifi_net_poll）と lwIP の送信（wifi_send_eth）は、空いているときだけ使う（待たない）
// 受信を割り込みで読むとき（info.irq_mode）: バスを持っている間は、コントローラに割り込みを上げさせない
// （自分の読み書きの最中に上がっても読めない）。外すときに、チップが上げていれば読んでから、また上げさせる。
// 割り込み（wifi_handle_irq、コア 0）は、バスが空いていれば読み、空いていなければ上げさせないようにして戻る
// （持っているところが外すときに読む）。行き違いで上げさせないまま空いても、10ms ごとのタイマーが拾う。
static atomic_int bus_owner = -1;
static int bus_depth;
static void poll_rx(void);
static void flush_tx(void);

static void bus_taken(void)
{
    bus_depth = 1;
    if (info.irq_mode)
        sdio_card_irq_signal(false);
}

static void bus_lock(void)
{
    int me = cpu_id();
    if (atomic_load_explicit(&bus_owner, memory_order_relaxed) == me) {
        bus_depth++;
        return;
    }
    int expected = -1;
    while (!atomic_compare_exchange_weak_explicit(&bus_owner, &expected, me, memory_order_acquire,
                                                  memory_order_relaxed)) {
        expected = -1;
        __asm__ volatile("yield");
    }
    bus_taken();
}

static bool bus_trylock(void)
{
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(&bus_owner, &expected, cpu_id(), memory_order_acquire,
                                                 memory_order_relaxed))
        return false;   // ほかのところ（同じコアの割り込まれた処理も）が使っている
    bus_taken();
    return true;
}

// read_pending: 持っている間に届いたものを、外す前に読む。lwIP の送信の中（wifi_send_eth）では読まない
// （届いたものを lwIP に渡すと、送信の途中の lwIP に入り直してしまう。割り込みを上げさせれば、lwIP のロックを
// 外したところで割り込みが読む）
static void bus_release(bool read_pending)
{
    if (bus_depth > 1) {
        bus_depth--;
        return;
    }
    if (info.irq_mode && info.running) {
        // 読むとチップは割り込みを下げる。続けて届く間は 8 回まで
        for (int i = 0; read_pending && i < 8 && sdio_card_irq_pending(); i++) {
            poll_rx();
            flush_tx();
        }
        sdio_card_irq_signal(true);
    }
    bus_depth = 0;
    atomic_store_explicit(&bus_owner, -1, memory_order_release);
}

static void bus_unlock(void)
{
    bus_release(true);
}

static uint8_t rxbuf[MAX_FRAME + 512];
static uint8_t txbuf[MAX_FRAME + 512];
static uint16_t dcmd_id;
static struct {
    bool waiting, done;
    uint16_t id;
    uint32_t status, flags;
    uint8_t *buf;
    uint32_t len;
} pending;

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// 機能 2 を n バイト（4 の倍数）読み書きする。512 を超える分はブロックモードと残りに分ける
static bool f2_rw(bool write, uint8_t *p, uint32_t n)
{
    if (!set_window(CHIPCOMMON_BASE))
        return false;
    uint32_t blocks = n > F2_BLOCKSIZE ? n / F2_BLOCKSIZE * F2_BLOCKSIZE : 0;
    if (blocks && !sdio_rw_extended(write, 2, F2_ADDR, write, p, blocks))
        return false;
    uint32_t rest = n - blocks;
    return rest == 0 || sdio_rw_extended(write, 2, F2_ADDR, write, p + blocks, rest);
}

static void handle_event(const uint8_t *d, uint32_t len);
#define BCDC_HEADER_LEN         4

// 調べる用（2026-11-20。ssh の画面が長く止まったとき、相手からの返事が Monocs に届いていなかった）:
// こちらは送っているのに、データを 1 秒以上受け取らなかったとき・送ってよい番号を 0.2 秒以上待ったときを覚え、
// wifid（コア 3）が wifi_diag_report で dmesg に書く（ここはコア 0 の割り込みの中のことがあるので書かない）
#define DIAG_RX_GAP_US  1000000
#define DIAG_CREDIT_US  200000
static uint64_t diag_last_rx_us, diag_tx_at_rx, diag_nocredit_since, diag_frames_at_rx;
static uint32_t diag_ev[WIFI_EVENT_TYPES / 32];         // 前に受け取ってから来たイベントの種類
// 前に受け取ってから: チップを読みに行った回数、そのうちチップが「枠がある」と言った回数、読み出しの誤り、
// 10ms ごとのタイマーで呼ばれた回数、バスが使われていて読めなかった回数（読みに行けていたのに来なかったのか、
// 読みに行けていなかったのかを分ける。2026-11-20）
static uint32_t diag_polls, diag_frame_ind, diag_read_err, diag_timer, diag_busy;
static struct {
    uint64_t gap_us, tx, other;
    uint32_t ev[WIFI_EVENT_TYPES / 32], polls, frame_ind, read_err, timer, busy;
} diag_rx;
static struct { uint64_t wait_us, queued; } diag_cr;
static atomic_uint diag_rx_seq, diag_cr_seq;
static unsigned diag_rx_done, diag_cr_done;

static void diag_rx_data(void)
{
    uint64_t now = timer_now_us();
    uint64_t sent = info.tx_data - diag_tx_at_rx;
    if (diag_last_rx_us && info.connected && now - diag_last_rx_us > DIAG_RX_GAP_US && sent >= 2) {
        diag_rx.gap_us = now - diag_last_rx_us;
        diag_rx.tx = sent;
        diag_rx.other = info.rx_frames - diag_frames_at_rx;     // データでない枠（イベントなど。この枠を含まない）
        diag_rx.polls = diag_polls;
        diag_rx.frame_ind = diag_frame_ind;
        diag_rx.read_err = diag_read_err;
        diag_rx.timer = diag_timer;
        diag_rx.busy = diag_busy;
        memcpy(diag_rx.ev, diag_ev, sizeof diag_ev);
        info.rx_gaps++;
        atomic_fetch_add(&diag_rx_seq, 1);
    }
    diag_last_rx_us = now;
    diag_tx_at_rx = info.tx_data;
    diag_frames_at_rx = info.rx_frames;
    diag_polls = diag_frame_ind = diag_read_err = diag_timer = diag_busy = 0;
    memset(diag_ev, 0, sizeof diag_ev);
}

void wifi_diag_report(void)
{
    unsigned s = atomic_load(&diag_rx_seq);
    if (s != diag_rx_done) {
        diag_rx_done = s;
        char ev[160];
        int k = 0;
        ev[0] = 0;
        for (int t = 0; t < WIFI_EVENT_TYPES && k < (int)sizeof ev - 8; t++)
            if (diag_rx.ev[t / 32] & (1u << (t % 32)))
                k += snprintf(ev + k, sizeof ev - k, " %d", t);
        klog_printf("wifi: no data received for %llu ms while sending %llu frames (events:%s; other frames %llu; "
                    "polls %u, frame-ind %u, read errors %u, timer %u, bus busy %u)",
                    (unsigned long long)(diag_rx.gap_us / 1000), (unsigned long long)diag_rx.tx, k ? ev : " none",
                    (unsigned long long)diag_rx.other, diag_rx.polls, diag_rx.frame_ind, diag_rx.read_err,
                    diag_rx.timer, diag_rx.busy);
    }
    s = atomic_load(&diag_cr_seq);
    if (s != diag_cr_done) {
        diag_cr_done = s;
        klog_printf("wifi: waited %llu ms for the chip to accept frames (%llu queued)",
                    (unsigned long long)(diag_cr.wait_us / 1000), (unsigned long long)diag_cr.queued);
    }
}
static void (*rx_hook)(const uint8_t *frame, uint32_t len);

static void handle_frame(uint8_t *f, uint32_t len, uint32_t chan, uint32_t doff)
{
    if (chan == SDPCM_CONTROL_CHANNEL) {
        if (len < doff + BCDC_DCMD_LEN)
            return;
        uint8_t *d = f + doff;
        uint32_t flags = le32(d + 8);
        if (!pending.waiting || (flags >> BCDC_DCMD_ID_SHIFT) != pending.id)
            return;
        pending.status = le32(d + 12);
        pending.flags = flags;
        uint32_t n = len - doff - BCDC_DCMD_LEN;
        if (n > pending.len)
            n = pending.len;
        if (pending.buf)
            memcpy(pending.buf, d + BCDC_DCMD_LEN, n);
        pending.done = true;
        return;
    }
    if (chan == SDPCM_GLOM_CHANNEL) {
        info.rx_glom++;     // まとめた枠（扱わない。bus:txglom を 0 にして、送らせない）
        return;
    }
    if (chan == SDPCM_EVENT_CHANNEL) {
        info.rx_events++;
        handle_event(f + doff, len - doff);
        return;
    }
    else if (chan == SDPCM_DATA_CHANNEL) {
        info.rx_data++;
        diag_rx_data();
        // BCDC の頭（4 バイト）と data_offset（4 バイト単位）の後が Ethernet のフレーム
        if (len < doff + BCDC_HEADER_LEN)
            return;
        uint8_t *d = f + doff;
        uint32_t off = BCDC_HEADER_LEN + d[3] * 4;
        if (len - doff > off + 14 && rx_hook)
            rx_hook(d + off, len - doff - off);
    }
}

// チップから届いている枠をすべて読む（SDIO のコアの intstatus を見て）
static void poll_rx(void)
{
    diag_polls++;
    uint32_t st = rd(sdio_core + SD_INTSTATUS);
    if (st == 0xFFFFFFFFU) {
        diag_read_err++;
        return;
    }
    if (st & I_HMB_FRAME_IND)
        diag_frame_ind++;
    // 知らせてもらうことにしているビット（hostintmask）は、読んだら全部消す（brcmfmac の dpc と同じ。残すと、
    // チップが割り込みを上げ続ける）
    if (st & (I_HMB_SW_MASK | I_CHIPACTIVE))
        wr(sdio_core + SD_INTSTATUS, st & (I_HMB_SW_MASK | I_CHIPACTIVE));
    if (st & I_HMB_HOST_INT) {
        info.mailbox = rd(sdio_core + SD_TOHOSTMAILBOXDATA);
        wr(sdio_core + SD_TOSBMAILBOX, SMB_INT_ACK);
    }
    if (!(st & I_HMB_FRAME_IND))
        return;
    for (int k = 0; k < 32; k++) {
        if (!f2_rw(false, rxbuf, FIRSTREAD)) {
            diag_read_err++;
            return;
        }
        uint32_t len = rxbuf[0] | (rxbuf[1] << 8), chk = rxbuf[2] | (rxbuf[3] << 8);
        if ((len | chk) == 0)
            return;                         // もうない
        if ((uint16_t)~(len ^ chk) || len < SDPCM_HDRLEN || len > MAX_FRAME) {
            info.rx_errors++;
            return;
        }
        if (len > FIRSTREAD && !f2_rw(false, rxbuf + FIRSTREAD, (len - FIRSTREAD + 3) & ~3U)) {
            diag_read_err++;
            return;
        }
        uint32_t sw = le32(rxbuf + 4), sw2 = le32(rxbuf + 8);
        uint32_t chan = (sw >> 8) & 0xF, doff = sw >> 24;
        uint8_t max = (uint8_t)(sw2 >> 8);
        if ((uint8_t)(max - tx_seq) <= 0x40)
            tx_max = max;
        if (doff < SDPCM_HDRLEN || doff > len)
            continue;
        info.rx_frames++;
        handle_frame(rxbuf, len, chan, doff);
    }
}

static bool have_credit(void)
{
    uint8_t d = (uint8_t)(tx_max - tx_seq);
    return d != 0 && !(d & 0x80);
}

// 枠を送る。wait なら送ってよい番号が来るまで 1 秒まで待つ（来なければ false）
static bool send_frame(uint32_t chan, const uint8_t *data, uint32_t len, bool wait)
{
    uint32_t total = SDPCM_HDRLEN + len;
    if (total > MAX_FRAME)
        return false;
    uint64_t end = timer_now_us() + 1000000;
    while (!have_credit()) {
        if (!wait || timer_now_us() > end)
            return false;   // 送ってよい番号がない
        poll_rx();
    }
    uint32_t padded = (total + 3) & ~3U;
    if (padded > F2_BLOCKSIZE)
        padded = (padded + F2_BLOCKSIZE - 1) / F2_BLOCKSIZE * F2_BLOCKSIZE;
    memset(txbuf, 0, padded);
    txbuf[0] = (uint8_t)total;
    txbuf[1] = (uint8_t)(total >> 8);
    txbuf[2] = (uint8_t)~total;
    txbuf[3] = (uint8_t)(~total >> 8);
    put32(txbuf + 4, tx_seq | (chan << 8) | ((uint32_t)SDPCM_HDRLEN << 24));
    memcpy(txbuf + SDPCM_HDRLEN, data, len);
    if (!f2_rw(true, txbuf, padded))
        return false;
    tx_seq++;
    return true;
}

// 制御のコマンド（BCDC の dcmd）。buf は送るもの・受け取るもの（len バイト）。チップの答え（0 で成功、負は誤り）
static int ioctl_locked(uint32_t cmd, bool set, void *buf, uint32_t len);

int wifi_ioctl(uint32_t cmd, bool set, void *buf, uint32_t len)
{
    if (!info.running)
        return -1;
    bus_lock();
    int r = ioctl_locked(cmd, set, buf, len);
    bus_unlock();
    return r;
}

static int ioctl_locked(uint32_t cmd, bool set, void *buf, uint32_t len)
{
    static uint8_t msg[BCDC_DCMD_LEN + 1600];
    if (len > sizeof msg - BCDC_DCMD_LEN)
        return -1;
    uint16_t id = ++dcmd_id;
    put32(msg, cmd);
    put32(msg + 4, len);
    put32(msg + 8, ((uint32_t)id << BCDC_DCMD_ID_SHIFT) | (set ? BCDC_DCMD_SET : 0));
    put32(msg + 12, 0);
    memcpy(msg + BCDC_DCMD_LEN, buf, len);
    pending.waiting = true;
    pending.done = false;
    pending.id = id;
    pending.buf = set ? nullptr : buf;
    pending.len = len;
    if (!send_frame(SDPCM_CONTROL_CHANNEL, msg, BCDC_DCMD_LEN + len, true)) {
        pending.waiting = false;
        return -2;
    }
    uint64_t end = timer_now_us() + 1000000;
    while (!pending.done && timer_now_us() < end)
        poll_rx();
    pending.waiting = false;
    if (!pending.done)
        return -3;          // 答えが来ない
    return (pending.flags & BCDC_DCMD_ERROR) ? (int)pending.status : 0;
}

// 名前で読み書きする設定（iovar。「名前\0値」を WLC_GET_VAR・WLC_SET_VAR で）
int wifi_iovar_get(const char *name, void *out, uint32_t len)
{
    static uint8_t buf[1600];
    uint32_t n = (uint32_t)strlen(name) + 1;
    uint32_t total = n > len ? n : len;
    if (total > sizeof buf)
        return -1;
    bus_lock();     // buf を共有するので、組み立てから答えを写すまでロックの中で（ほかのコアからも呼ばれる）
    memset(buf, 0, total);
    memcpy(buf, name, n);
    int r = wifi_ioctl(WLC_GET_VAR, false, buf, total);
    if (r == 0)
        memcpy(out, buf, len);
    bus_unlock();
    return r;
}

int wifi_iovar_set(const char *name, const void *val, uint32_t len)
{
    static uint8_t buf[1600];
    uint32_t n = (uint32_t)strlen(name) + 1;
    if (n + len > sizeof buf)
        return -1;
    bus_lock();     // buf を共有するので（wifi_iovar_get と同じ）
    memcpy(buf, name, n);
    memcpy(buf + n, val, len);
    int r = wifi_ioctl(WLC_SET_VAR, true, buf, n + len);
    bus_unlock();
    return r;
}

// ---- イベント（チャンネル 1。BCDC の頭、Ethernet の頭（型 0x886C）、Broadcom の頭、イベントの本体。fweh.h）----
#define ETHERTYPE_BRCM          0x886C
#define EVENT_MSG_LEN           48
#define E_SET_SSID              0
#define E_DEAUTH                5
#define E_DEAUTH_IND            6
#define E_DISASSOC              11
#define E_DISASSOC_IND          12
#define E_LINK                  16
#define WLC_SUP_KEYED           6
static struct {
    bool set_ssid_done, link, psk_done, keyed;
    uint32_t set_ssid_status;
} join;
// 接続の間に届いたイベントの記録（うまくいかないときに `wifi connect` が表示する）
static struct wifi_event_rec join_trace[WIFI_TRACE_MAX];
static int join_trace_count;
#define E_PSK_SUP               46
#define E_ESCAN_RESULT          69
#define ESCAN_STATUS_PARTIAL    8

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static struct wifi_bss bss_list[WIFI_MAX_BSS];
static int bss_count;
static bool scan_done;

static void add_bss(const uint8_t *b, uint32_t blen)
{
    if (blen < 128)
        return;
    struct wifi_bss x = {0};
    memcpy(x.bssid, b + 8, 6);
    uint32_t sl = b[18] > 32 ? 32 : b[18];
    memcpy(x.ssid, b + 19, sl);
    x.ssid[sl] = 0;
    uint16_t cap = b[16] | (b[17] << 8);
    uint16_t chanspec = b[72] | (b[73] << 8);
    x.rssi = (int16_t)(b[78] | (b[79] << 8));
    x.channel = b[81] ? b[88] : (chanspec & 0xFF);
    x.chanspec = chanspec;
    uint32_t ie_off = b[116] | (b[117] << 8), ie_len = le32(b + 120);
    x.privacy = (cap & 0x10) != 0;
    // 情報要素: RSN（48）は WPA2、ベンダー固有（221）の 00:50:F2:01 は WPA
    if (ie_off + ie_len <= blen) {
        const uint8_t *ie = b + ie_off, *end = ie + ie_len;
        while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
            if (ie[0] == 48)
                x.wpa2 = true;
            if (ie[0] == 221 && ie[1] >= 4 && ie[2] == 0x00 && ie[3] == 0x50 && ie[4] == 0xF2 && ie[5] == 1)
                x.wpa = true;
            ie += 2 + ie[1];
        }
    }
    for (int i = 0; i < bss_count; i++)
        if (!memcmp(bss_list[i].bssid, x.bssid, 6)) {
            if (x.rssi > bss_list[i].rssi)
                bss_list[i] = x;
            return;
        }
    if (bss_count < WIFI_MAX_BSS)
        bss_list[bss_count++] = x;
}

static void handle_event(const uint8_t *d, uint32_t len)
{
    if (len < BCDC_HEADER_LEN)
        return;
    uint32_t off = BCDC_HEADER_LEN + d[3] * 4;
    if (len < off + 14 + 10 + EVENT_MSG_LEN)
        return;
    const uint8_t *eth = d + off;
    if (((eth[12] << 8) | eth[13]) != ETHERTYPE_BRCM)
        return;
    const uint8_t *m = eth + 24;
    uint32_t type = be32(m + 4), status = be32(m + 8), reason = be32(m + 12), dlen = be32(m + 20);
    const uint8_t *data = m + EVENT_MSG_LEN;
    if (data + dlen > d + len)
        dlen = (uint32_t)(d + len - data);
    info.last_event = type;
    if (type < WIFI_EVENT_TYPES) {
        info.event_counts[type]++;
        diag_ev[type / 32] |= 1u << (type % 32);
    }
    uint32_t eflags = (m[2] << 8) | m[3];
    if (type != E_ESCAN_RESULT && join_trace_count < WIFI_TRACE_MAX)
        join_trace[join_trace_count++] = (struct wifi_event_rec){type, status, reason, eflags};
    switch (type) {
    case E_SET_SSID:
        join.set_ssid_done = true;
        join.set_ssid_status = status;
        break;
    case E_LINK:
        join.link = (eflags & 1) != 0;
        if (!join.link)
            join.keyed = false;
        break;
    case E_PSK_SUP:
        join.psk_done = true;
        join.keyed = status == WLC_SUP_KEYED;
        break;
    case E_DEAUTH:
    case E_DEAUTH_IND:
    case E_DISASSOC:
    case E_DISASSOC_IND:
        join.link = false;
        join.keyed = false;
        break;
    }
    if (type == E_ESCAN_RESULT) {
        if (status == ESCAN_STATUS_PARTIAL && dlen >= 12 + 128) {
            uint32_t n = data[10] | (data[11] << 8);
            const uint8_t *b = data + 12;
            for (uint32_t i = 0; i < n && b + 8 <= data + dlen; i++) {
                uint32_t bl = le32(b + 4);
                if (bl < 128 || b + bl > data + dlen)
                    break;
                add_bss(b, bl);
                b += bl;
            }
        } else if (status != ESCAN_STATUS_PARTIAL) {
            scan_done = true;
        }
    }
}

void wifi_poll(void)
{
    if (!info.running)
        return;
    bus_lock();
    poll_rx();
    bus_unlock();
}

#define WLC_UP                  2
#define MAX_CHUNK_LEN           1400
#define DL_BEGIN                0x0002
#define DL_END                  0x0004
#define DLOAD_HANDLER_VER       1
#define DLOAD_FLAG_VER_SHIFT    12
#define DL_TYPE_CLM             2

// CLM（国ごとの電波の決まり）を読み込ませる（common.c の brcmf_c_download_blob と同じ。1400 バイトずつ）
static int load_clm(const char *path)
{
    uint32_t size;
    uint8_t *clm = read_file(path, &size, 0);
    if (!clm)
        return -100;
    static uint8_t chunk[12 + MAX_CHUNK_LEN];
    uint16_t flag = DL_BEGIN;
    int r = 0;
    for (uint32_t off = 0; off < size && r == 0; off += MAX_CHUNK_LEN) {
        uint32_t n = size - off > MAX_CHUNK_LEN ? MAX_CHUNK_LEN : size - off;
        if (off + n >= size)
            flag |= DL_END;
        uint16_t f = flag | (DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT);
        chunk[0] = (uint8_t)f;
        chunk[1] = (uint8_t)(f >> 8);
        chunk[2] = DL_TYPE_CLM;
        chunk[3] = 0;
        put32(chunk + 4, n);
        put32(chunk + 8, 0);
        memcpy(chunk + 12, clm + off, n);
        r = wifi_iovar_set("clmload", chunk, 12 + n);
        flag &= ~DL_BEGIN;
    }
    free(clm);
    if (r)
        return r;
    uint32_t st = 1;
    r = wifi_iovar_get("clmload_status", &st, 4);
    return r ? r : (int)st;
}

// ファームウェアを動かした後の準備: CLM、イベントの選択、国、WLC_UP
bool wifi_setup(const char *clm_path, const char *country)
{
    if (!info.running)
        return false;
    info.clm_status = load_clm(clm_path);
    // チップからホストへの枠をまとめない（まとめたものは読めない。続けて届くイベントを落として、接続が
    // 時間切れになった）。
    uint32_t glom = 0;
    info.glom_status = wifi_iovar_set("bus:txglom", &glom, 4);
    // 受信を割り込みで読む（チップの側の hostintmask は wifi_start で設定してある）。10ms ごとのタイマーも残す
    bus_lock();
    info.irq_mode = sdio_card_irq_setup();
    bus_unlock();
    // 使うイベントを選ぶ（event_msgs は 16 バイトのビットの並び。今の値に足す）
    uint8_t mask[16] = {0};
    wifi_iovar_get("event_msgs", mask, sizeof mask);
    const int events[] = {E_SET_SSID, 1, 3, 5, 6, 7, 11, 12, E_LINK, E_PSK_SUP, 54, E_ESCAN_RESULT};
    for (unsigned i = 0; i < sizeof events / sizeof events[0]; i++)
        mask[events[i] / 8] |= (uint8_t)(1U << (events[i] % 8));
    info.event_status = wifi_iovar_set("event_msgs", mask, sizeof mask);
    if (country) {
        uint8_t c[12] = {0};
        memcpy(c, country, 2);
        put32(c + 4, 0xFFFFFFFFU);  // rev = -1（CLM に任せる）
        memcpy(c + 8, country, 2);
        info.country_status = wifi_iovar_set("country", c, sizeof c);
    }
    uint32_t zero = 0;
    info.up_status = wifi_ioctl(WLC_UP, true, &zero, 4);
    return info.up_status == 0;
}

// 周りのアクセスポイントを探す（escan。結果は E_ESCAN_RESULT で届く）。見つけた数。負は誤り
// 始めるだけ（待たない）。終わったかは wifi_scan_poll で
int wifi_scan_begin(void)
{
    if (!info.running)
        return -1;
    bss_count = 0;
    scan_done = false;
    static uint8_t p[76];
    memset(p, 0, sizeof p);
    put32(p, 1);                // version
    p[4] = 1;                   // action = start
    p[6] = 0x34;                // sync_id
    p[6 + 1] = 0x12;
    memset(p + 44, 0xFF, 6);    // bssid = すべて
    p[50] = 2;                  // bss_type = any
    p[51] = 0;                  // scan_type = 能動（既定）
    put32(p + 52, 0xFFFFFFFFU); // nprobes = 既定
    put32(p + 56, 0xFFFFFFFFU);
    put32(p + 60, 0xFFFFFFFFU);
    put32(p + 64, 0xFFFFFFFFU);
    put32(p + 68, 0);           // すべてのチャンネル
    int r = wifi_iovar_set("escan", p, sizeof p);
    return r > 0 ? -r : r;
}

// 終わっていれば見つけた数（list に一覧）、まだなら -1。結果はタイマー割り込み（wifi_net_poll）か wifi_poll が読む
int wifi_scan_poll(const struct wifi_bss **list)
{
    *list = bss_list;
    return scan_done ? bss_count : -1;
}

int wifi_scan(const struct wifi_bss **list, uint32_t timeout_ms)
{
    int r = wifi_scan_begin();
    if (r)
        return r;
    uint64_t end = timer_now_us() + (uint64_t)timeout_ms * 1000;
    while (!scan_done && timer_now_us() < end) {
        wifi_poll();
        timer_delay_us(1000);
    }
    *list = bss_list;
    if (!scan_done)
        return -10;
    return bss_count;
}

// ---- 接続（ファームウェアの中の WPA2 の認証 sup_wpa を使う。cfg80211.c の brcmf_cfg80211_connect と同じ設定）----
#define WLC_SET_INFRA           20
#define WLC_SET_AUTH            22
#define WLC_SET_SSID            26
#define WLC_DISASSOC            52
#define WLC_SET_WSEC_PMK        268
#define TKIP_ENABLED            0x0002
#define AES_ENABLED             0x0004
#define WPA_AUTH_DISABLED       0x0000
#define WPA_AUTH_PSK            0x0004
#define WPA2_AUTH_PSK           0x0080
#define WSEC_PASSPHRASE         1
#define WSEC_PMK_KEYLEN         128     // brcmf_wsec_pmk_le の key（BRCMF_WSEC_MAX_SAE_PASSWORD_LEN）

static int set_int(const char *name, uint32_t v)
{
    return wifi_iovar_set(name, &v, 4);
}

static int ioctl_int(uint32_t cmd, uint32_t v)
{
    return wifi_ioctl(cmd, true, &v, 4);
}

static bool hexkey(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return false;
        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(v << 4);
        else
            out[i / 2] |= (uint8_t)v;
    }
    return s[64] == 0;
}

// pass が空なら暗号なし。8〜63 文字ならパスフレーズ、16 進の 64 文字なら PSK そのもの（wpa_supplicant.conf と同じ）
// 始めるだけ（待たない）。結果は wifi_connect_poll で
static struct {
    bool active, secured;
    uint64_t end;
    char ssid[33];
} joining;

int wifi_connect_begin(const char *ssid, const char *pass, int security, const struct wifi_bss *hint, uint32_t timeout_ms)
{
    if (!info.running)
        return WIFI_ERR_NOT_RUNNING;
    size_t sl = strlen(ssid), pl = pass ? strlen(pass) : 0;
    if (sl == 0 || sl > 32)
        return WIFI_ERR_BAD_SSID;
    uint8_t pmk_raw[32];
    bool raw = pl == 64 && hexkey(pass, pmk_raw);
    if (pl && !raw && (pl < 8 || pl > 63))
        return WIFI_ERR_BAD_PASS;

    joining.active = false;
    info.connected = false;
    wifi_ioctl(WLC_DISASSOC, true, nullptr, 0);
    memset(&join, 0, sizeof join);
    join_trace_count = 0;
    // brcmf_cfg80211_connect と同じく、WPA2 なら WPA2 の PSK と AES だけ、WPA なら WPA の PSK と TKIP・AES
    uint32_t wsec = !pl ? 0 : security == WIFI_SEC_WPA ? TKIP_ENABLED | AES_ENABLED : AES_ENABLED;
    uint32_t wpa_auth = !pl ? WPA_AUTH_DISABLED : security == WIFI_SEC_WPA ? WPA_AUTH_PSK : WPA2_AUTH_PSK;
    int r = ioctl_int(WLC_SET_INFRA, 1);
    if (!r) r = ioctl_int(WLC_SET_AUTH, 0);                         // オープンシステム認証
    if (!r) r = set_int("wsec", wsec);
    if (!r) r = set_int("wpa_auth", wpa_auth);
    if (!r) r = set_int("sup_wpa", pl ? 1 : 0);
    if (!r && pl) {
        static uint8_t pmk[4 + WSEC_PMK_KEYLEN];
        memset(pmk, 0, sizeof pmk);
        uint16_t klen = raw ? 32 : (uint16_t)pl, flags = raw ? 0 : WSEC_PASSPHRASE;
        pmk[0] = (uint8_t)klen;
        pmk[1] = (uint8_t)(klen >> 8);
        pmk[2] = (uint8_t)flags;
        pmk[3] = (uint8_t)(flags >> 8);
        memcpy(pmk + 4, raw ? pmk_raw : (const uint8_t *)pass, klen);
        r = wifi_ioctl(WLC_SET_WSEC_PMK, true, pmk, sizeof pmk);
        memset(pmk, 0, sizeof pmk);
    }
    if (r)
        return WIFI_ERR_SETUP;
    // Linux の brcmfmac（cfg80211.c の brcmf_cfg80211_connect）と同じ struct brcmf_join_params: SSID（長さ u32 と
    // 32 バイト）に続けて、スキャンでわかっている相手の BSSID とチャンネル（assoc_params: bssid[6]・詰め物 2・
    // chanspec_num u32・chanspec_list u16）。渡すとファームウェアが自分でスキャンし直さない（2026-11-17。SSID だけの
    // ときは、つなぐのに 2.8 秒かかっていた）。hint がなければ SSID だけ（今までどおり）
    uint8_t jp[52] = {0};
    put32(jp, (uint32_t)sl);
    memcpy(jp + 4, ssid, sl);
    size_t jlen = 36;
    if (hint && hint->chanspec) {
        memcpy(jp + 36, hint->bssid, 6);
        put32(jp + 44, 1);
        jp[48] = (uint8_t)hint->chanspec;
        jp[49] = (uint8_t)(hint->chanspec >> 8);
        jlen = 50;
    }
    if (wifi_ioctl(WLC_SET_SSID, true, jp, jlen))
        return WIFI_ERR_SETUP;
    memcpy(joining.ssid, ssid, sl);
    joining.ssid[sl] = 0;
    joining.secured = pl != 0;
    joining.end = timer_now_us() + (uint64_t)timeout_ms * 1000;
    joining.active = true;
    return 0;
}

// 0 = つながった、WIFI_PENDING = まだ、負 = WIFI_ERR_*。イベントはタイマー割り込み（wifi_net_poll）か wifi_poll が読む
int wifi_connect_poll(void)
{
    if (!joining.active)
        return info.connected ? 0 : WIFI_ERR_NOT_RUNNING;
    int r = WIFI_PENDING;
    if (join.set_ssid_done && join.set_ssid_status != 0)
        r = WIFI_ERR_JOIN - (int)join.set_ssid_status;      // 1: 失敗、2: 時間切れ、3: 見つからない
    else if (join.psk_done && !join.keyed)
        r = WIFI_ERR_AUTH;                                  // パスワードが違うなど
    else if (join.link && (!joining.secured || join.keyed)) {
        strcpy(info.ssid, joining.ssid);
        info.connected = true;
        r = 0;
    } else if (timer_now_us() >= joining.end)
        r = WIFI_ERR_TIMEOUT;
    if (r != WIFI_PENDING)
        joining.active = false;
    return r;
}

int wifi_connect(const char *ssid, const char *pass, int security, const struct wifi_bss *hint, uint32_t timeout_ms)
{
    int r = wifi_connect_begin(ssid, pass, security, hint, timeout_ms);
    if (r)
        return r;
    while ((r = wifi_connect_poll()) == WIFI_PENDING) {
        wifi_poll();
        timer_delay_us(1000);
    }
    return r;
}

int wifi_join_trace(const struct wifi_event_rec **list)
{
    *list = join_trace;
    return join_trace_count;
}

void wifi_disconnect(void)
{
    joining.active = false;
    wifi_ioctl(WLC_DISASSOC, true, nullptr, 0);
    info.connected = false;
}

bool wifi_link_up(void)
{
    if (info.connected && !join.link)
        info.connected = false;     // 切れた（DEAUTH・DISASSOC・LINK のイベント）
    return info.connected;
}

int wifi_rssi(void)
{
    int32_t v = 0;
    return wifi_ioctl(127 /* WLC_GET_RSSI */, false, &v, 4) == 0 ? v : 0;
}

// ---- データ（チャンネル 2）。lwIP の送信は待てない（割り込みの中のこともある）ので、待ち行列に入れて、バスが
// 空いていればすぐ送る。届いたフレームは rx_hook（os/net/net.c）へ ----
#define TXQ_LEN 128     // 32 では、小さな TCP の切れ端が続くとあふれて捨てていた（2026-11-20）
static uint8_t txq[TXQ_LEN][BCDC_HEADER_LEN + 1514];
static uint16_t txq_len[TXQ_LEN];
static atomic_uint txq_head, txq_tail;   // head: 入れる、tail: 送る

static void flush_tx(void)
{
    while (atomic_load(&txq_tail) != atomic_load(&txq_head)) {
        unsigned t = atomic_load(&txq_tail) % TXQ_LEN;
        if (!send_frame(SDPCM_DATA_CHANNEL, txq[t], txq_len[t], false)) {
            if (!diag_nocredit_since)
                diag_nocredit_since = timer_now_us();
            return;     // 送ってよい番号がない: 次の機会に
        }
        if (diag_nocredit_since) {
            uint64_t w = timer_now_us() - diag_nocredit_since;
            diag_nocredit_since = 0;
            if (w > DIAG_CREDIT_US) {
                diag_cr.wait_us = w;
                diag_cr.queued = atomic_load(&txq_head) - atomic_load(&txq_tail);
                info.credit_waits++;
                atomic_fetch_add(&diag_cr_seq, 1);
            }
        }
        info.tx_data++;
        atomic_fetch_add(&txq_tail, 1);
    }
}

bool wifi_send_eth(const uint8_t *frame, uint32_t len)
{
    if (!info.running || len > 1514)
        return false;
    static atomic_flag qlock = ATOMIC_FLAG_INIT;
    while (atomic_flag_test_and_set_explicit(&qlock, memory_order_acquire))
        __asm__ volatile("yield");
    unsigned h = atomic_load(&txq_head);
    bool ok = h - atomic_load(&txq_tail) < TXQ_LEN;
    if (ok) {
        uint8_t *b = txq[h % TXQ_LEN];
        b[0] = 2 << 4;      // BCDC の版 2
        b[1] = 0;           // 優先度
        b[2] = 0;           // インターフェース 0
        b[3] = 0;           // data_offset
        memcpy(b + BCDC_HEADER_LEN, frame, len);
        txq_len[h % TXQ_LEN] = (uint16_t)(BCDC_HEADER_LEN + len);
        atomic_store(&txq_head, h + 1);
    } else {
        info.tx_dropped++;
    }
    atomic_flag_clear_explicit(&qlock, memory_order_release);
    if (ok && bus_trylock()) {
        flush_tx();
        bus_release(false);
    }
    return ok;
}

void wifi_set_rx_hook(void (*hook)(const uint8_t *frame, uint32_t len))
{
    rx_hook = hook;
}

#define IRQ_STORM 16
static unsigned irq_burst;      // 続けて、読むものがなかった割り込みの数

// タイマー割り込みから（10ms ごと。os/net/net.c の net_timer）。バスが空いていれば、届いたものを読み、待たせて
// いる送信を送る
void wifi_net_poll(void)
{
    if (!info.running)
        return;
    diag_timer++;
    if (!bus_trylock()) {
        diag_busy++;
        return;
    }
    poll_rx();
    flush_tx();
    bus_unlock();
}

// 受信を割り込みで読むか、10ms ごとのタイマーだけにするか（シェルの wifi irq on・off。比べる・切り分ける用）
bool wifi_set_irq_mode(bool on)
{
    if (!info.running)
        return false;
    bus_lock();
    if (on) {
        irq_burst = 0;
        info.irq_mode = sdio_card_irq_setup();
    } else {
        info.irq_mode = false;
        sdio_card_irq_off();
    }
    bus_unlock();
    return info.irq_mode == on;
}

// EMMC の割り込みから（コア 0）: チップが、読むものがあると知らせた
void wifi_handle_irq(void)
{
    info.irqs++;
    // 読むものがないのに上がり続けたら（続けて IRQ_STORM 回）、割り込みはやめて 10ms ごとのタイマーだけにする。
    // 上がり続けると、コア 0 は割り込みから戻れず、シェルが動かなくなる（最初の版で起きた）
    if (!info.running || !info.irq_mode || irq_burst > IRQ_STORM) {
        if (info.irq_mode) {
            info.irq_mode = false;
            info.irq_storms++;
        }
        sdio_card_irq_off();
        return;
    }
    if (!bus_trylock()) {
        diag_busy++;
        sdio_card_irq_signal(false);    // 使っているところが、外すときに読む
        return;
    }
    uint64_t before = info.rx_frames;
    poll_rx();
    flush_tx();
    irq_burst = info.rx_frames != before ? 0 : irq_burst + 1;
    if (irq_burst > IRQ_STORM) {    // やめる前に、チップの側の状態を残す
        uint8_t v = 0;
        info.storm_intstatus = rd(sdio_core + SD_INTSTATUS);
        info.storm_hostintmask = rd(sdio_core + SD_HOSTINTMASK);
        sdio_rw_direct(false, 0, 0x05, 0, &v);
        info.storm_cccr_pending = v;
        sdio_rw_direct(false, 0, 0x04, 0, &v);
        info.storm_cccr_ien = v;
    }
    bus_unlock();
}
