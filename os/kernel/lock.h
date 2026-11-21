// コア間のロック（再帰できるスピンロック）
//
// picolibc（__retarget_lock_*）と VFS が使う。ロックを持っている間は、そのコアの中断
// （コア 0 の Ctrl-C、コア 1〜3 の smp_abort_level）を後回しにする。ロックを持ったまま止まると、
// ほかのコアが待ち続けたり、ヒープなどが壊れたままになったりするため。
// 同期例外でロックを持ったまま止まったときは、lock_release_all でそのコアのロックを外す。
#pragma once

#include <stdatomic.h>
#include <stdbool.h>

// キャッシュラインの大きさ（Cortex-A53。ARM Cortex-A53 TRM の L1・L2 キャッシュ）
#define CACHE_LINE 64

// ロック 1 つで、キャッシュライン 1 本を占める（偽の共有を避ける: 別々のコアが別々のロックを使うとき、
// 同じラインに並んでいると、取るたびにラインを奪い合う。2026-11-14 に実機で測った。os/kernel/lock.c）
struct klock {
    alignas(CACHE_LINE) atomic_uint word;   // 0: 空き、1: 使用中
    volatile int owner; // 持っているコア（空きなら -1）
    int count;          // 同じコアが重ねて取った回数
};

#define KLOCK_INIT { 0, -1, 0 }

void klock_acquire(struct klock *l);
void klock_release(struct klock *l);

// アプリ用（monocs_mutex_lock・monocs_mutex_unlock）。中断は後回しにしない
void app_mutex_lock(struct klock *l);
void app_mutex_unlock(struct klock *l);

// ロックを持たずに、この間のこのコアの中断（Ctrl-C、smp_abort_level）を後回しにする（重ねてよい）。
// 途中で止めると状態が壊れるもの（アプリから呼んだコンパイラ。os/slimcc/cmd.c）に使う
void abort_defer_begin(void);
void abort_defer_end(void);

// このコアが持っているロックの数（重ねて取った分も数える）。0 でなければ中断を待たせる
int lock_depth(int core);

// コア core が持っているロックをすべて外す（同期例外・中断の後始末）
void lock_release_all(int core);

// picolibc 用のロック（__retarget_lock_init）を使い切ったとき。テスト用
