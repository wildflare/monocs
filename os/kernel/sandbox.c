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

#define BLOCK       0x200000UL      // 2MB（mmu.c の L2_BLOCK_SIZE）
#define REGION_SIZE (SANDBOX_IMAGE_SIZE + SANDBOX_DATA_SIZE)
#define SYS_EXIT    0
#define SYS_WRITE   1
#define ESR_EC(e)   (((e) >> 26) & 0x3F)
#define EC_SVC64    0x15

uint64_t sandbox_kernel_tp, sandbox_user_tp;    // vectors.S が使う（コア 0 の TPIDR_EL0 の、カーネルの値・EL0 の値）

static jmp_buf jmp;
static volatile bool active;
static enum sandbox_result result;
static int64_t exit_status;
static uint32_t intr_at_start;
static uint64_t deadline_us;
static uintptr_t region_lo, region_hi;     // EL0 が読み書きしてよい範囲（システムコールのポインタを確かめる）

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
        if (no == SYS_WRITE) {      // write(1 か 2, buf, len): buf はサンドボックスの中であること
            uint64_t fd = f->x[0], buf = f->x[1], len = f->x[2];
            if ((fd == 1 || fd == 2) && buf >= region_lo && len <= region_hi - buf && buf <= region_hi) {
                fwrite((const void *)buf, 1, len, fd == 1 ? stdout : stderr);
                f->x[0] = len;
            } else
                f->x[0] = (uint64_t)-1;
            return;                 // svc の次の命令から続ける（ELR はもう次を指している）
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
    if (uart_shell_interrupts() != intr_at_start) {
        printf("^C\n");
        leave(SANDBOX_INTERRUPTED);
    }
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
    if (setjmp(jmp) == 0) {
        active = true;
        sandbox_eret(entry, (uint64_t)data_end, (uint64_t)in);   // スタックはデータの領域の上端から
    }
    // ここへは leave の longjmp で戻る（例外の処理の中から。割り込みは止まっている）
    write_sysreg(tpidr_el0, sandbox_kernel_tp);
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
