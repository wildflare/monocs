// カーネルの記録（dmesg）。klog.h
#include "klog.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sysreg.h"
#include "../drivers/timer.h"

#define KLOG_SIZE   (64 * 1024)
#define CORES       4

// 記録の輪は、再起動（ウォッチドッグのリセット）をまたいで残る固定の番地に置く（2026-11-20）。カーネルより下の
// 0x60000〜0x70020（0x80000 のカーネルの手前。0 番地のページ（NULL を捕まえる）・ファームウェアのスピンテーブル
// （0xD8〜0xF0）とは重ならない）。次の起動の最初に、印と確かめの値が合えば前の起動の記録として写し
// （klog_previous。os/kernel/klog_file.c が /var/log/dmesg.1 にする）、新しく始める。固まってリセットされたとき、
// SD カードに書く前の最後の記録も読める。電源を切ると消える。書くたびにキャッシュから RAM へ書き出す（lock_give）
#define KLOG_PERSIST_ADDR   0x60000UL
#define KLOG_MAGIC          0x484942414e414c47ULL   // "MONOCSLG"
typedef struct {
    uint64_t magic;
    uint64_t count;         // これまでに書いたバイト数（輪の位置は count % KLOG_SIZE。下の written）
    uint64_t end;           // この起動の終わり方（KLOG_END_*）
    uint64_t check;         // magic ^ count ^ end ^ KLOG_CHECK（書きかけ・ごみを見分ける）
    char text[KLOG_SIZE];
} Persist;
#define KLOG_CHECK          0x9e3779b97f4a7c15ULL
#define KP                  ((Persist *)KLOG_PERSIST_ADDR)
#define ring                (KP->text)
#define written             (KP->count)
static_assert(KLOG_PERSIST_ADDR + sizeof(Persist) <= 0x80000, "記録の輪がカーネルに重なる");

static bool started;                // この起動で輪を始めた（BSS。最初の put_raw で persist_start）
static uint64_t cleaned;            // キャッシュから書き出した位置（written の値）
static char prev_text[KLOG_SIZE];   // 前の起動の記録（行の頭から）
static size_t prev_len;
static int prev_end = -1;           // 前の起動の終わり方（-1 = 残っていなかった）
static bool at_bol = true;          // 次の文字が行の頭（時刻を付ける）
static atomic_flag lock;
volatile bool klog_boot = true;
volatile int klog_tee_any;
static volatile int tee_depth[CORES];

// MMU（キャッシュ）が有効か。有効になる前は排他アクセス（ldaxr・stxr）が実機で成功しないことがあるので、
// ロックを取らない（そのときはコア 0 だけが動いている。os/drivers/uart.c の uart_putc と同じ考え）
static bool mmu_on(void)
{
    return read_sysreg(sctlr_el1) & 1;
}

static uint64_t lock_take(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    if (mmu_on())
        while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire))
            __asm__ volatile("yield");
    return daif;
}

static void persist_clean(void);

static void lock_give(uint64_t daif)
{
    persist_clean();
    if (mmu_on())
        atomic_flag_clear_explicit(&lock, memory_order_release);
    write_sysreg(daif, daif);
}

static void seal(void)
{
    KP->check = KP->magic ^ KP->count ^ KP->end ^ KLOG_CHECK;
}

// 前の起動の記録が残っていれば写してから、新しく始める
static void persist_start(void)
{
    started = true;
    if (KP->magic == KLOG_MAGIC && KP->check == (KP->magic ^ KP->count ^ KP->end ^ KLOG_CHECK) &&
        KP->end <= KLOG_END_HALT) {
        uint64_t w = KP->count, start = w > KLOG_SIZE ? w - KLOG_SIZE : 0;
        bool wrapped = start > 0;
        for (uint64_t i = start; i < w; i++) {
            char c = KP->text[i % KLOG_SIZE];
            if (wrapped) {      // 一周して頭が欠けた行は飛ばす
                if (c == '\n')
                    wrapped = false;
                continue;
            }
            prev_text[prev_len++] = c;
        }
        prev_end = (int)KP->end;
    }
    KP->magic = KLOG_MAGIC;
    KP->count = 0;
    KP->end = KLOG_END_RUNNING;
    seal();
    cleaned = 0;
}

static void put_raw(char c)
{
    if (!started)
        persist_start();
    ring[written % KLOG_SIZE] = c;
    written++;
}

// 書いた分をキャッシュから RAM へ（リセットで消えないように。MMU の前はキャッシュを使っていない）
static void clean_range(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + n;
    for (; a < e; a += 64)
        __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
}

static void persist_clean(void)
{
    if (!started || !mmu_on())
        return;
    seal();
    uint64_t from = cleaned, to = written;
    if (to - from >= KLOG_SIZE) {
        clean_range(KP->text, KLOG_SIZE);
    } else if (to > from) {
        size_t a = from % KLOG_SIZE, b = to % KLOG_SIZE;
        if (a < b) {
            clean_range(KP->text + a, b - a);
        } else {
            clean_range(KP->text + a, KLOG_SIZE - a);
            clean_range(KP->text, b);
        }
    }
    clean_range(KP, 32);
    __asm__ volatile("dsb sy" ::: "memory");
    cleaned = to;
}

void klog_set_end(int how)
{
    if (!started)
        persist_start();
    KP->end = (uint64_t)how;
    seal();
    if (mmu_on()) {
        clean_range(KP, 32);
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

void klog_persist_flush(void)
{
    persist_clean();
}

bool klog_previous(const char **text, size_t *len, int *how)
{
    if (prev_end < 0)
        return false;
    *text = prev_text;
    *len = prev_len;
    *how = prev_end;
    return true;
}

// 行の頭に "[   12.345678] "（起動からの秒。Linux の dmesg と同じ形）
static void put_stamp(void)
{
    uint64_t us = timer_now_us();
    uint64_t s = us / 1000000, frac = us % 1000000;
    char t[32];
    int n = 0;
    char d[24];
    int k = 0;
    do {
        d[k++] = (char)('0' + s % 10);
        s /= 10;
    } while (s);
    t[n++] = '[';
    for (int i = k; i < 5; i++)
        t[n++] = ' ';
    while (k)
        t[n++] = d[--k];
    t[n++] = '.';
    for (int i = 100000; i; i /= 10)
        t[n++] = (char)('0' + frac / (uint64_t)i % 10);
    t[n++] = ']';
    t[n++] = ' ';
    for (int i = 0; i < n; i++)
        put_raw(t[i]);
}

static void put_char(char c)
{
    if (c == '\r')
        return;
    if (at_bol && c != '\n')
        put_stamp();
    put_raw(c);
    at_bol = c == '\n';
}

void klog_console_char(char c, bool locked)
{
    if (!klog_boot && tee_depth[cpu_id()] == 0)
        return;
    if (!locked) {
        put_char(c);
        return;
    }
    uint64_t daif = lock_take();
    put_char(c);
    lock_give(daif);
}

void klog_boot_done(void)
{
    uint64_t daif = lock_take();
    if (!at_bol)
        put_char('\n');
    klog_boot = false;
    lock_give(daif);
}

void klog_tee_begin(void)
{
    tee_depth[cpu_id()]++;
    if (mmu_on())
        atomic_fetch_add_explicit((atomic_int *)&klog_tee_any, 1, memory_order_relaxed);
    else
        klog_tee_any++;
}

void klog_tee_end(void)
{
    if (tee_depth[cpu_id()] == 0)
        return;
    tee_depth[cpu_id()]--;
    if (mmu_on())
        atomic_fetch_sub_explicit((atomic_int *)&klog_tee_any, 1, memory_order_relaxed);
    else
        klog_tee_any--;
}

void klog_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof buf)
        n = (int)sizeof buf - 1;
    uint64_t daif = lock_take();
    if (!at_bol)
        put_char('\n');     // コンソールの表示の途中（起動中）なら、行を分ける
    for (int i = 0; i < n; i++)
        put_char(buf[i]);
    if (!at_bol)
        put_char('\n');
    lock_give(daif);
}

size_t klog_read(char *buf, size_t size)
{
    uint64_t daif = lock_take();
    uint64_t start = written > KLOG_SIZE ? written - KLOG_SIZE : 0;
    bool wrapped = start > 0;
    size_t n = 0;
    for (uint64_t i = start; i < written && n < size; i++) {
        char c = ring[i % KLOG_SIZE];
        if (wrapped) {      // 一周して頭が欠けた行は飛ばす
            if (c == '\n')
                wrapped = false;
            continue;
        }
        buf[n++] = c;
    }
    lock_give(daif);
    return n;
}

size_t klog_read_since(uint64_t *pos, char *buf, size_t size)
{
    uint64_t daif = lock_take();
    if (*pos > written)
        *pos = 0;               // dmesg -c で消された
    uint64_t start = written > KLOG_SIZE ? written - KLOG_SIZE : 0;
    if (*pos < start)
        *pos = start;           // 間に一周した: 残っている古いものから
    size_t n = 0;
    while (*pos < written && n < size)
        buf[n++] = ring[(*pos)++ % KLOG_SIZE];
    lock_give(daif);
    return n;
}

size_t klog_size(void)
{
    return written > KLOG_SIZE ? KLOG_SIZE : (size_t)written;
}

void klog_clear(void)
{
    uint64_t daif = lock_take();
    if (!started)
        persist_start();
    written = 0;
    cleaned = 0;
    at_bol = true;
    lock_give(daif);
}
