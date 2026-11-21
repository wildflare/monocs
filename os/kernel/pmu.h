// CPU の性能カウンタ（PMU）で、キャッシュのミスなどを数える（シェルの perf。os/kernel/pmu.c）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PMU_MAX_EVENTS  6       // Cortex-A53 のイベントカウンタの数（PMCR_EL0.N）

typedef struct {
    const char *name;           // perf -e で使う名前
    uint16_t num;               // イベントの番号（ARMv8 の共通イベント）
    const char *desc;
} PmuEvent;

// 知っているイベントの一覧（count に数）
const PmuEvent *pmu_events(int *count);
const PmuEvent *pmu_find_event(const char *name);
bool pmu_event_supported(uint16_t num);    // PMCEID0/1_EL0 にあるか
int pmu_counters(void);                    // EL1 から使えるイベントカウンタの数

// 数え始める（events の数は pmu_counters 以下）。アプリが動かすほかのコアの仕事も数える
void pmu_begin(const uint16_t *events, int n);
// 止めて結果を返す: counts[i] は events[i]、cycles はコア 0 のサイクル数。per_core[c] はコアごと
// （[0..n-1] がイベント、[n] がサイクル）。app_only は、アプリの実行中だけを数えたか
typedef struct {
    int n;
    uint64_t counts[PMU_MAX_EVENTS];
    uint64_t cycles;
    uint64_t per_core[4][PMU_MAX_EVENTS + 1];
    bool used_core[4];
    bool app_only;
    uint64_t elapsed_us;
} PmuResult;
void pmu_end(PmuResult *r);

// os/slimcc/run.c: アプリの実行の前後（数えている間なら、コンパイルの分を捨て、アプリの終わりで止める）
void pmu_app_begin(void);
void pmu_app_end(void);

// os/kernel/smp.c: コア 1〜3 の仕事の前後
void pmu_job_begin(void);
void pmu_job_end(void);

// サンプリング（perf record）: event が every 回起きるごとに、そのとき動いていた番地を記録する
#define PMU_MAX_SAMPLES 65536
#define PMU_CALLS       4       // 呼び出し元を何段まで記録するか（perf record -g）
typedef struct {
    uint64_t pc;
    uint64_t calls[PMU_CALLS];  // 呼び出し元（[0] は x30、続きはフレームポインタをたどったもの。0 = なし）
    uint8_t core;
    const char *name;   // アプリの関数名（アプリが終わる前に os/slimcc/run.c が付ける。カーネルのものは NULL）
    const char *call_names[PMU_CALLS];  // 同じく、呼び出し元
    uint64_t off;
} PmuSample;
void pmu_record_begin(uint16_t event, uint32_t every);  // 終わりは pmu_end
bool pmu_recording(void);
const PmuSample *pmu_samples(int *n, int *lost);
PmuSample *pmu_samples_mut(int *n);

// 割り込み（カウンタのあふれ。ARM ローカル周辺の要因 9）
// 割り込みで止まった番地と、そのときの x29（フレームポインタ）・x30（pmu_handle_irq の前に）
void pmu_set_irq_frame(uint64_t elr, uint64_t fp, uint64_t lr);
void pmu_handle_irq(void);
void pmu_init(void);
