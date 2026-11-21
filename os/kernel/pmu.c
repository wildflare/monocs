// CPU の性能カウンタ（PMU）: キャッシュのミスなどを数える（シェルの perf）
// 出典: ARM DDI 0487 D7（PMU。PMCR_EL0、PMCNTENSET_EL0、PMEVTYPER<n>_EL0、PMOVSCLR_EL0 など）、
//       D7.10（共通のイベントの番号）、Cortex-A53 TRM 12 章（6 個のイベントカウンタ）
//
// 数えるのは、コア 0 と、数えている間に始まったコア 1〜3 の仕事（アプリの monocs_core_start など）。
// コア 3 のサービスは数えない（数え始める前から動いているので）。
// イベントカウンタは 32 ビットなので、1GHz では数秒であふれる。あふれたら PMU の割り込み（ARM ローカル周辺の
// 要因 9。pmu_init でそのコアの IRQ に配る）で上位を数える。サイクルカウンタは 64 ビット（PMCR_EL0.LC）。
// OS とアプリは同じ EL1 で動くので、カーネル（割り込みなど）の分も入る。wfi で眠っている間は数えない。

#include <stdatomic.h>
#include <string.h>
#include "pmu.h"
#include "smp.h"
#include "mmu.h"
#include "sysreg.h"
#include "../drivers/bcm2837.h"
#include "../drivers/mmio.h"
#include "../drivers/timer.h"

#define PMCR_E          (1UL << 0)  // 数える
#define PMCR_P          (1UL << 1)  // イベントカウンタを 0 に
#define PMCR_C          (1UL << 2)  // サイクルカウンタを 0 に
#define PMCR_LC         (1UL << 6)  // サイクルカウンタを 64 ビットで
#define PMCR_N_SHIFT    11
#define CYCLE_BIT       (1UL << 31)

static const PmuEvent events[] = {
    {"cycles",    0x11, "CPU cycles (CPU_CYCLES. perf record -e cycles: where the time goes)"},
    {"inst",      0x08, "instructions retired (INST_RETIRED)"},
    {"l1d",       0x04, "L1 data cache accesses (L1D_CACHE)"},
    {"l1d-miss",  0x03, "L1 data cache refills (L1D_CACHE_REFILL)"},
    {"l1i",       0x14, "L1 instruction cache accesses (L1I_CACHE)"},
    {"l1i-miss",  0x01, "L1 instruction cache refills (L1I_CACHE_REFILL)"},
    {"l2",        0x16, "L2 cache accesses (L2D_CACHE)"},
    {"l2-miss",   0x17, "L2 cache refills (L2D_CACHE_REFILL)"},
    {"l1d-wb",    0x15, "L1 data cache write-backs (L1D_CACHE_WB)"},
    {"l2-wb",     0x18, "L2 cache write-backs (L2D_CACHE_WB)"},
    {"dtlb-miss", 0x05, "L1 data TLB refills (L1D_TLB_REFILL)"},
    {"itlb-miss", 0x02, "L1 instruction TLB refills (L1I_TLB_REFILL)"},
    {"br",        0x12, "branches predicted (BR_PRED)"},
    {"br-miss",   0x10, "branch mispredictions (BR_MIS_PRED)"},
    {"mem",       0x13, "data memory accesses (MEM_ACCESS)"},
    {"bus",       0x19, "bus accesses (BUS_ACCESS)"},
    {"exc",       0x09, "exceptions taken (EXC_TAKEN)"},
};

#define NEVENTS ((int)(sizeof(events) / sizeof(events[0])))

static int nev;
static uint16_t evs[PMU_MAX_EVENTS];
static volatile bool counting;      // perf の測っている間
static volatile bool app_mode;      // アプリの実行中だけを数える段階に入った
static volatile bool frozen;        // アプリが終わって止めた（後のシェルの分は数えない）
static uint64_t acc[SMP_MAX_CORES][PMU_MAX_EVENTS + 1];  // コアごと（最後がサイクル）
static volatile bool used[SMP_MAX_CORES];
static volatile bool job_counting[SMP_MAX_CORES];
static uint64_t t_start, t_end;

// サンプリング（perf record）: イベント 0 が period 回起きるごとにあふれさせ、割り込みで止まっていた番地を記録する
static volatile bool sampling;
static uint32_t period;
static PmuSample samples[PMU_MAX_SAMPLES];
static atomic_int nsamples;
static atomic_int dropped;
static uint64_t irq_elr[SMP_MAX_CORES];     // 割り込みで止まった番地（exception.c が設定する）
static uint64_t irq_fp[SMP_MAX_CORES], irq_lr[SMP_MAX_CORES];

const PmuEvent *pmu_events(int *count)
{
    *count = NEVENTS;
    return events;
}

const PmuEvent *pmu_find_event(const char *name)
{
    for (int i = 0; i < NEVENTS; i++)
        if (!strcmp(events[i].name, name))
            return &events[i];
    return nullptr;
}

bool pmu_event_supported(uint16_t num)
{
    if (num >= 64)
        return false;
    uint64_t id = num < 32 ? read_sysreg(pmceid0_el0) : read_sysreg(pmceid1_el0);
    return (id >> (num % 32)) & 1;
}

int pmu_counters(void)
{
    int n = (int)((read_sysreg(pmcr_el0) >> PMCR_N_SHIFT) & 31);
    return n < PMU_MAX_EVENTS ? n : PMU_MAX_EVENTS;
}

// このコアで数え始める（カウンタを 0 にする）
static void hw_start(void)
{
    uint64_t mask = (1UL << nev) - 1;
    write_sysreg(pmcntenclr_el0, 0xFFFFFFFFUL);
    write_sysreg(pmintenclr_el1, 0xFFFFFFFFUL);
    write_sysreg(pmovsclr_el0, 0xFFFFFFFFUL);
    for (int i = 0; i < nev; i++) {
        write_sysreg(pmselr_el0, (uint64_t)i);
        isb();
        write_sysreg(pmxevtyper_el0, (uint64_t)evs[i]);  // フィルタのビットは 0（EL1 も数える）
    }
    write_sysreg(pmccfiltr_el0, 0UL);
    write_sysreg(pmcr_el0, PMCR_E | PMCR_P | PMCR_C | PMCR_LC);
    isb();
    if (sampling) {
        write_sysreg(pmselr_el0, 0UL);
        isb();
        write_sysreg(pmxevcntr_el0, (uint64_t)(uint32_t)(0x100000000ULL - period));
    }
    write_sysreg(pmintenset_el1, mask);
    write_sysreg(pmcntenset_el0, mask | CYCLE_BIT);
    isb();
}

// このコアで止め、数えた分を acc[core] に足す（割り込みを止めて呼ぶ）
static void hw_stop_add(int core)
{
    write_sysreg(pmcntenclr_el0, 0xFFFFFFFFUL);
    isb();
    uint64_t ovs = read_sysreg(pmovsclr_el0);
    write_sysreg(pmovsclr_el0, ovs);
    write_sysreg(pmintenclr_el1, 0xFFFFFFFFUL);
    for (int i = 0; i < nev; i++) {
        write_sysreg(pmselr_el0, (uint64_t)i);
        isb();
        uint64_t v = read_sysreg(pmxevcntr_el0) & 0xFFFFFFFFUL;
        if (sampling && i == 0) {
            // 0x100000000 - period から数えている。あふれた後なら、その period も
            v = (ovs & 1) ? v + period : (uint32_t)(v - (uint32_t)(0x100000000ULL - period));
        } else if ((ovs >> i) & 1) {
            v += 1UL << 32;     // あふれた後、割り込みを受ける前に止めた
        }
        acc[core][i] += v;
    }
    acc[core][nev] += read_sysreg(pmccntr_el0);
}

static uint64_t irq_save(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    return daif;
}

static void irq_restore(uint64_t daif)
{
    write_sysreg(daif, daif);
}

void pmu_init(void)
{
    // 各コアの PMU の割り込みを、そのコアの IRQ に配る
    mmio_write32(LOCAL_PM_ROUTING_SET, 0xF);
}

void pmu_set_irq_frame(uint64_t elr, uint64_t fp, uint64_t lr)
{
    int core = cpu_id();
    irq_elr[core] = elr;
    irq_fp[core] = fp;
    irq_lr[core] = lr;
}

// フレームポインタをたどって呼び出し元を集める（{前の x29, 戻り先} の組。AAPCS64 のフレームレコード）。
// 割り込みの中なので、このコアのスタックの中を指しているときだけ読む（外を読むと例外になりうる）
static void walk_calls(int core, uint64_t *calls)
{
    uint64_t lo = mmu_stack_guard(core) + MMU_STACK_GUARD_SIZE;
    uint64_t hi = mmu_stack_guard(core) - 0x1000 + 0x800000;    // スタックは 8MB（linker.ld）
    calls[0] = irq_lr[core];
    uint64_t fp = irq_fp[core];
    for (int i = 1; i < PMU_CALLS; i++) {
        if (fp < lo || fp + 16 > hi || (fp & 7)) {
            calls[i] = 0;
            continue;
        }
        const uint64_t *rec = (const uint64_t *)fp;
        calls[i] = rec[1];
        fp = rec[0] > fp ? rec[0] : 0;  // スタックは上へたどる
    }
}

void pmu_handle_irq(void)
{
    int core = cpu_id();
    uint64_t ovs = read_sysreg(pmovsclr_el0);
    write_sysreg(pmovsclr_el0, ovs);
    for (int i = 0; i < nev; i++) {
        if (!((ovs >> i) & 1))
            continue;
        if (sampling && i == 0) {
            acc[core][0] += period;
            int k = atomic_fetch_add(&nsamples, 1);
            if (k < PMU_MAX_SAMPLES) {
                samples[k].pc = irq_elr[core];
                samples[k].core = (uint8_t)core;
                samples[k].name = nullptr;
                samples[k].off = 0;
                walk_calls(core, samples[k].calls);
                for (int c = 0; c < PMU_CALLS; c++)
                    samples[k].call_names[c] = nullptr;
            } else {
                atomic_fetch_add(&dropped, 1);
            }
            // 次のあふれまで、また period 回
            write_sysreg(pmselr_el0, 0UL);
            isb();
            write_sysreg(pmxevcntr_el0, (uint64_t)(uint32_t)(0x100000000ULL - period));
        } else {
            acc[core][i] += 1UL << 32;
        }
    }
}

void pmu_record_begin(uint16_t event, uint32_t every)
{
    period = every;
    atomic_store(&nsamples, 0);
    atomic_store(&dropped, 0);
    sampling = true;
    pmu_begin(&event, 1);
}

bool pmu_recording(void)
{
    return sampling;
}

const PmuSample *pmu_samples(int *n, int *lost)
{
    int k = atomic_load(&nsamples);
    *n = k < PMU_MAX_SAMPLES ? k : PMU_MAX_SAMPLES;
    *lost = atomic_load(&dropped);
    return samples;
}

PmuSample *pmu_samples_mut(int *n)
{
    int k = atomic_load(&nsamples);
    *n = k < PMU_MAX_SAMPLES ? k : PMU_MAX_SAMPLES;
    return samples;
}

static void reset_counts(void)
{
    memset(acc, 0, sizeof(acc));
    for (int c = 0; c < SMP_MAX_CORES; c++)
        used[c] = false;
}

void pmu_begin(const uint16_t *events_, int n)
{
    uint64_t daif = irq_save();
    nev = n;
    memcpy(evs, events_, sizeof(uint16_t) * (size_t)n);
    reset_counts();
    app_mode = frozen = false;
    counting = true;
    used[0] = true;
    t_start = timer_now_us();
    hw_start();
    irq_restore(daif);
}

void pmu_app_begin(void)
{
    if (!counting)
        return;
    // コンパイルなど、アプリを始めるまでの分は捨てる
    uint64_t daif = irq_save();
    hw_stop_add(0);
    reset_counts();
    atomic_store(&nsamples, 0);     // コンパイルの間のサンプルも捨てる
    atomic_store(&dropped, 0);
    app_mode = true;
    used[0] = true;
    t_start = timer_now_us();
    hw_start();
    irq_restore(daif);
}

void pmu_app_end(void)
{
    if (!counting || !app_mode || frozen)
        return;
    uint64_t daif = irq_save();
    hw_stop_add(0);
    t_end = timer_now_us();
    frozen = true;
    irq_restore(daif);
}

void pmu_job_begin(void)
{
    int core = cpu_id();
    if (!counting || frozen)
        return;
    used[core] = true;
    job_counting[core] = true;
    hw_start();
}

void pmu_job_end(void)
{
    int core = cpu_id();
    if (!job_counting[core])
        return;
    uint64_t daif = irq_save();
    hw_stop_add(core);
    job_counting[core] = false;
    irq_restore(daif);
}

void pmu_end(PmuResult *r)
{
    uint64_t daif = irq_save();
    if (counting && !frozen) {
        hw_stop_add(0);
        t_end = timer_now_us();
    }
    counting = false;
    sampling = false;
    irq_restore(daif);

    memset(r, 0, sizeof(*r));
    r->n = nev;
    r->app_only = app_mode;
    r->elapsed_us = t_end - t_start;
    r->cycles = acc[0][nev];
    for (int c = 0; c < SMP_MAX_CORES; c++) {
        r->used_core[c] = used[c];
        for (int i = 0; i <= nev; i++)
            r->per_core[c][i] = acc[c][i];
        for (int i = 0; i < nev; i++)
            r->counts[i] += acc[c][i];
    }
}
