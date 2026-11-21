// pthread の待ちが眠るか（2026-11-16）: スレッドが待っている間（mutex・条件変数・join・バリア・時間切れのある待ち）、待っているコアが
// 忙しいかを monocs_sysstat で測る。眠れていれば、待っているコアはほとんど 0%。
//   nos.py run tools/perf/pthwait.c（実機。QEMU の wfe は眠らない）
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <monocs.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t bar;
static int ready;

static void *lock_waiter(void *a) { (void)a; pthread_mutex_lock(&mtx); pthread_mutex_unlock(&mtx); return nullptr; }
static void *cond_waiter(void *a)
{
    (void)a;
    pthread_mutex_lock(&mtx);
    while (!ready)
        pthread_cond_wait(&cv, &mtx);
    pthread_mutex_unlock(&mtx);
    return nullptr;
}
static void *bar_waiter(void *a) { (void)a; pthread_barrier_wait(&bar); return nullptr; }
static void *sleeper(void *a) { (void)a; usleep(500000); return nullptr; }

// 時間切れのある待ち（コア 1〜3 は前は回っていた）: 0.45 秒先までの timedlock・cond_timedwait。かかった時間も
static long timed_ms[2];
static void deadline(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += ms * 1000000;
    ts->tv_sec += ts->tv_nsec / 1000000000;
    ts->tv_nsec %= 1000000000;
}
static void *timed_lock_waiter(void *a)
{
    (void)a;
    struct timespec ts;
    deadline(&ts, 450);
    uint64_t t0 = timer_now_us();
    int r = pthread_mutex_timedlock(&mtx, &ts);
    timed_ms[0] = r == ETIMEDOUT ? (long)(timer_now_us() - t0) / 1000 : -r;
    return nullptr;
}
static void *timed_cond_waiter(void *a)
{
    (void)a;
    struct timespec ts;
    deadline(&ts, 450);
    pthread_mutex_lock(&mtx);
    uint64_t t0 = timer_now_us();
    int r = pthread_cond_timedwait(&cv, &mtx, &ts);
    timed_ms[1] = r == ETIMEDOUT ? (long)(timer_now_us() - t0) / 1000 : -r;
    pthread_mutex_unlock(&mtx);
    return nullptr;
}

// ピンポン: 2 つのスレッドが、条件変数で番を渡し合う（起こしてから相手が動くまでの速さ）
static pthread_cond_t pcv = PTHREAD_COND_INITIALIZER;
static int turn, rounds;
static void *ponger(void *a)
{
    (void)a;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < rounds; i++) {
        while (turn != 1)
            pthread_cond_wait(&pcv, &mtx);
        turn = 0;
        pthread_cond_signal(&pcv);
    }
    pthread_mutex_unlock(&mtx);
    return nullptr;
}

static void pingpong(void)
{
    pthread_t t;
    rounds = 10000;
    turn = 0;
    uint64_t t0 = timer_now_us();
    pthread_create(&t, nullptr, ponger, nullptr);
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < rounds; i++) {
        turn = 1;
        pthread_cond_signal(&pcv);
        while (turn != 0)
            pthread_cond_wait(&pcv, &mtx);
    }
    pthread_mutex_unlock(&mtx);
    pthread_join(t, nullptr);
    printf("%-28s %d rounds in %llu ms\n", "ping-pong (cond, core0-1)", rounds,
           (unsigned long long)(timer_now_us() - t0) / 1000);
}

// 0.5 秒の間の、コア 1 とコア 2 の忙しさ（%）
static void measure(const char *what)
{
    MonocsSysStat s0, s1;
    monocs_sysstat(&s0);
    usleep(500000);
    monocs_sysstat(&s1);
    double dt = (double)(s1.uptime_us - s0.uptime_us);
    printf("%-28s core1 %5.1f%%  core2 %5.1f%%\n", what,
           100.0 * (1 - (double)(s1.core[1].idle_us - s0.core[1].idle_us) / dt),
           100.0 * (1 - (double)(s1.core[2].idle_us - s0.core[2].idle_us) / dt));
}

int main(void)
{
    pthread_t t;
    // mutex: main が持っている間、スレッドが待つ
    pthread_mutex_lock(&mtx);
    pthread_create(&t, nullptr, lock_waiter, nullptr);
    measure("mutex wait (thread)");
    pthread_mutex_unlock(&mtx);
    pthread_join(t, nullptr);
    // 条件変数
    pthread_create(&t, nullptr, cond_waiter, nullptr);
    measure("cond wait (thread)");
    pthread_mutex_lock(&mtx);
    ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mtx);
    pthread_join(t, nullptr);
    // バリア: スレッドだけが先に来て待つ
    pthread_barrier_init(&bar, nullptr, 2);
    pthread_create(&t, nullptr, bar_waiter, nullptr);
    measure("barrier wait (thread)");
    pthread_barrier_wait(&bar);
    pthread_join(t, nullptr);
    // join: main（コア 0）が待つ。スレッドは usleep で眠る（コア 1〜3 の usleep は回る）
    pthread_create(&t, nullptr, sleeper, nullptr);
    MonocsSysStat s0, s1;
    monocs_sysstat(&s0);
    pthread_join(t, nullptr);
    monocs_sysstat(&s1);
    double dt = (double)(s1.uptime_us - s0.uptime_us);
    printf("%-28s core0 %5.1f%%\n", "join wait (main, core 0)",
           100.0 * (1 - (double)(s1.core[0].idle_us - s0.core[0].idle_us) / dt));
    // 時間切れのある待ち（0.45 秒で時間切れ。measure は 0.5 秒）
    pthread_mutex_lock(&mtx);
    pthread_create(&t, nullptr, timed_lock_waiter, nullptr);
    measure("timedlock wait (thread)");
    pthread_join(t, nullptr);
    pthread_mutex_unlock(&mtx);
    pthread_create(&t, nullptr, timed_cond_waiter, nullptr);
    measure("cond_timedwait (thread)");
    pthread_join(t, nullptr);
    printf("%-28s timedlock %ld ms, cond_timedwait %ld ms (want 450)\n", "timeouts", timed_ms[0], timed_ms[1]);
    pingpong();
    pingpong();
    return 0;
}
