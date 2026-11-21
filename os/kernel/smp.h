// マルチコア: コア 1〜3 で関数を動かす
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SMP_MAX_CORES 4

typedef void (*smp_fn)(void *arg);

// コア 1〜3 を起こす（コア 0 で、MMU・例外ベクタ・割り込みの準備の後に呼ぶ）
void smp_init(void);

// 使えるコアの数（コア 0 を含む）
int smp_cores(void);

// コア core（1〜3）で fn(arg) を始める。コアが使えないか、仕事中か、サービス用に取っておいたコアなら false
bool smp_run(int core, smp_fn fn, void *arg);
// スレッド（os/libc/pthread.c）として fn(arg) を始める。中の例外は、アプリ全体を止める（Linux と同じ）
bool smp_run_thread(int core, smp_fn fn, void *arg);
// このコアの仕事を、ここで終わらせる（スレッドの中の exit。止められた扱い）。コア 1〜3 から
[[noreturn]] void smp_exit_job(void);
// サービス用に取っておいたコアで fn(arg) を始める（os/kernel/svc.c）
bool smp_run_reserved(int core, smp_fn fn, void *arg);

// core をサービス用に取っておく（アプリには見せず、アプリの終わりでも止めない）
void smp_reserve(int core, bool reserved);
// core が仕事中か
bool smp_busy(int core);
// top（os/kernel/sysstat.c）: コア 1〜3 が眠っていた時間（cntpct のカウント）と様子
// （0 = 起きていない、1 = 待っている、2 = 仕事、3 = スレッド、4 = サービスのために取ってある）
void smp_add_idle(int core, uint64_t ticks);
uint64_t smp_idle_ticks(int core);
int smp_core_state(int core);

// コア core の仕事が終わるのを待つ。正常に終われば true（例外や中断で止まったら false）
bool smp_wait(int core);

// 仕事中のコアをすべて止めて待機に戻す（アプリが終わったときに app_call から呼ぶ）
// アプリが終わったとき: 入れ子の深さが min_level 以上のアプリが始めた仕事を止める（os/kernel/app.c）
void smp_abort_level(int min_level);

// 例外ハンドラから呼ぶ（コア 1〜3）。IRQ（メールボックス）と、仕事中の同期例外
void smp_handle_irq(uint64_t *elr);
void smp_fault(uint64_t *elr);

// 後回しにしていた中断があれば、ここで止める（コア 1〜3。lock.c が最後のロックを外したときに呼ぶ）
void smp_check_abort(void);
