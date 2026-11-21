// メールボックスのプロパティインターフェース
// 出典: https://github.com/raspberrypi/firmware/wiki/Mailboxes
//       https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
//       BCM2835 ARM Peripherals 1.2.4（バスアドレス 0xC0000000 = L2 キャッシュを通らない別名）

#include "mailbox.h"
#include "bcm2837.h"
#include "mmio.h"
#include "../kernel/lock.h"

#define MBOX_FULL           0x80000000U
#define MBOX_EMPTY          0x40000000U
#define MBOX_CH_PROPERTY    8

#define MBOX_REQUEST        0x00000000U
#define MBOX_RESPONSE_OK    0x80000000U
#define MBOX_TAG_END        0

#define TAG_GET_ARM_MEMORY  0x00010005
#define TAG_GET_CLOCK_RATE  0x00030002

// VideoCore から見た SDRAM の別名（キャッシュを通らない）
// （実機で、この別名で応答が返ることを確認済み。2026-11-04）
#define BUS_ALIAS_UNCACHED  0xC0000000U

#define CACHE_LINE          64

// バッファを VideoCore と共有する前後に、データキャッシュを書き戻して無効化する
static void dcache_clean_invalidate(volatile void *p, uint32_t size)
{
    uintptr_t start = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)p + size;
    for (uintptr_t a = start; a < end; a += CACHE_LINE)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// mailbox は 1 つなので、コアどうしで同時に使わない（2026-11-19。ロックがなく、コア 0 の reboot tryboot（再起動の
// 印）とコア 3 の sshd の monocs_sysstat（温度・クロック）が重なると、一方がもう一方の返事を読んで捨て、取られた側が
// 来ない返事を待ち続けた。nos.py kernel の途中でシェルが止まり、電源を入れ直すしかなかった（3 回）。
// MMU の前（起動の始め。コアは 1 つ）は排他アクセスを使えないので、ロックを取らない（os/kernel/lock.c と同じ考え）
static struct klock mbox_lock = KLOCK_INIT;

static bool mmu_on(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v & 1;
}

static bool mailbox_call_locked(volatile uint32_t *buf);

bool mailbox_call(volatile uint32_t *buf)
{
    if (!mmu_on())
        return mailbox_call_locked(buf);
    klock_acquire(&mbox_lock);
    bool ok = mailbox_call_locked(buf);
    klock_release(&mbox_lock);
    return ok;
}

static bool mailbox_call_locked(volatile uint32_t *buf)
{
    uint32_t size = buf[0];
    uint32_t msg = ((uint32_t)(uintptr_t)buf | BUS_ALIAS_UNCACHED) | MBOX_CH_PROPERTY;

    dcache_clean_invalidate(buf, size);

    while (mmio_read32(MBOX_STATUS) & MBOX_FULL)
        ;
    mmio_write32(MBOX_WRITE, msg);

    for (;;) {
        while (mmio_read32(MBOX_STATUS) & MBOX_EMPTY)
            ;
        if (mmio_read32(MBOX_READ) == msg)
            break;
    }

    dcache_clean_invalidate(buf, size);
    return buf[1] == MBOX_RESPONSE_OK;
}

uint32_t mailbox_get_clock_rate(uint32_t clock_id)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 8 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = TAG_GET_CLOCK_RATE;
    buf[3] = 8;     // 値バッファのサイズ
    buf[4] = 0;     // 要求
    buf[5] = clock_id;
    buf[6] = 0;     // 応答: 周波数（Hz）
    buf[7] = MBOX_TAG_END;

    if (!mailbox_call(buf))
        return 0;
    return buf[6];
}

bool mailbox_get_value(uint32_t tag, uint32_t id, uint32_t *value)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 8 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = tag;
    buf[3] = 8;     // 値バッファのサイズ
    buf[4] = 0;     // 要求
    buf[5] = id;
    buf[6] = 0;     // 応答: 値
    buf[7] = MBOX_TAG_END;

    if (!mailbox_call(buf) || !(buf[4] & MBOX_RESPONSE_OK))
        return false;
    *value = buf[6];
    return true;
}

// クロックを頼む（Set clock rate、0x00038002。値: クロックの番号、Hz、skip setting turbo（0 なら、ARM を最小より上に
// するとファームウェアがターボ（電圧を上げる）にする））。実際の値（Hz）を返す。0 は失敗。
// 出典: Raspberry Pi のファームウェアのメールボックスのプロパティの説明（Mailbox property interface）、
//       Linux の drivers/clk/bcm/clk-raspberrypi.c
uint32_t mailbox_set_clock_rate(uint32_t clock_id, uint32_t hz, bool skip_turbo)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 9 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = 0x00038002;
    buf[3] = 12;    // 値バッファのサイズ
    buf[4] = 0;     // 要求
    buf[5] = clock_id;
    buf[6] = hz;
    buf[7] = skip_turbo ? 1 : 0;
    buf[8] = MBOX_TAG_END;

    if (!mailbox_call(buf))
        return 0;
    return buf[6];
}

// 次の再起動の印（Set reboot flags、0x00038064。ビット 0 = tryboot: 次の 1 回だけ、config.txt の代わりに tryboot.txt を
// 読む）。出典: Linux の include/soc/bcm2835/raspberrypi-firmware.h（RPI_FIRMWARE_SET_REBOOT_FLAGS）と
// drivers/firmware/raspberrypi.c（reboot "0 tryboot"）。Zero 2 W（bootcode.bin の機種）で効くかは資料にない
bool mailbox_set_reboot_flags(uint32_t flags)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 7 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = 0x00038064;
    buf[3] = 4;     // 値バッファのサイズ
    buf[4] = 0;     // 要求
    buf[5] = flags;
    buf[6] = MBOX_TAG_END;

    return mailbox_call(buf) && (buf[4] & MBOX_RESPONSE_OK);
}

// カーネルのコマンドライン（Get command line、0x00050001。config.txt の cmdline= のファイルの中身）。読めなければ ""
void mailbox_get_cmdline(char *out, size_t size)
{
    alignas(CACHE_LINE) volatile uint32_t buf[80];     // 値バッファ 256 バイト

    buf[0] = sizeof buf;
    buf[1] = MBOX_REQUEST;
    buf[2] = 0x00050001;
    buf[3] = 256;
    buf[4] = 0;
    for (int i = 5; i < 5 + 64; i++)
        buf[i] = 0;
    buf[5 + 64] = MBOX_TAG_END;
    out[0] = '\0';
    if (!mailbox_call(buf) || !(buf[4] & MBOX_RESPONSE_OK) || size == 0)
        return;
    size_t n = buf[4] & 0x7FFFFFFF;     // 応答の長さ
    if (n > 256)
        n = 256;
    if (n >= size)
        n = size - 1;
    const volatile char *src = (const volatile char *)&buf[5];
    for (size_t i = 0; i < n; i++)
        out[i] = src[i];
    out[n] = '\0';
}

#define TAG_SET_SDHOST_CLOCK 0x00038042

uint32_t mailbox_set_sdhost_clock(uint32_t hz)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 9 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = TAG_SET_SDHOST_CLOCK;
    buf[3] = 12;    // 値バッファのサイズ（要求: クロック、応答: クロック 2 つ）
    buf[4] = 0;
    buf[5] = hz;
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = MBOX_TAG_END;

    if (!mailbox_call(buf) || !(buf[4] & MBOX_RESPONSE_OK))
        return 0;
    return buf[6] > buf[7] ? buf[6] : buf[7];
}

#define TAG_SET_POWER_STATE 0x00028001
#define POWER_ON            (1U << 0)
#define POWER_WAIT          (1U << 1)

bool mailbox_set_power(uint32_t device_id, bool on, uint32_t *state)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 8 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = TAG_SET_POWER_STATE;
    buf[3] = 8;     // 値バッファのサイズ
    buf[4] = 0;     // 要求
    buf[5] = device_id;
    buf[6] = (on ? POWER_ON : 0) | POWER_WAIT;  // 応答: ビット 0 = 電源、ビット 1 = デバイスがない
    buf[7] = MBOX_TAG_END;

    if (!mailbox_call(buf) || !(buf[4] & MBOX_RESPONSE_OK))
        return false;
    *state = buf[6];
    return true;
}

bool mailbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    alignas(CACHE_LINE) volatile uint32_t buf[16];

    buf[0] = 8 * sizeof(uint32_t);
    buf[1] = MBOX_REQUEST;
    buf[2] = TAG_GET_ARM_MEMORY;
    buf[3] = 8;     // 値バッファのサイズ
    buf[4] = 0;     // 要求
    buf[5] = 0;     // 応答: ベースアドレス
    buf[6] = 0;     // 応答: サイズ
    buf[7] = MBOX_TAG_END;

    if (!mailbox_call(buf))
        return false;
    *base = buf[5];
    *size = buf[6];
    return true;
}
