// 素数の数を、pthread で分担して数える（primes_mc.c と同じ計算を、Linux と同じ書き方で）。
// Linux でもそのままコンパイルできる（gcc -O2 -pthread primes_pthread.c）。
// Monocs では、スレッド 1 つに空いたコアを 1 つ渡す（os/api/include/pthread.h）。
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LIMIT 2000000
#define MAX_THREADS 16

struct job {
    int from, to;   // [from, to) を調べる
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

static void *count_range(void *arg)
{
    struct job *j = arg;
    int c = 0;
    for (int n = j->from; n < j->to; n++)
        c += is_prime(n);
    j->count = c;
    return NULL;
}

// n 人（main と n-1 本のスレッド）で分ける
static int run(int n)
{
    struct job jobs[MAX_THREADS];
    pthread_t th[MAX_THREADS];
    int step = LIMIT / n;
    for (int i = 0; i < n; i++) {
        jobs[i].from = i * step;
        jobs[i].to = i == n - 1 ? LIMIT : (i + 1) * step;
        jobs[i].count = 0;
    }
    for (int i = 1; i < n; i++)
        if (pthread_create(&th[i], NULL, count_range, &jobs[i]) != 0) {
            printf("pthread_create failed\n");
            return -1;
        }
    count_range(&jobs[0]);      // main も分担する
    int total = jobs[0].count;
    for (int i = 1; i < n; i++) {
        pthread_join(th[i], NULL);
        total += jobs[i].count;
    }
    return total;
}

static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

int main(void)
{
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > MAX_THREADS)
        cores = MAX_THREADS;
    printf("%d cores\n", cores);
    for (int n = 1; n <= cores; n++) {
        long t = ms_now();
        int c = run(n);
        printf("%d thread(s): %d primes < %d, %ld ms\n", n, c, LIMIT, ms_now() - t);
    }
    return 0;
}
