// システムの様子をまとめて返す（OS API の monocs_sysstat。/bin/top が使う）
#include <stdio.h>
#include <string.h>
#include "../api/include/monocs_sysstat.h"
#include "app.h"
#include "smp.h"
#include "svc.h"
#include "sysreg.h"
#include "memory.h"
#include "../drivers/timer.h"
#include "../drivers/mailbox.h"
#include "../fs/vfs.h"
#include "../libc/heap.h"

extern uint64_t irq_counts[4];

static uint64_t to_us(uint64_t ticks, uint64_t freq)
{
    return ticks / freq * 1000000 + ticks % freq * 1000000 / freq;
}

void sysstat_get(MonocsSysStat *st)
{
    memset(st, 0, sizeof *st);
    uint64_t freq = read_sysreg(cntfrq_el0);
    st->uptime_us = timer_now_us();
    // コア 0: アプリかシェル。眠っていた時間は cpu_idle が数える
    st->core[0].state = app_is_running() ? MONOCS_CORE_APP : MONOCS_CORE_SHELL;
    st->core[0].idle_us = timer_idle_us();
    app_current_name(st->app, sizeof st->app);
    for (int c = 0; c < MONOCS_STAT_CORES; c++)
        st->core[c].irqs = irq_counts[c];
    static const int map[] = {MONOCS_CORE_OFF, MONOCS_CORE_IDLE, MONOCS_CORE_JOB, MONOCS_CORE_THREAD, MONOCS_CORE_SERVICES};
    for (int c = 1; c < MONOCS_STAT_CORES; c++) {
        st->core[c].state = map[smp_core_state(c)];
        st->core[c].idle_us = to_us(smp_idle_ticks(c), freq);
    }
    // メモリ
    size_t ka, ku, aa, au;
    malloc_heap_info(&ka, &ku, &aa, &au);
    st->mem_total = memory_heap_end() - memory_heap_start();
    st->kernel_heap = ka;
    st->kernel_in_use = ku;
    st->app_heap = aa;
    st->app_in_use = au;
    struct vfs_cache_info ci;
    vfs_cache_info(&ci);
    st->file_cache = ci.bytes;
    // サービス
    for (int i = 0; i < SVC_MAX && st->nservices < MONOCS_STAT_SERVICES; i++) {
        uint64_t busy;
        if (svc_stat(i, st->service[st->nservices].name, sizeof st->service[0].name, &busy,
                     &st->service[st->nservices].polls, &st->service[st->nservices].work_polls)) {
            st->service[st->nservices].busy_us = to_us(busy, freq);
            st->nservices++;
        }
    }
    st->svc_sleeps = svc_sleeps();
    uint32_t v;
    if (mailbox_get_value(MBOX_TAG_GET_TEMPERATURE, 0, &v))
        st->temp_mc = (int32_t)v;
    if (mailbox_get_value(MBOX_TAG_GET_CLOCK_RATE_MEASURED, MBOX_CLOCK_ARM, &v))
        st->arm_hz = v;
    if (mailbox_get_value(MBOX_TAG_GET_THROTTLED, 0, &v))
        st->throttled = v;
}
