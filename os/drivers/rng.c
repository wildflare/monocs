// ハードウェアの乱数生成器（BCM2835 の RNG、0x3F104000）。POSIX の getentropy の元になる
// （TLS の鍵・nonce など、当てられては困る乱数に。rand() は当てられる）。
//
// ペリフェラル資料に説明がないので、レジスタと手順は Linux の drivers/char/hw_random/bcm2835-rng.c による。
// Zero 2 W のデバイスツリー（bcm2710-rpi-zero-2-w.dtb）の rng@7e104000 が compatible = "brcm,bcm2835-rng"。
//   CTRL   (+0x00)  ビット 0 RBGEN: 生成する
//   STATUS (+0x04)  上位 8 ビット: 読める語の数。下位 20 ビット: 始めに捨てる数（ウォームアップ。動かす前に書く）
//   DATA   (+0x08)  乱数（32 ビット）
//   INT_MASK (+0x10) ビット 0: 割り込みを止める（使わない）
// QEMU の raspi3b にもある（bcm2835_rng）。

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "bcm2837.h"
#include "mmio.h"
#include "timer.h"

#define RNG_BASE            (PERIPHERAL_BASE + 0x104000)
#define RNG_CTRL            (RNG_BASE + 0x00)
#define RNG_STATUS          (RNG_BASE + 0x04)
#define RNG_DATA            (RNG_BASE + 0x08)
#define RNG_INT_MASK        (RNG_BASE + 0x10)
#define RNG_RBGEN           0x1
#define RNG_INT_OFF         0x1
#define RNG_WARMUP_COUNT    0x40000     // 始めに捨てる数（Linux と同じ）
#define RNG_TIMEOUT_US      1000000     // 1 語をこれだけ待っても出なければ失敗（ウォームアップは実機で約 0.3 秒）

static atomic_flag rng_busy = ATOMIC_FLAG_INIT;

// 1 語を読む。出なければ false
static bool rng_word(uint32_t *out)
{
    if (!(mmio_read32(RNG_CTRL) & RNG_RBGEN)) {
        mmio_write32(RNG_INT_MASK, mmio_read32(RNG_INT_MASK) | RNG_INT_OFF);
        mmio_write32(RNG_STATUS, RNG_WARMUP_COUNT);
        mmio_write32(RNG_CTRL, RNG_RBGEN);
    }
    uint64_t end = timer_now_us() + RNG_TIMEOUT_US;
    while ((mmio_read32(RNG_STATUS) >> 24) == 0)
        if (timer_now_us() > end)
            return false;
    *out = mmio_read32(RNG_DATA);
    return true;
}

// POSIX: buf を len バイト（256 まで）の乱数で埋める。0 で成功、-1 は失敗（errno は EIO）
int getentropy(void *buf, size_t len)
{
    if (len > 256) {
        errno = EIO;
        return -1;
    }
    while (atomic_flag_test_and_set_explicit(&rng_busy, memory_order_acquire))
        __asm__ volatile("yield");
    uint8_t *p = buf;
    bool ok = true;
    while (len > 0 && ok) {
        uint32_t w;
        ok = rng_word(&w);
        size_t n = len < 4 ? len : 4;
        if (ok)
            memcpy(p, &w, n);
        p += n;
        len -= n;
    }
    atomic_flag_clear_explicit(&rng_busy, memory_order_release);
    if (!ok)
        errno = EIO;
    return ok ? 0 : -1;
}
