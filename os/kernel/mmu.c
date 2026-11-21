// MMU を恒等マップで有効にする（4KB 粒度、仮想アドレス 32 ビット）
// 出典: ARM DDI 0487 D8（VMSAv8-64 アドレス変換）、
//       D13.2（MAIR_EL1、TCR_EL1、SCTLR_EL1）
//
// マップ:
//   0x00000000-0x00000FFF  未マップ（NULL 参照を例外にする）
//   0x00001000-0x1FFFFFFF  Normal（ライトバック、キャッシュ有効）  RAM 512MB
//   0x20000000-0x3EFFFFFF  未マップ（実機に RAM がない領域）
//   0x3F000000-0x3FFFFFFF  Device-nGnRnE  周辺機器
//   0x40000000-0x7FFFFFFF  Device-nGnRnE  ARM ローカル周辺
//
// レベル 1 は 1GB 単位で 4 エントリ。最初の 1GB はレベル 2（2MB ブロック）に分け、
// 先頭 2MB はさらにレベル 3（4KB ページ）に分けて 0 番地のページだけ外す。
//
// システムを守るため（アプリのバグでカーネルが壊れないように）、4KB ページに分けて次の 2 つも行う:
//   - カーネルの .text・.rodata（0x80000〜.tdata の終わりのページ）は読み出し専用。書くと例外になる
//   - 各コアのスタックのいちばん下の 64KB（ガードページ）は無効。スタックがあふれたら例外になる
//     （例外の入口で気づき、別のスタックに切り替えて表示する。os/kernel/vectors.S）
// アプリも EL1 で動くので、わざと MMU の設定を書き換えれば外せる。守るのは、うっかりからだけ。

#include <stdint.h>
#include "mmu.h"
#include "sysreg.h"

#define DEVICE_START        0x3F000000UL
#define LOCAL_START         0x40000000UL

#define PAGE_SIZE           0x1000UL
#define L2_BLOCK_SIZE       0x200000UL

// MAIR_EL1 の属性番号
#define MAIR_IDX_DEVICE     0   // 0x00: Device-nGnRnE
#define MAIR_IDX_NORMAL     1   // 0xFF: Normal、内外ともライトバック・リード／ライトアロケート
#define MAIR_VALUE          ((0x00UL << (8 * MAIR_IDX_DEVICE)) | (0xFFUL << (8 * MAIR_IDX_NORMAL)))

// 記述子のビット（D8.3）
#define DESC_VALID          (1UL << 0)
#define DESC_TABLE          (1UL << 1)  // レベル 1/2 ではテーブル、レベル 3 ではページ
#define DESC_BLOCK          (0UL << 1)
#define DESC_PAGE           (1UL << 1)
#define DESC_ATTR(idx)      ((uint64_t)(idx) << 2)
#define DESC_AP_EL1_RW      (0UL << 6)  // EL1 のみ読み書き可（OS もアプリも EL1）
#define DESC_AP_EL1_RO      (2UL << 6)  // EL1 のみ読み出し可（D8.4.1、AP[2:1] = 0b10）
#define DESC_SH_INNER       (3UL << 8)
#define DESC_AF             (1UL << 10) // アクセスフラグ（立てないと初回アクセスで例外）
#define DESC_PXN            (1UL << 53)
#define DESC_UXN            (1UL << 54)

#define ATTR_NORMAL         (DESC_ATTR(MAIR_IDX_NORMAL) | DESC_AP_EL1_RW | DESC_SH_INNER | DESC_AF)
#define ATTR_NORMAL_RO      (DESC_ATTR(MAIR_IDX_NORMAL) | DESC_AP_EL1_RO | DESC_SH_INNER | DESC_AF)
#define ATTR_DEVICE         (DESC_ATTR(MAIR_IDX_DEVICE) | DESC_AP_EL1_RW | DESC_AF | DESC_PXN | DESC_UXN)

// TCR_EL1（D13.2.131）
#define TCR_T0SZ            (32UL << 0)  // 仮想アドレス 32 ビット → レベル 1 から引く
#define TCR_IRGN0_WBWA      (1UL << 8)
#define TCR_ORGN0_WBWA      (1UL << 10)
#define TCR_SH0_INNER       (3UL << 12)
#define TCR_TG0_4K          (0UL << 14)
#define TCR_EPD1            (1UL << 23)  // TTBR1 側は使わない
#define TCR_IPS_32BIT       (0UL << 32)
#define TCR_VALUE           (TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | \
                             TCR_TG0_4K | TCR_EPD1 | TCR_IPS_32BIT)

// SCTLR_EL1（D13.2.118）
#define SCTLR_M             (1UL << 0)
#define SCTLR_C             (1UL << 2)
#define SCTLR_I             (1UL << 12)

alignas(PAGE_SIZE) static uint64_t l1_table[512];
alignas(PAGE_SIZE) static uint64_t l2_table[512];
alignas(PAGE_SIZE) static uint64_t l3_table[512];
// 2MB のブロックを 4KB ページに分けるときのレベル 3 の表（カーネルの読み出し専用の範囲と、4 つのガードページ）
#define L3_POOL             12
alignas(PAGE_SIZE) static uint64_t l3_pool[L3_POOL][512];
static int l3_used;

#define ADDR_MASK           0x0000FFFFFFFFF000UL  // 記述子の出力アドレス（D8.3.2）

extern char __tbss_start[], __stack_top[], __stack_smp_base[];

#define STACK_SIZE          0x800000UL          // os/boot/linker.ld の .stack・.stack_smp

uint64_t mmu_stack_guard(int core)
{
    uint64_t bottom = core == 0 ? (uint64_t)__stack_top - STACK_SIZE
                                : (uint64_t)__stack_smp_base + (uint64_t)(core - 1) * STACK_SIZE;
    return (bottom + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

// addr（RAM）のレベル 3 の記述子。2MB のブロックなら、同じ属性の 4KB ページの表に分ける。分けられなければ NULL
static uint64_t *page_entry(uint64_t addr)
{
    uint64_t *l2 = &l2_table[addr / L2_BLOCK_SIZE];
    if (addr >= MMU_RAM_END || !(*l2 & DESC_VALID))
        return nullptr;
    if (!(*l2 & DESC_TABLE)) {
        if (l3_used == L3_POOL)
            return nullptr;
        uint64_t *t = l3_pool[l3_used++];
        uint64_t base = addr & ~(L2_BLOCK_SIZE - 1);
        for (uint64_t i = 0; i < 512; i++)
            t[i] = (base + i * PAGE_SIZE) | ATTR_NORMAL | DESC_PAGE | DESC_VALID;
        *l2 = (uint64_t)t | DESC_TABLE | DESC_VALID;
    }
    return &((uint64_t *)(*l2 & ADDR_MASK))[(addr / PAGE_SIZE) % 512];
}

// [start, end) のページを attr にする（0 なら無効）
static void map_pages(uint64_t start, uint64_t end, uint64_t attr)
{
    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        uint64_t *e = page_entry(a);
        if (e)
            *e = attr ? (a | attr | DESC_PAGE | DESC_VALID) : 0;
    }
}

void mmu_init(void)
{
    // レベル 3: 先頭 2MB（0 番地のページは無効のまま）
    for (uint64_t i = 1; i < 512; i++)
        l3_table[i] = (i * PAGE_SIZE) | ATTR_NORMAL | DESC_PAGE | DESC_VALID;

    // レベル 2: 最初の 1GB
    l2_table[0] = (uint64_t)l3_table | DESC_TABLE | DESC_VALID;
    for (uint64_t i = 1; i < 512; i++) {
        uint64_t addr = i * L2_BLOCK_SIZE;
        if (addr < MMU_RAM_END)
            l2_table[i] = addr | ATTR_NORMAL | DESC_BLOCK | DESC_VALID;
        else if (addr >= DEVICE_START)
            l2_table[i] = addr | ATTR_DEVICE | DESC_BLOCK | DESC_VALID;
    }

    // レベル 1
    l1_table[0] = (uint64_t)l2_table | DESC_TABLE | DESC_VALID;
    l1_table[1] = LOCAL_START | ATTR_DEVICE | DESC_BLOCK | DESC_VALID;

    // カーネルのコードと定数は読み出し専用（.tdata の初期値も読むだけ。.data が始まるページの手前まで）
    map_pages(0x80000, (uint64_t)__tbss_start & ~(PAGE_SIZE - 1), ATTR_NORMAL_RO);
    // 各コアのスタックのガードページ
    for (int core = 0; core < 4; core++)
        map_pages(mmu_stack_guard(core), mmu_stack_guard(core) + MMU_STACK_GUARD_SIZE, 0);

    // MMU 無効中の書き込みは直接メモリに届いているので、キャッシュ操作は不要
    dsb_sy();
    mmu_enable();
}

// このコアで MMU とキャッシュを有効にする（ページ表はコア 0 の mmu_init が作ったもの。
// コア 1〜3 は os/kernel/smp.c から呼ぶ）
void mmu_enable(void)
{
    write_sysreg(mair_el1, MAIR_VALUE);
    write_sysreg(tcr_el1, TCR_VALUE);
    write_sysreg(ttbr0_el1, l1_table);
    isb();
    __asm__ volatile("tlbi vmalle1\n\tdsb nsh\n\tic iallu\n\tdsb nsh" ::: "memory");
    isb();

    uint64_t sctlr = read_sysreg(sctlr_el1);
    write_sysreg(sctlr_el1, sctlr | SCTLR_M | SCTLR_C | SCTLR_I);
    isb();
}

// 0 番地のページ（NULL の参照を捕まえるため無効にしてある）を一時的に有効・無効にする。
// コア 1〜3 を起こすとき、スピンテーブル（0xD8〜0xF0）に書くために使う
void mmu_map_page0(bool on)
{
    l3_table[0] = on ? (0 | ATTR_NORMAL | DESC_PAGE | DESC_VALID) : 0;
    dsb_sy();
    __asm__ volatile("tlbi vmalle1\n\tdsb sy" ::: "memory");
    isb();
}
