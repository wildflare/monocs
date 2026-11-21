// wfe がどれだけ眠れるかを調べる（2026-11-16）: コア 1 で「何かをしてから wfe」を 10ms 繰り返し、wfe が戻った回数を数える。
// Cortex-A53 では、アトミック操作（ldaxr・stlxr）の後の wfe はすぐ戻る。sevl; wfe で溜まったイベントを消すと眠れる。
//   nos.py run tools/perf/wfe.c（実機。QEMU の wfe は眠らない）
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <monocs.h>
static volatile int mode;
static volatile uint64_t result;
static int lst;
static atomic_int shared;
static monocs_mutex mtx;
static void job(void *a) {
    (void)a;
    uint64_t f, t0, t; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
    uint64_t n = 0;
    do {
        switch (mode % 100) {
        case 1: monocs_mutex_lock(&mtx); monocs_mutex_unlock(&mtx); break;
        case 2: atomic_fetch_add(&shared, 1); break;
        case 3: monocs_tcp_accept(lst, 0); break;
        case 4: free(malloc(16)); break;
        case 5: timer_now_us(); break;
        case 6: { int e = 5; atomic_compare_exchange_strong(&shared, &e, 6); } break;
        }
        if (mode >= 100) __asm__ volatile("sevl\n\twfe" ::: "memory");
        __asm__ volatile("wfe"); n++;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    } while (t - t0 < f / 100);
    result = n;
}
int main(void) {
    lst = monocs_tcp_listen(7777);
    static const char *names[] = {"wfe only", "mutex lock/unlock", "atomic add", "tcp_accept", "malloc/free", "timer_now_us", "failed CAS", "atomic add + sevl;wfe", "failed CAS + sevl;wfe"};
    static const int modes[] = {0, 1, 2, 3, 4, 5, 6, 102, 106};
    for (int k = 0; k < 9; k++) {
        mode = modes[k];
        monocs_core_start(1, job, 0); monocs_core_wait(1);
        printf("%-22s: wfe returns in 10ms = %llu\n", names[k], (unsigned long long)result);
    }
    return 0;
}
