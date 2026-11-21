// 偽の共有のベンチマーク（Monocs の上で: nos.py run tools/perf/fsbench.c）: 各コアが自分の FILE（/dev/null）に
// fprintf し、malloc・free する（コンパイラと同じ形。fprintf のたびに FILE のロックを取る）。
// 1 コアと 3 コアで同じ仕事をして時間を比べる。ロックやコアごとの変数が同じキャッシュラインに並ぶと、3 コアの時間が延びる。
// 2026-11-14（実機）: 1 コア 123 ms に対して 3 コア 150〜155 ms → ロックをキャッシュラインにそろえて 129〜134 ms
#include <stdio.h>
#include <stdlib.h>
#include <monocs.h>
#define N 200000
static volatile uint64_t took[4];
static void work(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint64_t t0 = timer_now_us();
    FILE *f = fopen("/dev/null", "w");
    for (int i = 0; i < N; i++) {
        fprintf(f, "%d,", i);
        if ((i & 7) == 0) {
            void *p = malloc(48);
            free(p);
        }
    }
    fclose(f);
    took[id] = timer_now_us() - t0;
}
int main(void)
{
    int n = monocs_cores();
    uint64_t t0 = timer_now_us();
    work((void *)0);
    uint64_t one = timer_now_us() - t0;
    t0 = timer_now_us();
    for (int c = 1; c < n; c++)
        monocs_core_start(c, work, (void *)(intptr_t)c);
    work((void *)0);
    for (int c = 1; c < n; c++)
        monocs_core_wait(c);
    uint64_t all = timer_now_us() - t0;
    printf("1 core %llu ms, %d cores %llu ms\n", (unsigned long long)one / 1000, n, (unsigned long long)all / 1000);
    return 0;
}
