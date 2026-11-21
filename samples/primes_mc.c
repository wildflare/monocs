// 素数の数を、使えるコアすべてで分担して数える（マルチコアの例）
// ほかのコアで動く関数（count_range）は計算だけをする（C ライブラリを呼ぶ例は libc_mc.c）
#include <stdio.h>
#include <monocs.h>

#define LIMIT 2000000

struct job {
    int from, to;   // [from, to) の奇数を調べる
    int count;
};

static int is_prime(int n)
{
    if (n < 2)
        return 0;
    if (n % 2 == 0)
        return n == 2;
    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0)
            return 0;
    return 1;
}

static void count_range(void *arg)
{
    struct job *j = arg;
    int c = 0;
    for (int n = j->from; n < j->to; n++)
        c += is_prime(n);
    j->count = c;
}

static int run(int ncores)
{
    struct job jobs[4];
    // 大きい数ほど時間がかかるので、交互に区切らず、区間を細かく分けて順に割り当てる
    int step = LIMIT / ncores;
    for (int i = 0; i < ncores; i++) {
        jobs[i].from = i * step;
        jobs[i].to = i == ncores - 1 ? LIMIT : (i + 1) * step;
        jobs[i].count = 0;
    }
    for (int i = 1; i < ncores; i++)
        monocs_core_start(i, count_range, &jobs[i]);
    count_range(&jobs[0]); // コア 0 も分担する
    int total = jobs[0].count;
    for (int i = 1; i < ncores; i++) {
        monocs_core_wait(i);
        total += jobs[i].count;
    }
    return total;
}

int main(void)
{
    int cores = monocs_cores();
    printf("%d cores\n", cores);
    for (int n = 1; n <= cores; n++) {
        uint64_t t = timer_now_us();
        int total = run(n);
        printf("%d core(s): %d primes < %d, %llu ms\n", n, total, LIMIT,
               (unsigned long long)((timer_now_us() - t) / 1000));
    }
    return 0;
}
