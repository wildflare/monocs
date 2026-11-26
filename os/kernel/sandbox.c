// サンドボックス（2026-11-26。docs/sandbox.md の 3.1・8 の 2）: 信用できないプログラムを EL0 で動かす。
// 決めた形の領域（プログラム 4MB・データとスタック 8MB）を EL0 から使えるようにし、そこで動かして、終わる・不正な
// アクセス・Ctrl-C・時間の上限で、シェルに戻ってくる。システムコール（svc #0、番号は x8。lib/sandbox/sandbox_sys.h）:
// 0 終わる、1 画面に書く。C のソースは os/slimcc/sandbox_cmd.c が、サンドボックスの中の C ライブラリ（/usr/lib/sandbox）
// と一緒にこの領域の中でつなぐ（OS の関数にはつながない）。
//
// 守り（MMU。恒等マップのまま、ページの許可だけを変える。os/kernel/mmu.c）:
//   OS のメモリと周辺機器は EL0 から読み書きも実行もできない（AP 00・10 と UXN）。サンドボックスのコードの領域は
//   EL0 から実行と読み出しだけ（AP 11）、データの領域は読み書きだけ（AP 01、UXN）。
// 例外（os/kernel/vectors.S の ventry_lower）: EL0 から来たら SP_EL0 と EL0 の TPIDR_EL0 を退避し、カーネルの
//   TPIDR_EL0（コアごとの TLS）に戻してから handle_exception。戻るときに逆をする。サンドボックスはコア 0 だけで動く。
// 出るとき: 例外の処理の中から longjmp で sandbox_run に戻る（例外のフレームは捨てる）。
#define _GNU_SOURCE         // clock_gettime・CLOCK_MONOTONIC・getentropy（カーネルは ISO C23 なので、宣言を見せる。posix.c と同じ）
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sandbox.h"
#include "mmu.h"
#include "sysreg.h"
#include "app.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "term.h"
#include <time.h>
#include <unistd.h>

#define BLOCK       0x200000UL      // 2MB（mmu.c の L2_BLOCK_SIZE）
#define REGION_SIZE (SANDBOX_IMAGE_SIZE + SANDBOX_DATA_SIZE)
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_READ    2       // read(0, buf, len, timeout_ms): 生のバイト（打った文字の表示・行の編集はライブラリ）。時間切れは 0
#define SYS_TERMSIZE 3      // → 行 << 32 | 桁
#define SYS_CLOCK   4       // clock(id): ナノ秒（0 = 実時間、1 = 起動から）
#define SYS_SLEEP   5       // sleep(ns)
#define SYS_RANDOM  6       // random(buf, len ≤ 256): 0 か -1
#define SYS_GO      7       // go(url, len, back): ページを終えて、ほかのページへ移る（back が 0 以外なら戻る。docs/cweb.md）
#define ESR_EC(e)   (((e) >> 26) & 0x3F)
#define EC_SVC64    0x15

uint64_t sandbox_kernel_tp, sandbox_user_tp;    // vectors.S が使う（コア 0 の TPIDR_EL0 の、カーネルの値・EL0 の値）

static jmp_buf jmp;
static volatile bool active;
static enum sandbox_result result;
static int64_t exit_status;
static uint32_t intr_at_start;
static uint64_t deadline_us;
static uintptr_t region_lo, region_hi;
static bool alt_screen;
static char nav_url[512];   // SYS_GO の移る先
static bool nav_back;

const char *sandbox_nav(bool *back)
{
    if (back)
        *back = nav_back;
    return nav_url;
}     // プログラムが代わりの画面（curses）に切り替えたまま（終わったときに戻す）     // EL0 が読み書きしてよい範囲（システムコールのポインタを確かめる）

void sandbox_eret(uint64_t entry, uint64_t sp, uint64_t arg);   // vectors.S（戻ってこない）
void __clear_cache(void *begin, void *end);         // 命令キャッシュとデータキャッシュの整合（cc/rt/clear_cache.c）

static const char *fault_name(uint32_t ec)
{
    switch (ec) {
    case 0x20: return "instruction abort (not allowed to run there)";
    case 0x24: return "data abort (not allowed to read or write there)";
    case 0x22: return "PC alignment";
    case 0x26: return "SP alignment";
    case 0x18: return "system register access";
    case 0x00: return "undefined instruction";
    default:   return "exception";
    }
}

[[noreturn]] static void leave(enum sandbox_result r)
{
    result = r;
    active = false;
    longjmp(jmp, 1);
}

// 待つシステムコール（読む・眠る・画面の大きさ）の間は割り込みを受ける（例外の処理の中は止まっている）。
// 待ちながら Ctrl-C・時間の上限を調べる（sandbox_after_irq と同じ）
static void irq_on(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static void irq_off(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

static void check_stop(void)
{
    if (uart_shell_interrupts() != intr_at_start) {
        irq_off();
        leave(SANDBOX_INTERRUPTED);     // ^C はシェルが表示する
    }
    if (deadline_us && timer_now_us() >= deadline_us) {
        irq_off();
        leave(SANDBOX_TIMEDOUT);
    }
}

static bool in_region(uint64_t p, uint64_t len)
{
    return p >= region_lo && p <= region_hi && len <= region_hi - p;
}

// キーを読む: 最大 timeout_ms（負ならいつまでも）待ち、来た分だけ返す（10ms ごとに止めるかを調べる）
static uint64_t sys_read(uint8_t *buf, uint64_t len, int64_t timeout_ms)
{
    uint64_t end = timeout_ms < 0 ? 0 : timer_now_us() + (uint64_t)timeout_ms * 1000;
    irq_on();
    for (;;) {
        check_stop();
        size_t n = uart_read(buf, len, 10000);
        // Ctrl-C は止める合図（キーとしては渡さない）。合図の数は文字と同じときに増えるので、読んだ後にも調べる
        check_stop();
        if (memchr(buf, 3, n)) {
            irq_off();
            leave(SANDBOX_INTERRUPTED);
        }
        if (n || (timeout_ms >= 0 && timer_now_us() >= end)) {
            irq_off();
            return n;
        }
    }
}

static void sys_sleep(uint64_t ns)
{
    uint64_t end = timer_now_us() + ns / 1000;
    irq_on();
    while (timer_now_us() < end) {
        check_stop();
        uint64_t left = end - timer_now_us();
        timer_delay_us(left > 10000 ? 10000 : left);
    }
    irq_off();
}

void sandbox_handle_sync(struct trap_frame *f)
{
    uint32_t ec = ESR_EC(f->esr);
    if (!active) {      // 来ないはず（EL0 にいるのはサンドボックスだけ）
        printf("\n*** exception from EL0 outside the sandbox (ESR %#llx)\n", (unsigned long long)f->esr);
        f->elr += 4;
        return;
    }
    if (ec == EC_SVC64 && (f->esr & 0xFFFF) == 0) {
        uint64_t no = f->x[8];
        if (no == SYS_EXIT) {
            exit_status = (int64_t)f->x[0];
            leave(SANDBOX_EXITED);
        }
        uint64_t a0 = f->x[0], a1 = f->x[1], a2 = f->x[2], a3 = f->x[3];
        f->x[0] = (uint64_t)-1;     // 誤りの値（うまくいったものが書き直す）。svc の次の命令から続ける
        switch (no) {
        case SYS_WRITE:             // write(1 か 2, buf, len)
            if ((a0 == 1 || a0 == 2) && in_region(a1, a2)) {
                // 代わりの画面への切り替え・戻し（curses の initscr・endwin）を見ておく
                const char *last_on = nullptr, *last_off = nullptr;
                const char *end = (const char *)a1 + a2;
                for (const char *p = (const char *)a1; (p = memmem(p, (size_t)(end - p), "\x1b[?1049", 7)); p++)
                    if (p + 7 < end && p[7] == 'h')
                        last_on = p;
                    else if (p + 7 < end && p[7] == 'l')
                        last_off = p;
                if (last_on || last_off)
                    alt_screen = last_on > last_off;
                irq_on();
                fwrite((const void *)a1, 1, a2, a0 == 1 ? stdout : stderr);
                fflush(a0 == 1 ? stdout : stderr);
                irq_off();
                f->x[0] = a2;
            }
            return;
        case SYS_READ:              // read(0, buf, len, timeout_ms)
            if (a0 == 0 && a2 && in_region(a1, a2))
                f->x[0] = sys_read((uint8_t *)a1, a2, (int64_t)a3);
            return;
        case SYS_TERMSIZE: {
            int rows = 24, cols = 80;
            irq_on();
            term_size(&rows, &cols);
            irq_off();
            f->x[0] = (uint64_t)rows << 32 | (uint32_t)cols;
            return;
        }
        case SYS_CLOCK: {
            struct timespec ts;
            if (clock_gettime(a0 == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts) == 0)
                f->x[0] = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            return;
        }
        case SYS_SLEEP:
            sys_sleep(a0);
            f->x[0] = 0;
            return;
        case SYS_GO:                // go(url, len, back): ページを終えて移る
            nav_back = a2 != 0;
            nav_url[0] = 0;
            if (!nav_back && (a1 == 0 || a1 >= sizeof nav_url || !in_region(a0, a1)))
                return;             // 長すぎる・おかしい URL は誤り（-1）を返して続ける
            if (!nav_back) {
                memcpy(nav_url, (const void *)a0, a1);
                nav_url[a1] = 0;
            }
            leave(SANDBOX_NAVIGATE);
        case SYS_RANDOM:
            if (a1 <= 256 && in_region(a0, a1))
                f->x[0] = getentropy((void *)a0, a1) == 0 ? 0 : (uint64_t)-1;
            return;
        default:
            break;
        }
        printf("sandbox: stopped: unknown system call %llu at %#llx\n", (unsigned long long)no,
               (unsigned long long)f->elr);
        leave(SANDBOX_FAULTED);
    }
    printf("sandbox: stopped: %s at %#llx (address %#llx, ESR %#llx)\n", fault_name(ec),
           (unsigned long long)f->elr, (unsigned long long)f->far, (unsigned long long)f->esr);
    leave(SANDBOX_FAULTED);
}

// EL0 で動いている間の割り込み（コア 0）の後: Ctrl-C と時間の上限
void sandbox_after_irq(void)
{
    if (!active)
        return;
    if (uart_shell_interrupts() != intr_at_start)
        leave(SANDBOX_INTERRUPTED);     // ^C はシェルが表示する
    if (deadline_us && timer_now_us() >= deadline_us)
        leave(SANDBOX_TIMEDOUT);
}

uint8_t *sandbox_region_alloc(void)
{
    uint8_t *r = aligned_alloc(BLOCK, REGION_SIZE);
    if (r)
        memset(r, 0, REGION_SIZE);
    return r;
}

void sandbox_region_free(uint8_t *region)
{
    free(region);
}

enum sandbox_result sandbox_enter(uint8_t *region, uint64_t entry, bool writable_image, int argc, char **argv,
                                  uint32_t timeout_ms, int64_t *status)
{
    if (cpu_id() != 0 || active || app_is_running() || !region)
        return SANDBOX_NOT_STARTED;
    // データの領域の頭: struct sandbox_info（lib/sandbox/sandbox_sys.h と同じ形）、argv、引数の文字列
    struct info { uint64_t heap_start, heap_end; int argc; char **argv; };
    uint8_t *data = region + SANDBOX_IMAGE_SIZE;
    struct info *in = (struct info *)data;
    char **av = (char **)(data + sizeof *in);
    char *str = (char *)(av + argc + 1);
    char *data_end = (char *)data + SANDBOX_DATA_SIZE;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        if (str + n > (char *)data + BLOCK)
            return SANDBOX_NOT_STARTED;         // 引数が長すぎる（データの最初の 2MB に収める）
        memcpy(str, argv[i], n);
        av[i] = str;
        str += n;
    }
    av[argc] = nullptr;
    in->argc = argc;
    in->argv = av;
    in->heap_start = ((uint64_t)str + 15) & ~15ULL;
    in->heap_end = (uint64_t)data_end - (1UL << 20);   // 上の 1MB はスタック
    if (!mmu_sandbox_map((uint64_t)region, SANDBOX_IMAGE_SIZE, writable_image ? MMU_SANDBOX_IMAGE : MMU_SANDBOX_CODE) ||
        !mmu_sandbox_map((uint64_t)data, SANDBOX_DATA_SIZE, MMU_SANDBOX_DATA)) {
        mmu_sandbox_map((uint64_t)region, REGION_SIZE, MMU_SANDBOX_NORMAL);
        return SANDBOX_NOT_STARTED;
    }
    region_lo = (uintptr_t)region;
    region_hi = (uintptr_t)region + REGION_SIZE;
    uint64_t daif = read_sysreg(daif);
    sandbox_kernel_tp = read_sysreg(tpidr_el0);
    sandbox_user_tp = 0;
    intr_at_start = uart_shell_interrupts();
    deadline_us = timeout_ms ? timer_now_us() + (uint64_t)timeout_ms * 1000 : 0;
    exit_status = 0;
    alt_screen = false;
    if (setjmp(jmp) == 0) {
        active = true;
        sandbox_eret(entry, (uint64_t)data_end, (uint64_t)in);   // スタックはデータの領域の上端から
    }
    // ここへは leave の longjmp で戻る（例外の処理の中から。割り込みは止まっている）
    write_sysreg(tpidr_el0, sandbox_kernel_tp);
    if (alt_screen) {           // curses の画面のまま終わった（止められた）: 端末を戻す（os/curses/curses.c と同じ）
        fputs("\x1b[0m\x1b[?25h\x1b[?2004l\x1b[>4;0m\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l"
              "\x1b[?1l\x1b>\x1b[?1049l", stdout);
        alt_screen = false;
    }
    if (result == SANDBOX_INTERRUPTED) {    // 残っている入力（Ctrl-C の文字も）を捨てる（Linux の ISIG と同じ。NOFLSH なし）
        char junk[64];
        while (uart_read(junk, sizeof junk, 0))
            ;
    }
    fflush(stdout);
    mmu_sandbox_map((uint64_t)region, REGION_SIZE, MMU_SANDBOX_NORMAL);
    write_sysreg(daif, daif);
    if (status)
        *status = exit_status;
    return result;
}

enum sandbox_result sandbox_run(const uint32_t *code, size_t ncode, uint32_t timeout_ms, int64_t *status)
{
    if (ncode * 4 > SANDBOX_IMAGE_SIZE)
        return SANDBOX_NOT_STARTED;
    uint8_t *region = sandbox_region_alloc();
    if (!region)
        return SANDBOX_NOT_STARTED;
    memcpy(region, code, ncode * 4);
    __clear_cache(region, region + ncode * 4);
    char *argv[] = { "sbtest", nullptr };
    enum sandbox_result r = sandbox_enter(region, (uint64_t)region, false, 1, argv, timeout_ms, status);
    sandbox_region_free(region);
    return r;
}
