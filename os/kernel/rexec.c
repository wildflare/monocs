// サービスから頼まれたシェルの 1 行を動かす（ssh の exec: `ssh monocs 'ls /'`）。
// サービス（コア 3）が rexec_start で行を頼み、シェル（コア 0）がプロンプトで入力を待っている間に取り出して動かす
// （os/shell/shell.c の shell_rexec_poll。行の編集の途中でも、画面は変えない）。行の標準出力・標準エラーは
// 出力の輪（64KB）に、標準入力は入力の輪（16KB）から（リダイレクトの仕組み。os/libc/syscalls.c）。
// 輪はどちらも、書く側 1 つ・読む側 1 つ（出力: コア 0 のアプリ（ほかのコアの printf は syscalls.c の out_lock で
// 1 つずつ）→ サービス、入力: サービス → コア 0）。
// 出力の輪がいっぱいなら、サービスが読むまで待つ。サービスが 10 秒読まなければ（相手が止まった・サービスが落ちた）、
// 捨てる。サービスがやめたら（rexec_release）、動いているものを Ctrl-C と同じく止め、出力を捨て、入力を終わりにする。

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "rexec.h"
#include "lock.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"

#define CMD_MAX     8192
#define OUT_SIZE    (64 * 1024)
#define IN_SIZE     (16 * 1024)
#define STALL_US    10000000

enum { IDLE, QUEUED, RUNNING, DONE };

static atomic_int state = IDLE;
static atomic_bool cancelled;
static atomic_int exit_status;
static char cmd_buf[CMD_MAX];
static uint8_t out_ring[OUT_SIZE], in_ring[IN_SIZE];
static atomic_uint out_head, out_tail, in_head, in_tail;
static atomic_bool in_eof;
static _Atomic uint64_t last_read_us;   // サービスが最後に出力を読んだ時刻

int rexec_start(const char *cmd)
{
    size_t n = strlen(cmd);
    if (n >= CMD_MAX)
        return -1;
    int expect = IDLE;
    // 先に IDLE から抜けてから中身を用意する（QUEUED にするのは用意の後）
    if (!atomic_compare_exchange_strong(&state, &expect, -1))
        return -1;
    memcpy(cmd_buf, cmd, n + 1);
    atomic_store(&out_head, 0);
    atomic_store(&out_tail, 0);
    atomic_store(&in_head, 0);
    atomic_store(&in_tail, 0);
    atomic_store(&in_eof, false);
    atomic_store(&cancelled, false);
    atomic_store(&last_read_us, timer_now_us());
    atomic_store(&state, QUEUED);
    return 0;
}

int rexec_read(void *buf, int n)
{
    atomic_store(&last_read_us, timer_now_us());
    unsigned t = atomic_load_explicit(&out_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&out_head, memory_order_acquire);
    int k = 0;
    uint8_t *p = buf;
    while (k < n && t != h)
        p[k++] = out_ring[t++ % OUT_SIZE];
    atomic_store_explicit(&out_tail, t, memory_order_release);
    return k;
}

int rexec_write(const void *p, int n)
{
    unsigned h = atomic_load_explicit(&in_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&in_tail, memory_order_acquire);
    int k = 0;
    const uint8_t *s = p;
    while (k < n && h - t < IN_SIZE)
        in_ring[h++ % IN_SIZE] = s[k++];
    atomic_store_explicit(&in_head, h, memory_order_release);
    return k;
}

void rexec_write_eof(void)
{
    atomic_store(&in_eof, true);
}

int rexec_status(void)
{
    atomic_store(&last_read_us, timer_now_us());
    return atomic_load(&state) == DONE ? atomic_load(&exit_status) : -1;
}

void rexec_release(void)
{
    int s = atomic_load(&state);
    if (s == QUEUED) {
        int expect = QUEUED;
        if (atomic_compare_exchange_strong(&state, &expect, IDLE))
            return;
        s = atomic_load(&state);    // ちょうどシェルが取り出した
    }
    if (s == RUNNING) {
        atomic_store(&cancelled, true);
        atomic_store(&in_eof, true);
        uart_raise_interrupt();     // Ctrl-\ と同じ（アプリなら止める、組み込みのコマンドの繰り返しなら止める）
        return;                     // シェルが終わったら IDLE に戻す（rexec_done）
    }
    if (s == DONE)
        atomic_store(&state, IDLE);
}

bool rexec_queued(void)
{
    return atomic_load(&state) == QUEUED;
}

const char *rexec_take(void)
{
    int expect = QUEUED;
    if (!atomic_compare_exchange_strong(&state, &expect, RUNNING))
        return nullptr;
    return cmd_buf;
}

void rexec_done(int status)
{
    atomic_store(&exit_status, status & 0xFF);
    atomic_store(&state, atomic_load(&cancelled) ? IDLE : DONE);
}

bool rexec_running(void)
{
    return atomic_load(&state) == RUNNING;
}

void rexec_putc(char c)
{
    if (atomic_load(&cancelled))
        return;
    unsigned h = atomic_load_explicit(&out_head, memory_order_relaxed);
    while (h - atomic_load_explicit(&out_tail, memory_order_acquire) >= OUT_SIZE) {
        if (atomic_load(&cancelled) || timer_now_us() - atomic_load(&last_read_us) > STALL_US)
            return;             // 読まれない: 捨てる
        cpu_idle();             // 次の割り込みまで（ネットワークの受信はこの間に進む）
    }
    out_ring[h % OUT_SIZE] = (uint8_t)c;
    atomic_store_explicit(&out_head, h + 1, memory_order_release);
}

// lock を持って 1 文字入れる。輪がいっぱいなら、lock を外して空くのを待つ（待つ間にアプリが Ctrl-C・timeout で
// 止められても、lock を持ったまま残らない。2026-11-19）。読まれないまま STALL_US たったら捨てる
void rexec_putc_locked(char c, struct klock *lock)
{
    for (;;) {
        klock_acquire(lock);
        if (atomic_load(&cancelled)) {
            klock_release(lock);
            return;
        }
        unsigned h = atomic_load_explicit(&out_head, memory_order_relaxed);
        if (h - atomic_load_explicit(&out_tail, memory_order_acquire) < OUT_SIZE) {
            out_ring[h % OUT_SIZE] = (uint8_t)c;
            atomic_store_explicit(&out_head, h + 1, memory_order_release);
            klock_release(lock);
            return;
        }
        klock_release(lock);
        if (timer_now_us() - atomic_load(&last_read_us) > STALL_US)
            return;             // 読まれない: 捨てる
        cpu_idle();             // 次の割り込みまで（ネットワークの受信はこの間に進む）
    }
}

void rexec_drain(int ms)
{
    uint64_t end = timer_now_us() + (uint64_t)ms * 1000;
    while (atomic_load(&state) == RUNNING && atomic_load(&out_tail) != atomic_load(&out_head) && timer_now_us() < end)
        cpu_idle();
}

int rexec_read_in(void *buf, int n)
{
    unsigned t = atomic_load_explicit(&in_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&in_head, memory_order_acquire);
    int k = 0;
    uint8_t *p = buf;
    while (k < n && t != h)
        p[k++] = in_ring[t++ % IN_SIZE];
    atomic_store_explicit(&in_tail, t, memory_order_release);
    return k;
}

int rexec_getc(void)
{
    for (;;) {
        unsigned t = atomic_load_explicit(&in_tail, memory_order_relaxed);
        if (t != atomic_load_explicit(&in_head, memory_order_acquire)) {
            int c = in_ring[t % IN_SIZE];
            atomic_store_explicit(&in_tail, t + 1, memory_order_release);
            return c;
        }
        if (atomic_load(&in_eof) || atomic_load(&cancelled))
            return -1;
        cpu_idle();
    }
}
