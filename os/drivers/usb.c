// USB（Synopsys DesignWare Hi-Speed USB 2.0 OTG、DWC2）を機器側（ガジェット）として使う。
// 2 つの機能を持つ複合機器: USB シリアル（CDC-ACM。コンソール。Pi 5 などからは /dev/ttyACM0）と、
// USB イーサネット（CDC-ECM。ホストからはネットワークのインターフェースに見える。os/net/net.c）。
//
// 出典: BCM2835 ARM Peripherals には USB のレジスタの説明がない。レジスタとビットは Linux の
//       drivers/usb/dwc2/hw.h（v6.12）、初期化の手順は同じく gadget.c（dwc2_hsotg_core_init_disconnected）
//       と core.c（dwc2_core_reset）による。割り込みは GPU 割り込みの 9 番（Linux の bcm283x.dtsi の
//       usb@7e980000、interrupts = <1 9>）。コントローラの電源はメールボックスの Set power state（デバイス 3）。
//       記述子とリクエストは USB 2.0 仕様 9 章、CDC 1.2 と PSTN 1.2（ACM）、ECM 1.2（イーサネット）、
//       IAD（Interface Association Descriptor ECN）。定数は Linux の include/uapi/linux/usb/cdc.h と照らした。
//
// 方式: DMA を使わないスレーブモード。受信は RX FIFO の割り込み（RXFLVL）で読み、
//       送信は TX FIFO（エンドポイントごとの専用 FIFO）に直接書く。
//       受け取った文字は UART と同じ受信バッファに入れ（uart_rx_push）、UART に出す文字を USB にも送る
//       （uart_add_tx_mirror）。ホストが端末を開いていない（DTR が立っていない）間の出力は捨てる。

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/klog.h"
#include "usb.h"
#include "bcm2837.h"
#include "mailbox.h"
#include "mmio.h"
#include "timer.h"
#include "uart.h"
#include "../kernel/sysreg.h"
#include "../net/net.h"

#define USB_BASE        0x3F980000UL    // バスアドレス 0x7E980000

// コアのグローバルレジスタ
#define GOTGCTL         (USB_BASE + 0x000)
#define GAHBCFG         (USB_BASE + 0x008)
#define GUSBCFG         (USB_BASE + 0x00C)
#define GRSTCTL         (USB_BASE + 0x010)
#define GINTSTS         (USB_BASE + 0x014)
#define GINTMSK         (USB_BASE + 0x018)
#define GRXSTSP         (USB_BASE + 0x020)
#define GRXFSIZ         (USB_BASE + 0x024)
#define GNPTXFSIZ       (USB_BASE + 0x028)
#define GSNPSID         (USB_BASE + 0x040)
#define GHWCFG3         (USB_BASE + 0x04C)
#define DPTXFSIZN(n)    (USB_BASE + 0x104 + ((n) - 1) * 4)
// 機器側のレジスタ
#define DCFG            (USB_BASE + 0x800)
#define DCTL            (USB_BASE + 0x804)
#define DSTS            (USB_BASE + 0x808)
#define DIEPMSK         (USB_BASE + 0x810)
#define DOEPMSK         (USB_BASE + 0x814)
#define DAINT           (USB_BASE + 0x818)
#define DAINTMSK        (USB_BASE + 0x81C)
#define DIEPCTL(n)      (USB_BASE + 0x900 + (n) * 0x20)
#define DIEPINT(n)      (USB_BASE + 0x908 + (n) * 0x20)
#define DIEPTSIZ(n)     (USB_BASE + 0x910 + (n) * 0x20)
#define DTXFSTS(n)      (USB_BASE + 0x918 + (n) * 0x20)     // IN の FIFO の空き（語。dwc2 の hw.h）
#define DOEPCTL(n)      (USB_BASE + 0xB00 + (n) * 0x20)
#define DOEPINT(n)      (USB_BASE + 0xB08 + (n) * 0x20)
#define DOEPTSIZ(n)     (USB_BASE + 0xB10 + (n) * 0x20)
#define PCGCTL          (USB_BASE + 0xE00)
#define EPFIFO(n)       (USB_BASE + 0x1000 + (n) * 0x1000)

#define GAHBCFG_GLBL_INTR_EN    (1U << 0)
#define GUSBCFG_FORCEDEVMODE    (1U << 30)
#define GUSBCFG_FORCEHOSTMODE   (1U << 29)
#define GUSBCFG_HNPCAP          (1U << 9)
#define GUSBCFG_SRPCAP          (1U << 8)
#define GUSBCFG_TOUTCAL_MASK    (7U << 0)
#define GRSTCTL_AHBIDLE         (1U << 31)
#define GRSTCTL_TXFNUM_ALL      (0x10U << 6)
#define GRSTCTL_TXFNUM(n)       ((uint32_t)(n) << 6)
#define GRSTCTL_TXFFLSH         (1U << 5)
#define GRSTCTL_RXFFLSH         (1U << 4)
#define GRSTCTL_CSFTRST         (1U << 0)
#define GINTSTS_OEPINT          (1U << 19)
#define GINTSTS_IEPINT          (1U << 18)
#define GINTSTS_ENUMDONE        (1U << 13)
#define GINTSTS_USBRST          (1U << 12)
#define GINTSTS_RXFLVL          (1U << 4)
#define GINTSTS_CURMODE_HOST    (1U << 0)
#define GRXSTS_EPNUM(v)         ((v) & 0xF)
#define GRXSTS_BYTECNT(v)       (((v) >> 4) & 0x7FF)
#define GRXSTS_PKTSTS(v)        (((v) >> 17) & 0xF)
#define PKTSTS_OUTRX            2
#define PKTSTS_SETUPRX          6
#define DCFG_DEVADDR_MASK       (0x7FU << 4)
#define DCFG_EPMISCNT(x)        ((x) << 18)
#define DCTL_CGOUTNAK           (1U << 10)
#define DCTL_CGNPINNAK          (1U << 8)
#define DCTL_SFTDISCON          (1U << 1)
#define DSTS_ENUMSPD(v)         (((v) >> 1) & 3)
#define DSTS_ENUMSPD_HS         0
#define DXEPCTL_EPENA           (1U << 31)
#define DXEPCTL_EPDIS           (1U << 30)
#define DXEPCTL_SETD0PID        (1U << 28)
#define DXEPCTL_SNAK            (1U << 27)
#define DXEPCTL_CNAK            (1U << 26)
#define DXEPCTL_TXFNUM(x)       ((x) << 22)
#define DXEPCTL_STALL           (1U << 21)
#define DXEPCTL_EPTYPE_BULK     (2U << 18)
#define DXEPCTL_EPTYPE_INTR     (3U << 18)
#define DXEPCTL_USBACTEP        (1U << 15)
#define DXEPINT_INEPNAKEFF      (1U << 6)   // IN: SNAK が効いた（dwc2 の hw.h の DXEPINT_INEPNAKEFF）
#define DXEPINT_SETUP           (1U << 3)
#define DXEPINT_EPDISBLD        (1U << 1)   // エンドポイントを止めた
#define DXEPINT_XFERCOMPL       (1U << 0)
#define DXEPTSIZ_PKTCNT(x)      ((x) << 19)
#define DOEPTSIZ0_SUPCNT(x)     ((x) << 29)
#define DAINT_IN(n)             (1U << (n))
#define DAINT_OUT(n)            (1U << ((n) + 16))


#define MBOX_DEVICE_USB 3

// エンドポイント: 0 = 制御。ACM: 1 IN = バルク（送信）、2 OUT = バルク（受信）、3 IN = 割り込み（通知。使わない）。
// ECM: 4 IN・4 OUT = バルク（イーサネットのフレーム）、5 IN = 割り込み（通知: つながった）
#define EP_TX           1
#define EP_RX           2
#define EP_NOTIFY       3
#define EP_ETH          4
#define EP_ETH_NOTIFY   5
#define EP0_MPS         64

// インターフェース: 0・1 = ACM（通信・データ）、2・3 = ECM（通信・データ。データは代替設定 1 で使う）
#define IF_ETH_COMM     2
#define IF_ETH_DATA     3

// FIFO の配置（32 ビット語。合計は GHWCFG3 の 4080 語まで）
#define RXFIFO_WORDS    1024
#define TXFIFO0_WORDS   64
#define TXFIFO1_WORDS   1024    // ACM の送信（バルク IN）。1 回の転送はこの大きさまで
#define TXFIFO2_WORDS   16      // ACM の通知（割り込み IN）
#define TXFIFO3_WORDS   512     // ECM の送信（フレーム 1 つ、最大 1514 バイト）
#define TXFIFO4_WORDS   16      // ECM の通知
#define ETH_FRAME_MAX   1514

// 記述子（USB 2.0 9.6 節、CDC 1.2 5.2.3 節、PSTN 1.2 5.3 節）
static const uint8_t device_desc[18] = {
    18, 1, 0x00, 0x02,      // bLength, DEVICE, bcdUSB 2.00
    0xEF, 0x02, 0x01,       // 複合機器（IAD を使う。Miscellaneous / Common Class / IAD）
    EP0_MPS,
    0x6B, 0x1D, 0x04, 0x01, // 1d6b:0104（Linux の g_multi「Multifunction Composite Gadget」と同じ。legacy/multi.c）
    0x00, 0x01,             // bcdDevice 1.00
    1, 2, 0,                // 文字列: 製造者、製品、シリアル番号なし
    1,                      // 構成の数
};

static const uint8_t qualifier_desc[10] = { 10, 6, 0x00, 0x02, 0xEF, 0x02, 0x01, EP0_MPS, 1, 0 };

// 文字列 3 は ECM の iMACAddress（ホスト側のインターフェースの MAC アドレス。12 桁の 16 進）
static const char *const strings[] = { nullptr, "Monocs", "Monocs console", USB_ETH_HOST_MAC_STR };

// 構成の記述子（最大パケット長が速さで変わるので、要求のたびに作る）
static uint8_t config_desc[192];

static uint32_t build_config(uint32_t mps)
{
    uint8_t *p = config_desc;
#define B(...) do { const uint8_t b_[] = { __VA_ARGS__ }; memcpy(p, b_, sizeof(b_)); p += sizeof(b_); } while (0)
    B(9, 2, 0, 0, 4, 1, 0, 0x80, 250);      // 構成: インターフェース 4 個、バスから給電、500mA（長さは後で）
    // ACM（インターフェース 0・1）
    B(8, 0x0B, 0, 2, 0x02, 0x02, 0x00, 0);  // IAD
    // インターフェース 0: 通信クラス（CDC 2）、ACM（2）、プロトコルなし（0。AT コマンドで探られないように）
    B(9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0);
    B(5, 0x24, 0x00, 0x10, 0x01);           // Header（CDC 1.10）
    B(5, 0x24, 0x01, 0x00, 0x01);           // Call Management（データインターフェース 1）
    B(4, 0x24, 0x02, 0x02);                 // ACM: Set/Get_Line_Coding、Set_Control_Line_State
    B(5, 0x24, 0x06, 0x00, 0x01);           // Union（制御 0、データ 1）
    B(7, 5, 0x80 | EP_NOTIFY, 0x03, 16, 0, 16);   // 割り込み IN、16 バイト
    B(9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0);  // インターフェース 1: データクラス（0x0A）
    B(7, 5, 0x80 | EP_TX, 0x02, mps & 0xFF, mps >> 8, 0);  // バルク IN
    B(7, 5, EP_RX, 0x02, mps & 0xFF, mps >> 8, 0);         // バルク OUT
    // ECM（インターフェース 2・3）
    B(8, 0x0B, IF_ETH_COMM, 2, 0x02, 0x06, 0x00, 0);       // IAD
    B(9, 4, IF_ETH_COMM, 0, 1, 0x02, 0x06, 0x00, 0);       // 通信クラス、ECM（6）
    B(5, 0x24, 0x00, 0x10, 0x01);                           // Header
    B(5, 0x24, 0x06, IF_ETH_COMM, IF_ETH_DATA);             // Union
    B(13, 0x24, 0x0F, 3, 0, 0, 0, 0,                        // Ethernet Networking（MAC は文字列 3、統計なし、
      ETH_FRAME_MAX & 0xFF, ETH_FRAME_MAX >> 8, 0, 0, 0);   //   最大 1514 バイト、マルチキャストのフィルターなし）
    B(7, 5, 0x80 | EP_ETH_NOTIFY, 0x03, 16, 0, 9);          // 割り込み IN、16 バイト、2^(9-1) マイクロフレーム
    B(9, 4, IF_ETH_DATA, 0, 0, 0x0A, 0x00, 0x00, 0);        // データ: 代替設定 0（エンドポイントなし = 止める）
    B(9, 4, IF_ETH_DATA, 1, 2, 0x0A, 0x00, 0x00, 0);        // データ: 代替設定 1（使う）
    B(7, 5, 0x80 | EP_ETH, 0x02, mps & 0xFF, mps >> 8, 0);
    B(7, 5, EP_ETH, 0x02, mps & 0xFF, mps >> 8, 0);
#undef B
    uint32_t len = (uint32_t)(p - config_desc);
    config_desc[2] = (uint8_t)len;
    config_desc[3] = (uint8_t)(len >> 8);
    return len;
}

// 状態。IRQ（コア 0）と送信（どのコアからも）で共有するので、usb_lock（割り込みを止めて取る）で守る
static atomic_flag usb_lock = ATOMIC_FLAG_INIT;
static bool usb_ready;          // usb_init が成功した
static bool configured;         // SET_CONFIGURATION を受けた
static bool dtr;                // ホストが端末を開いている（SET_CONTROL_LINE_STATE の DTR）
static uint32_t bulk_mps;       // 512（High Speed）か 64（Full Speed）
static uint8_t setup_pkt[8];
static uint8_t ep0_buf[64];
static uint32_t ep0_out_len;
static bool ep0_line_coding;    // 制御転送の OUT のデータ（SET_LINE_CODING）を待っている
static bool ep0_out_discard;    // 制御転送の OUT のデータを受け取って捨てる（使わないクラスのリクエスト）
// 制御転送の IN のデータ（64 バイトのパケットに分けて送る）
static uint8_t ep0_tx_buf[256];
static uint32_t ep0_tx_pos, ep0_tx_len;
static bool ep0_tx_zlp;         // 最後に長さ 0 のパケットを送る

// ECM（USB イーサネット）
static bool eth_active;         // データインターフェースが代替設定 1（ホストが使っている）
#define ETH_Q   32              // 送信待ちのフレーム（8 では TCP の送信であふれた）
static uint8_t eth_txq[ETH_Q][ETH_FRAME_MAX];
static uint16_t eth_txq_len[ETH_Q];
static uint32_t eth_txq_head, eth_txq_tail;
static bool eth_tx_busy, eth_tx_zlp;
static uint8_t eth_rx_frame[2048];
static uint32_t eth_rx_len;
#define ETH_RQ  4               // 受け取って、まだ net_input に渡していないフレーム
static uint8_t eth_rxq[ETH_RQ][ETH_FRAME_MAX];
static uint16_t eth_rxq_len[ETH_RQ];
static uint32_t eth_rxq_head, eth_rxq_tail;
static uint64_t eth_rx_frames, eth_tx_frames, eth_rx_dropped, eth_tx_dropped;
static uint8_t line_coding[7] = { 0x00, 0xC6, 0x2D, 0x00, 0, 0, 8 };  // 3000000bps、1 ストップ、パリティなし、8 ビット

#define TX_BUF_SIZE (64 * 1024)
static uint8_t tx_buf[TX_BUF_SIZE];
static uint32_t tx_head, tx_tail;
static bool tx_busy;
static bool tx_stalled;         // ホストが受け取らない（送信バッファがいっぱいのまま 0.5 秒）。進むまで捨てる
static uint64_t tx_dropped;
static uint32_t rx_buf_words[512 / 4];

static uint64_t usb_lock_take(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    while (atomic_flag_test_and_set_explicit(&usb_lock, memory_order_acquire))
        __asm__ volatile("yield");
    return daif;
}

static void usb_lock_give(uint64_t daif)
{
    atomic_flag_clear_explicit(&usb_lock, memory_order_release);
    write_sysreg(daif, daif);
}

static bool wait_bits(uintptr_t reg, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
    uint64_t end = timer_now_us() + timeout_us;
    while ((mmio_read32(reg) & mask) != want)
        if (timer_now_us() > end)
            return false;
    return true;
}

static void delay_us(uint32_t us)
{
    uint64_t end = timer_now_us() + us;
    while (timer_now_us() < end)
        ;
}

// FIFO にバイト列を書く（語単位。端数は 0 で埋める）
static void fifo_write(int ep, const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i + k < len; k++)
            w |= (uint32_t)p[i + k] << (8 * k);
        mmio_write32(EPFIFO(ep), w);
    }
}

// ---- エンドポイント 0（制御転送） ----

static void ep0_arm_out(void)
{
    mmio_write32(DOEPTSIZ(0), DOEPTSIZ0_SUPCNT(3) | DXEPTSIZ_PKTCNT(1) | EP0_MPS);
    mmio_write32(DOEPCTL(0), DXEPCTL_EPENA | DXEPCTL_CNAK);
}

// 次のパケット（64 バイトまで）を送る
static void ep0_send_chunk(void)
{
    uint32_t n = ep0_tx_len - ep0_tx_pos;
    if (n > EP0_MPS)
        n = EP0_MPS;
    mmio_write32(DIEPTSIZ(0), DXEPTSIZ_PKTCNT(1) | n);
    mmio_write32(DIEPCTL(0), DXEPCTL_EPENA | DXEPCTL_CNAK);
    fifo_write(0, ep0_tx_buf + ep0_tx_pos, n);
    ep0_tx_pos += n;
}

// IN のデータ。ホストが求めた長さ（wLength）より短く、64 の倍数なら、最後に長さ 0 のパケットで終わりを示す
// （USB 2.0 8.5.3.2）。len が 0 なら状態のステージの長さ 0 のパケット
static void ep0_reply(const void *data, uint32_t len, uint32_t wlength)
{
    if (len > wlength && wlength)
        len = wlength;
    if (len > sizeof(ep0_tx_buf))
        len = sizeof(ep0_tx_buf);
    if (len)
        memcpy(ep0_tx_buf, data, len);
    ep0_tx_len = len;
    ep0_tx_pos = 0;
    ep0_tx_zlp = len && len < wlength && len % EP0_MPS == 0;
    ep0_send_chunk();
}

static void ep0_send(const void *data, uint32_t len)
{
    ep0_reply(data, len, len);
}

// IN の 1 パケットを送り終えた
static void ep0_in_done(void)
{
    if (ep0_tx_pos < ep0_tx_len) {
        ep0_send_chunk();
    } else if (ep0_tx_zlp) {
        ep0_tx_zlp = false;
        ep0_send_chunk();   // 長さ 0
    }
}

static void ep0_stall(void)
{
    mmio_write32(DIEPCTL(0), mmio_read32(DIEPCTL(0)) | DXEPCTL_STALL);
    mmio_write32(DOEPCTL(0), mmio_read32(DOEPCTL(0)) | DXEPCTL_STALL);
}

static void send_string(int index, uint32_t max)
{
    uint8_t buf[64];
    uint32_t len;
    if (index == 0) {
        buf[2] = 0x09;      // 英語（米国）0x0409
        buf[3] = 0x04;
        len = 4;
    } else if (index < (int)(sizeof(strings) / sizeof(strings[0]))) {
        const char *s = strings[index];
        len = 2;
        for (; *s && len + 2 <= sizeof(buf); s++) {
            buf[len++] = (uint8_t)*s;   // UTF-16LE（ASCII だけ）
            buf[len++] = 0;
        }
    } else {
        ep0_stall();
        return;
    }
    buf[0] = (uint8_t)len;
    buf[1] = 3;
    ep0_reply(buf, len, max);
}

static void activate_endpoints(void)
{
    // バルク IN（送信）
    mmio_write32(DIEPCTL(EP_TX), DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_BULK | DXEPCTL_TXFNUM(1) |
                                 DXEPCTL_SETD0PID | DXEPCTL_SNAK | bulk_mps);
    // 割り込み IN（通知。送らない）
    mmio_write32(DIEPCTL(EP_NOTIFY), DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_INTR | DXEPCTL_TXFNUM(2) |
                                     DXEPCTL_SETD0PID | DXEPCTL_SNAK | 16);
    // バルク OUT（受信）
    mmio_write32(DOEPCTL(EP_RX), DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_BULK | DXEPCTL_SETD0PID | bulk_mps);
    mmio_write32(DOEPTSIZ(EP_RX), DXEPTSIZ_PKTCNT(1) | bulk_mps);
    mmio_write32(DOEPCTL(EP_RX), mmio_read32(DOEPCTL(EP_RX)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
    // ECM の通知（割り込み IN）。データのエンドポイントは代替設定 1 にされたとき（eth_activate）
    mmio_write32(DIEPCTL(EP_ETH_NOTIFY), DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_INTR | DXEPCTL_TXFNUM(4) |
                                         DXEPCTL_SETD0PID | DXEPCTL_SNAK | 16);
    mmio_write32(DAINTMSK, DAINT_IN(0) | DAINT_OUT(0) | DAINT_IN(EP_TX) | DAINT_OUT(EP_RX) |
                               DAINT_IN(EP_ETH) | DAINT_OUT(EP_ETH));
}

static void eth_arm_rx(void)
{
    // 1 フレーム（短いパケットで終わる）を最大 2048 バイトまで受け取る
    mmio_write32(DOEPTSIZ(EP_ETH), DXEPTSIZ_PKTCNT(2048 / bulk_mps) | 2048);
    mmio_write32(DOEPCTL(EP_ETH), mmio_read32(DOEPCTL(EP_ETH)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
}

// データインターフェースの代替設定（0 = 止める、1 = 使う）
static void eth_set_alt(int alt)
{
    eth_txq_head = eth_txq_tail = 0;
    eth_tx_busy = eth_tx_zlp = false;
    eth_rx_len = 0;
    if (alt != 1) {
        eth_active = false;
        for (int in = 0; in < 2; in++) {
            uintptr_t ctl = in ? DIEPCTL(EP_ETH) : DOEPCTL(EP_ETH);
            if (mmio_read32(ctl) & DXEPCTL_EPENA)
                mmio_write32(ctl, DXEPCTL_EPDIS | DXEPCTL_SNAK);
            mmio_write32(ctl, mmio_read32(ctl) & ~DXEPCTL_USBACTEP);
        }
        return;
    }
    mmio_write32(DIEPCTL(EP_ETH), DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_BULK | DXEPCTL_TXFNUM(3) |
                                  DXEPCTL_SETD0PID | DXEPCTL_SNAK | bulk_mps);
    mmio_write32(DOEPCTL(EP_ETH), DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_BULK | DXEPCTL_SETD0PID | bulk_mps);
    eth_arm_rx();
    eth_active = true;
    // NETWORK_CONNECTION（つながった）を通知する（ECM 1.2 6.3.1。Linux の cdc_ether はこれでリンクを上げる）
    static const uint8_t notify[8] = { 0xA1, 0x00, 0x01, 0x00, IF_ETH_COMM, 0x00, 0x00, 0x00 };
    mmio_write32(DIEPTSIZ(EP_ETH_NOTIFY), DXEPTSIZ_PKTCNT(1) | sizeof(notify));
    mmio_write32(DIEPCTL(EP_ETH_NOTIFY), mmio_read32(DIEPCTL(EP_ETH_NOTIFY)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
    fifo_write(EP_ETH_NOTIFY, notify, sizeof(notify));
}

// 送信待ちの先頭のフレームを送る（usb_lock の中で）
static void eth_start_tx(void)
{
    if (eth_tx_busy || !eth_active || eth_txq_head == eth_txq_tail)
        return;
    uint32_t k = eth_txq_tail % ETH_Q;
    uint32_t len = eth_txq_len[k];
    mmio_write32(DIEPTSIZ(EP_ETH), DXEPTSIZ_PKTCNT((len + bulk_mps - 1) / bulk_mps) | len);
    mmio_write32(DIEPCTL(EP_ETH), mmio_read32(DIEPCTL(EP_ETH)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
    fifo_write(EP_ETH, eth_txq[k], len);
    eth_txq_tail++;
    eth_tx_busy = true;
    eth_tx_zlp = len % bulk_mps == 0;   // 最大パケット長の倍数なら、長さ 0 のパケットでフレームの終わりを示す
    eth_tx_frames++;
}

bool usb_eth_send(const void *frame, uint32_t len)
{
    if (!eth_active || len == 0 || len > ETH_FRAME_MAX)
        return false;
    uint64_t daif = usb_lock_take();
    bool ok = eth_active && eth_txq_head - eth_txq_tail < ETH_Q;
    if (ok) {
        uint32_t k = eth_txq_head % ETH_Q;
        memcpy(eth_txq[k], frame, len);
        eth_txq_len[k] = (uint16_t)len;
        eth_txq_head++;
        eth_start_tx();
    } else {
        eth_tx_dropped++;
    }
    usb_lock_give(daif);
    return ok;
}

static void start_tx(void);
static void abort_tx(void);
static void usb_flush(void);

static void handle_setup(void)
{
    uint8_t type = setup_pkt[0], req = setup_pkt[1];
    uint16_t value = setup_pkt[2] | setup_pkt[3] << 8;
    uint16_t index = setup_pkt[4] | setup_pkt[5] << 8;
    uint16_t length = setup_pkt[6] | setup_pkt[7] << 8;

    ep0_line_coding = false;
    ep0_out_discard = false;
    switch (type << 8 | req) {
    case 0x8006: { // GET_DESCRIPTOR
        const void *d = nullptr;
        uint32_t len = 0;
        switch (value >> 8) {
        case 1: d = device_desc; len = sizeof(device_desc); break;
        case 2:
            len = build_config(bulk_mps);
            d = config_desc;
            break;
        case 3: send_string(value & 0xFF, length); return;
        case 6: d = qualifier_desc; len = sizeof(qualifier_desc); break;
        default: ep0_stall(); return;
        }
        ep0_reply(d, len, length);
        return;
    }
    case 0x0005: // SET_ADDRESS（状態のステージより前に設定してよい。コントローラが扱う）
        mmio_write32(DCFG, (mmio_read32(DCFG) & ~DCFG_DEVADDR_MASK) | (uint32_t)(value & 0x7F) << 4);
        ep0_send(nullptr, 0);
        return;
    case 0x0009: // SET_CONFIGURATION
        if ((value == 1) != configured)
            klog_printf("usb: %s by the host", value == 1 ? "configured" : "unconfigured");
        configured = value == 1;
        if (configured)
            activate_endpoints();
        tx_busy = false;
        eth_set_alt(0);
        ep0_send(nullptr, 0);
        return;
    case 0x8008: { // GET_CONFIGURATION
        uint8_t c = configured;
        ep0_send(&c, 1);
        return;
    }
    case 0x8000: case 0x8100: case 0x8200: { // GET_STATUS
        uint8_t s[2] = { 0, 0 };
        ep0_send(s, length < 2 ? length : 2);
        return;
    }
    case 0x810A: { // GET_INTERFACE
        uint8_t a = (index == IF_ETH_DATA && eth_active) ? 1 : 0;
        ep0_send(&a, 1);
        return;
    }
    case 0x010B: // SET_INTERFACE
        if (index == IF_ETH_DATA)
            eth_set_alt(value);
        ep0_send(nullptr, 0);
        return;
    case 0x0001: case 0x0101: case 0x0201: // CLEAR_FEATURE
    case 0x0003: case 0x0103: case 0x0203: // SET_FEATURE
        ep0_send(nullptr, 0);
        return;
    case 0x2120: // SET_LINE_CODING（OUT のデータ 7 バイトを待つ）
        ep0_line_coding = true;
        return;
    case 0xA121: // GET_LINE_CODING
        ep0_send(line_coding, length < 7 ? length : 7);
        return;
    case 0x2122: // SET_CONTROL_LINE_STATE
        dtr = value & 1;
        if (!dtr) { // 端末が閉じた: 送っていない分と、コントローラに渡した送信の残りを捨てる
            tx_head = tx_tail = 0;
            abort_tx();
        } else {
            start_tx();
        }
        ep0_send(nullptr, 0);
        return;
    case 0x2123: // SEND_BREAK
    case 0x2143: // SET_ETHERNET_PACKET_FILTER（ECM 6.2.4。フィルターはしない）
        if (length) {
            ep0_out_discard = true;
            return;
        }
        ep0_send(nullptr, 0);
        return;
    default:
        if ((type & 0x60) == 0x20 && !(type & 0x80) && length) {
            // そのほかのクラスのリクエスト（OUT のデータあり）: 受け取って捨てる
            ep0_out_discard = true;
            return;
        }
        ep0_stall();
        return;
    }
}

// ---- 送信（バルク IN） ----

// コントローラに渡したシリアルの送信（バルク IN）を取り消し、その TX FIFO を空にする（usb_lock の中で）。
// ホストが端末を閉じたとき: 閉じる前に渡した送信が残ると、開き直しても終わらず（2026-11-14 に実機で見た。
// パケットの残りの数も FIFO の空きも変わらない）、送信中の印（tx_busy）が外れないので、USB シリアルに何も出なく
// なった。手順は Linux の drivers/usb/dwc2/gadget.c の dwc2_hsotg_ep_stop_xfr（IN）と dwc2_flush_tx_fifo:
// SNAK → INEPNAKEFF を待つ → EPDIS → EPDISBLD を待つ → その TX FIFO を flush
static void abort_tx(void)
{
    if (mmio_read32(DIEPCTL(EP_TX)) & DXEPCTL_EPENA) {
        mmio_write32(DIEPCTL(EP_TX), mmio_read32(DIEPCTL(EP_TX)) | DXEPCTL_SNAK);
        wait_bits(DIEPINT(EP_TX), DXEPINT_INEPNAKEFF, DXEPINT_INEPNAKEFF, 1000);
        mmio_write32(DIEPCTL(EP_TX), mmio_read32(DIEPCTL(EP_TX)) | DXEPCTL_EPDIS | DXEPCTL_SNAK);
        wait_bits(DIEPINT(EP_TX), DXEPINT_EPDISBLD, DXEPINT_EPDISBLD, 1000);
        mmio_write32(DIEPINT(EP_TX), DXEPINT_INEPNAKEFF | DXEPINT_EPDISBLD | DXEPINT_XFERCOMPL);
    }
    mmio_write32(GRSTCTL, GRSTCTL_TXFNUM(1) | GRSTCTL_TXFFLSH);     // EP_TX の TX FIFO（activate_endpoints の TXFNUM(1)）
    wait_bits(GRSTCTL, GRSTCTL_TXFFLSH, 0, 1000);
    tx_busy = false;
    tx_stalled = false;
}

// 送信バッファの中身を、FIFO に収まる分だけ送る（usb_lock の中で）
static void start_tx(void)
{
    if (tx_busy || !configured || !dtr || tx_head == tx_tail)
        return;
    uint32_t n = (tx_head - tx_tail) % TX_BUF_SIZE;
    if (n > TXFIFO1_WORDS * 4)
        n = TXFIFO1_WORDS * 4;
    uint32_t pkts = (n + bulk_mps - 1) / bulk_mps;
    mmio_write32(DIEPTSIZ(EP_TX), DXEPTSIZ_PKTCNT(pkts) | n);
    mmio_write32(DIEPCTL(EP_TX), mmio_read32(DIEPCTL(EP_TX)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i + k < n; k++) {
            w |= (uint32_t)tx_buf[tx_tail] << (8 * k);
            tx_tail = (tx_tail + 1) % TX_BUF_SIZE;
        }
        mmio_write32(EPFIFO(EP_TX), w);
    }
    tx_busy = true;
}

// コンソールの出力（uart_putc から）。端末が開かれていて、ホストが受け取っていれば true
// （受け取っていない（tx_stalled）ときは false。UART の出力をいつもどおり待たせる）
static bool usb_putc(char c)
{
    if (!configured || !dtr)
        return false;
    uint64_t deadline = 0;
    for (;;) {
        uint64_t daif = usb_lock_take();
        uint32_t next = (tx_head + 1) % TX_BUF_SIZE;
        if (next != tx_tail || tx_stalled) {
            if (next != tx_tail) {
                tx_buf[tx_head] = (uint8_t)c;
                tx_head = next;
            } else {
                tx_dropped++;
            }
            start_tx();
            bool active = !tx_stalled;
            usb_lock_give(daif);
            return active;
        }
        usb_lock_give(daif);
        // いっぱい: 送り終わるのを待つ。割り込みを止めた場所やほかのコアから呼ばれることもあるので、
        // 割り込みの処理を自分で呼ぶ
        uint64_t now = timer_now_us();
        if (!deadline)
            deadline = now + 500000;
        else if (now > deadline)
            tx_stalled = true;
        usb_handle_irq();
        if (!configured || !dtr)
            return false;
    }
}

// ---- 割り込み ----

static void bus_reset(void)
{
    if (configured)
        klog_printf("usb: bus reset (the host reconnected or went away)");
    configured = false;
    dtr = false;
    tx_busy = false;
    tx_stalled = false;
    tx_head = tx_tail = 0;
    mmio_write32(DCFG, mmio_read32(DCFG) & ~DCFG_DEVADDR_MASK);
    mmio_write32(GRSTCTL, GRSTCTL_TXFNUM_ALL | GRSTCTL_TXFFLSH);
    wait_bits(GRSTCTL, GRSTCTL_TXFFLSH, 0, 1000);
    eth_set_alt(0);
    for (int n = 1; n <= 5; n++) {
        if (mmio_read32(DIEPCTL(n)) & DXEPCTL_EPENA)
            mmio_write32(DIEPCTL(n), DXEPCTL_EPDIS | DXEPCTL_SNAK);
        if (mmio_read32(DOEPCTL(n)) & DXEPCTL_EPENA)
            mmio_write32(DOEPCTL(n), DXEPCTL_EPDIS | DXEPCTL_SNAK);
    }
    mmio_write32(DAINTMSK, DAINT_IN(0) | DAINT_OUT(0));
    ep0_arm_out();
}

// RX FIFO から 1 つ取り出す
static void read_rx_fifo(void)
{
    uint32_t st = mmio_read32(GRXSTSP);
    int ep = GRXSTS_EPNUM(st);
    uint32_t cnt = GRXSTS_BYTECNT(st);
    uint32_t words = (cnt + 3) / 4;
    switch (GRXSTS_PKTSTS(st)) {
    case PKTSTS_SETUPRX: {
        uint32_t w0 = mmio_read32(EPFIFO(0)), w1 = mmio_read32(EPFIFO(0));
        memcpy(setup_pkt, &w0, 4);
        memcpy(setup_pkt + 4, &w1, 4);
        return;
    }
    case PKTSTS_OUTRX:
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = mmio_read32(EPFIFO(0));
            if (i < sizeof(rx_buf_words) / 4)
                rx_buf_words[i] = w;
        }
        if (cnt > sizeof(rx_buf_words))
            cnt = sizeof(rx_buf_words);
        if (ep == 0) {
            memcpy(ep0_buf, rx_buf_words, cnt < sizeof(ep0_buf) ? cnt : sizeof(ep0_buf));
            ep0_out_len = cnt;
        } else if (ep == EP_RX && cnt) {
            uart_rx_push((const char *)rx_buf_words, cnt);
        } else if (ep == EP_ETH && cnt) {
            if (eth_rx_len + cnt <= sizeof(eth_rx_frame))
                memcpy(eth_rx_frame + eth_rx_len, rx_buf_words, cnt);
            eth_rx_len += cnt;
        }
        return;
    default: // OUT・SETUP の完了など（データなし）
        for (uint32_t i = 0; i < words; i++)
            mmio_read32(EPFIFO(0));
        return;
    }
}

void usb_handle_irq(void)
{
    uint64_t daif = usb_lock_take();
    uint32_t sts = mmio_read32(GINTSTS) & mmio_read32(GINTMSK);

    if (sts & GINTSTS_USBRST) {
        mmio_write32(GINTSTS, GINTSTS_USBRST);
        bus_reset();
    }
    if (sts & GINTSTS_ENUMDONE) {
        mmio_write32(GINTSTS, GINTSTS_ENUMDONE);
        bulk_mps = DSTS_ENUMSPD(mmio_read32(DSTS)) == DSTS_ENUMSPD_HS ? 512 : 64;
        mmio_write32(DIEPCTL(0), 0);    // 最大パケット長 64
        mmio_write32(DCTL, mmio_read32(DCTL) | DCTL_CGOUTNAK | DCTL_CGNPINNAK);
    }
    while (mmio_read32(GINTSTS) & GINTSTS_RXFLVL)
        read_rx_fifo();
    if (sts & GINTSTS_OEPINT) {
        uint32_t daint = mmio_read32(DAINT);
        if (daint & DAINT_OUT(0)) {
            uint32_t i = mmio_read32(DOEPINT(0));
            mmio_write32(DOEPINT(0), i);
            if (i & DXEPINT_SETUP) {
                handle_setup();
            } else if ((i & DXEPINT_XFERCOMPL) && ep0_line_coding) {
                // SET_LINE_CODING のデータ。状態のステージ（長さ 0 の IN）を返す
                if (ep0_out_len >= 7)
                    memcpy(line_coding, ep0_buf, 7);
                ep0_line_coding = false;
                ep0_send(nullptr, 0);
            } else if ((i & DXEPINT_XFERCOMPL) && ep0_out_discard) {
                ep0_out_discard = false;
                ep0_send(nullptr, 0);
            }
            ep0_arm_out();
        }
        if (daint & DAINT_OUT(EP_RX)) {
            uint32_t i = mmio_read32(DOEPINT(EP_RX));
            mmio_write32(DOEPINT(EP_RX), i);
            if (i & DXEPINT_XFERCOMPL) {
                mmio_write32(DOEPTSIZ(EP_RX), DXEPTSIZ_PKTCNT(1) | bulk_mps);
                mmio_write32(DOEPCTL(EP_RX), mmio_read32(DOEPCTL(EP_RX)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
            }
        }
        if (daint & DAINT_OUT(EP_ETH)) {
            uint32_t i = mmio_read32(DOEPINT(EP_ETH));
            mmio_write32(DOEPINT(EP_ETH), i);
            if ((i & DXEPINT_XFERCOMPL) && eth_active) {
                // 1 フレームを受け取った。ロックの外で net_input に渡すため、待ち行列に入れる
                if (eth_rx_len >= 14 && eth_rx_len <= ETH_FRAME_MAX && eth_rxq_head - eth_rxq_tail < ETH_RQ) {
                    uint32_t k = eth_rxq_head % ETH_RQ;
                    memcpy(eth_rxq[k], eth_rx_frame, eth_rx_len);
                    eth_rxq_len[k] = (uint16_t)eth_rx_len;
                    eth_rxq_head++;
                    eth_rx_frames++;
                } else if (eth_rx_len) {
                    eth_rx_dropped++;
                }
                eth_rx_len = 0;
                eth_arm_rx();
            }
        }
    }
    if (sts & GINTSTS_IEPINT) {
        uint32_t daint = mmio_read32(DAINT);
        if (daint & DAINT_IN(0)) {
            uint32_t i = mmio_read32(DIEPINT(0));
            mmio_write32(DIEPINT(0), i);
            if (i & DXEPINT_XFERCOMPL)
                ep0_in_done();
        }
        if (daint & DAINT_IN(EP_ETH)) {
            uint32_t i = mmio_read32(DIEPINT(EP_ETH));
            mmio_write32(DIEPINT(EP_ETH), i);
            if (i & DXEPINT_XFERCOMPL) {
                if (eth_tx_zlp) {
                    eth_tx_zlp = false;
                    mmio_write32(DIEPTSIZ(EP_ETH), DXEPTSIZ_PKTCNT(1));
                    mmio_write32(DIEPCTL(EP_ETH), mmio_read32(DIEPCTL(EP_ETH)) | DXEPCTL_EPENA | DXEPCTL_CNAK);
                } else {
                    eth_tx_busy = false;
                    eth_start_tx();
                }
            }
        }
        if (daint & DAINT_IN(EP_TX)) {
            uint32_t i = mmio_read32(DIEPINT(EP_TX));
            mmio_write32(DIEPINT(EP_TX), i);
            if (i & DXEPINT_XFERCOMPL) {
                tx_busy = false;
                tx_stalled = false;
                start_tx();
            }
        }
    }
    usb_lock_give(daif);

    // 受け取ったフレームをネットワークの処理に渡す（ロックの外で。返事は usb_eth_send で送る）
    for (;;) {
        static uint8_t frame[ETH_FRAME_MAX];
        uint32_t len = 0;
        daif = usb_lock_take();
        if (eth_rxq_tail != eth_rxq_head) {
            uint32_t k = eth_rxq_tail % ETH_RQ;
            len = eth_rxq_len[k];
            memcpy(frame, eth_rxq[k], len);
            eth_rxq_tail++;
        }
        usb_lock_give(daif);
        if (!len)
            break;
        net_input(frame, len);
    }
}

// ---- 初期化 ----

static bool core_reset(void)
{
    if (!wait_bits(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100000))
        return false;
    mmio_write32(GRSTCTL, GRSTCTL_CSFTRST);
    if (!wait_bits(GRSTCTL, GRSTCTL_CSFTRST, 0, 100000))
        return false;
    return wait_bits(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100000);
}

bool usb_init(void)
{
    uint32_t state;
    if (!mailbox_set_power(MBOX_DEVICE_USB, true, &state) || !(state & 1))
        return false;
    if ((mmio_read32(GSNPSID) & 0xFFFF0000) != 0x4F540000)  // "OT"（DWC2）
        return false;

    mmio_write32(GAHBCFG, 0);   // 割り込みを止める
    if (!core_reset())
        return false;
    // 機器側に固定する（ファームウェアはホスト側に固定している）。切り替えには最大 25ms かかる（Linux の dwc2_force_mode）
    uint32_t usbcfg = mmio_read32(GUSBCFG);
    usbcfg &= ~(GUSBCFG_FORCEHOSTMODE | GUSBCFG_HNPCAP | GUSBCFG_SRPCAP | GUSBCFG_TOUTCAL_MASK);
    usbcfg |= GUSBCFG_FORCEDEVMODE | 7;
    mmio_write32(GUSBCFG, usbcfg);
    if (!wait_bits(GINTSTS, GINTSTS_CURMODE_HOST, 0, 110000))
        return false;
    mmio_write32(PCGCTL, 0);

    mmio_write32(DCTL, DCTL_SFTDISCON);     // 準備ができるまで切り離しておく
    mmio_write32(DCFG, DCFG_EPMISCNT(1));   // High Speed、アドレス 0

    // FIFO の配置
    mmio_write32(GRXFSIZ, RXFIFO_WORDS);
    mmio_write32(GNPTXFSIZ, TXFIFO0_WORDS << 16 | RXFIFO_WORDS);
    mmio_write32(DPTXFSIZN(1), TXFIFO1_WORDS << 16 | (RXFIFO_WORDS + TXFIFO0_WORDS));
    mmio_write32(DPTXFSIZN(2), TXFIFO2_WORDS << 16 | (RXFIFO_WORDS + TXFIFO0_WORDS + TXFIFO1_WORDS));
    mmio_write32(DPTXFSIZN(3), TXFIFO3_WORDS << 16 |
                                   (RXFIFO_WORDS + TXFIFO0_WORDS + TXFIFO1_WORDS + TXFIFO2_WORDS));
    mmio_write32(DPTXFSIZN(4), TXFIFO4_WORDS << 16 | (RXFIFO_WORDS + TXFIFO0_WORDS + TXFIFO1_WORDS +
                                                      TXFIFO2_WORDS + TXFIFO3_WORDS));
    mmio_write32(GRSTCTL, GRSTCTL_TXFNUM_ALL | GRSTCTL_TXFFLSH);
    wait_bits(GRSTCTL, GRSTCTL_TXFFLSH, 0, 1000);
    mmio_write32(GRSTCTL, GRSTCTL_RXFFLSH);
    wait_bits(GRSTCTL, GRSTCTL_RXFFLSH, 0, 1000);

    mmio_write32(GINTSTS, 0xFFFFFFFF);
    mmio_write32(GINTMSK, GINTSTS_USBRST | GINTSTS_ENUMDONE | GINTSTS_RXFLVL |
                          GINTSTS_OEPINT | GINTSTS_IEPINT);
    mmio_write32(DIEPMSK, DXEPINT_XFERCOMPL);
    mmio_write32(DOEPMSK, DXEPINT_SETUP | DXEPINT_XFERCOMPL);
    mmio_write32(DAINTMSK, DAINT_IN(0) | DAINT_OUT(0));
    bulk_mps = 512;
    ep0_arm_out();

    mmio_write32(GAHBCFG, GAHBCFG_GLBL_INTR_EN);
    mmio_write32(IRQ_ENABLE_1, IRQ_USB);
    usb_ready = true;
    uart_add_tx_mirror(usb_putc, usb_flush);
    delay_us(3000);
    mmio_write32(DCTL, mmio_read32(DCTL) & ~DCTL_SFTDISCON);   // つなぐ（ホストが列挙を始める）
    return true;
}

// 送信バッファの中身を送り切る（最大 200ms。割り込みを止めた場所からも呼ぶので、割り込みの処理を自分で呼ぶ）
static void usb_flush(void)
{
    if (!usb_ready)
        return;
    uint64_t end = timer_now_us() + 200000;
    while ((tx_busy || tx_head != tx_tail) && configured && dtr && !tx_stalled && timer_now_us() < end)
        usb_handle_irq();
}

void usb_disconnect(void)
{
    if (!usb_ready)
        return;
    usb_flush();    // 送っている途中の分を送り終えてから、ホストから切り離す
    mmio_write32(DCTL, mmio_read32(DCTL) | DCTL_SFTDISCON);
}

void usb_status(void)
{
    if (!usb_ready) {
        printf("usb: not initialized\n");
        return;
    }
    uint32_t dsts = mmio_read32(DSTS);
    printf("usb: %s, %s, address %u, DTR %d, tx dropped %llu\n",
           configured ? "configured" : "not configured (no host?)",
           !configured ? "-" : DSTS_ENUMSPD(dsts) == DSTS_ENUMSPD_HS ? "high speed" : "full speed",
           (unsigned)((mmio_read32(DCFG) & DCFG_DEVADDR_MASK) >> 4), dtr,
           (unsigned long long)tx_dropped);
    printf("  ethernet (CDC-ECM): %s, rx %llu frames (dropped %llu), tx %llu frames (dropped %llu)\n",
           eth_active ? "up" : "down", (unsigned long long)eth_rx_frames,
           (unsigned long long)eth_rx_dropped, (unsigned long long)eth_tx_frames,
           (unsigned long long)eth_tx_dropped);
    printf("  GINTSTS %08x GOTGCTL %08x DCTL %08x DSTS %08x\n", (unsigned)mmio_read32(GINTSTS),
           (unsigned)mmio_read32(GOTGCTL), (unsigned)mmio_read32(DCTL), (unsigned)dsts);
    // シリアルの送信（バルク IN）: 送信中か、止まりの印、送信バッファに残っている量、エンドポイントのレジスタ
    printf("  serial tx: busy %d stalled %d queued %u, DIEPCTL %08x DIEPTSIZ %08x DTXFSTS %08x\n", tx_busy,
           tx_stalled, (unsigned)((tx_head - tx_tail) % TX_BUF_SIZE), (unsigned)mmio_read32(DIEPCTL(EP_TX)),
           (unsigned)mmio_read32(DIEPTSIZ(EP_TX)), (unsigned)mmio_read32(DTXFSTS(EP_TX)));
}
