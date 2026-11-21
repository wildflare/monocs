// pthread の試験（tools/test/pthread.sh）。Linux（gcc -pthread）と Monocs で同じ出力になること。
// Monocs はスレッド 1 つにコア 1 つなので、同時に動かすスレッドは 2 つまで（main を入れて 3 つ）にする。
//   pthtest           ふつうの試験
//   pthtest exit      スレッドの中の exit(5) でアプリ全体が終わる（終了の値 5。after は出ない）
//   pthtest crash     スレッドの中の NULL への書き込みでアプリ全体が止まる（Linux は SIGSEGV、Monocs は 139）
//   pthtest many      空いたコアより多く作る（Monocs は EAGAIN。Linux は作れる）
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { int _r = (x); if (_r) printf("FAIL %s: %d (%s)\n", #x, _r, strerror(_r)); } while (0)

static void *sum_to(void *arg)
{
    long n = (long)arg, s = 0;
    for (long i = 1; i <= n; i++)
        s += i;
    return (void *)s;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *add_many(void *arg)
{
    for (long i = 0; i < (long)arg; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    return nullptr;
}

// 生産者と消費者（条件変数）
#define QN 8
static int queue[QN], qhead, qtail, qcount;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qnotempty = PTHREAD_COND_INITIALIZER, qnotfull = PTHREAD_COND_INITIALIZER;

static void *producer(void *arg)
{
    for (int i = 1; i <= (int)(long)arg; i++) {
        pthread_mutex_lock(&qmu);
        while (qcount == QN)
            pthread_cond_wait(&qnotfull, &qmu);
        queue[qtail] = i;
        qtail = (qtail + 1) % QN;
        qcount++;
        pthread_cond_signal(&qnotempty);
        pthread_mutex_unlock(&qmu);
    }
    pthread_mutex_lock(&qmu);
    while (qcount == QN)
        pthread_cond_wait(&qnotfull, &qmu);
    queue[qtail] = -1;      // 終わりの印
    qtail = (qtail + 1) % QN;
    qcount++;
    pthread_cond_signal(&qnotempty);
    pthread_mutex_unlock(&qmu);
    return nullptr;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_count;
static void once_fn(void)
{
    once_count++;
}

static void *call_once(void *)
{
    pthread_once(&once, once_fn);
    return nullptr;
}

static pthread_key_t key;
static int destructed;
static void destructor(void *v)
{
    destructed += *(int *)v;
    free(v);
}

static void *use_key(void *)
{
    int *v = malloc(sizeof *v);
    *v = 7;
    pthread_setspecific(key, v);
    return pthread_getspecific(key) == v ? (void *)1 : nullptr;
}

static pthread_barrier_t barrier;
static atomic_int phase_sum[3];
static void *barrier_worker(void *arg)
{
    long id = (long)arg;
    int serial = 0;
    for (int round = 0; round < 3; round++) {
        atomic_fetch_add(&phase_sum[round], (int)id);
        if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD)
            serial++;
    }
    return (void *)(long)serial;
}

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static int shared_value;
static void *writer(void *)
{
    for (int i = 0; i < 1000; i++) {
        pthread_rwlock_wrlock(&rw);
        shared_value++;
        pthread_rwlock_unlock(&rw);
    }
    return nullptr;
}

static volatile int detached_done;
static void *detached_fn(void *)
{
    detached_done = 1;
    return nullptr;
}

static pthread_t main_id;
static void *self_fn(void *)
{
    return (void *)(long)(pthread_equal(pthread_self(), main_id) ? 1 : 2);
}

static void *exit_fn(void *)
{
    exit(5);
}

static void *crash_fn(void *)
{
    *(volatile int *)0 = 1;
    return nullptr;
}

static void *exit_thread_fn(void *)
{
    pthread_exit((void *)42);
    return nullptr;
}

static void *sleeper(void *)
{
    struct timespec ts = {0, 300 * 1000 * 1000};
    nanosleep(&ts, nullptr);
    return nullptr;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    main_id = pthread_self();
    if (argc > 1 && !strcmp(argv[1], "exit")) {
        pthread_t t;
        pthread_create(&t, nullptr, exit_fn, nullptr);
        pthread_join(t, nullptr);
        printf("after\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "crash")) {
        pthread_t t;
        pthread_create(&t, nullptr, crash_fn, nullptr);
        pthread_join(t, nullptr);
        printf("after\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "many")) {
        pthread_t t[8];
        int made = 0, r = 0;
        for (int i = 0; i < 8; i++) {
            r = pthread_create(&t[i], nullptr, sleeper, nullptr);
            if (r)
                break;
            made++;
        }
        printf("made %d, then %s\n", made, r == EAGAIN ? "EAGAIN" : r ? "other error" : "no error");
        for (int i = 0; i < made; i++)
            pthread_join(t[i], nullptr);
        // 終わったら、また作れる
        pthread_t u;
        printf("again %d\n", pthread_create(&u, nullptr, sleeper, nullptr));
        pthread_join(u, nullptr);
        return 0;
    }

    // 1. 戻り値
    pthread_t a, b;
    CHECK(pthread_create(&a, nullptr, sum_to, (void *)1000000L));
    CHECK(pthread_create(&b, nullptr, sum_to, (void *)2000L));
    void *ra, *rb;
    CHECK(pthread_join(a, &ra));
    CHECK(pthread_join(b, &rb));
    printf("sums %ld %ld\n", (long)ra, (long)rb);

    // 2. mutex（main も一緒に足す）
    CHECK(pthread_create(&a, nullptr, add_many, (void *)200000L));
    CHECK(pthread_create(&b, nullptr, add_many, (void *)200000L));
    add_many((void *)100000L);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    printf("counter %ld\n", counter);

    // 3. 生産者と消費者
    CHECK(pthread_create(&a, nullptr, producer, (void *)10000L));
    long total = 0;
    int items = 0;
    for (;;) {
        pthread_mutex_lock(&qmu);
        while (qcount == 0)
            pthread_cond_wait(&qnotempty, &qmu);
        int v = queue[qhead];
        qhead = (qhead + 1) % QN;
        qcount--;
        pthread_cond_signal(&qnotfull);
        pthread_mutex_unlock(&qmu);
        if (v < 0)
            break;
        total += v;
        items++;
    }
    pthread_join(a, nullptr);
    printf("queue %d items, total %ld\n", items, total);

    // 4. once
    CHECK(pthread_create(&a, nullptr, call_once, nullptr));
    CHECK(pthread_create(&b, nullptr, call_once, nullptr));
    call_once(nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    printf("once %d\n", once_count);

    // 5. key と後始末の関数
    CHECK(pthread_key_create(&key, destructor));
    CHECK(pthread_create(&a, nullptr, use_key, nullptr));
    pthread_join(a, &ra);
    printf("key ok %ld, destructed %d, main value %s\n", (long)ra, destructed, pthread_getspecific(key) ? "set" : "NULL");

    // 6. バリア（main とスレッド 2 つ）
    CHECK(pthread_barrier_init(&barrier, nullptr, 3));
    CHECK(pthread_create(&a, nullptr, barrier_worker, (void *)1L));
    CHECK(pthread_create(&b, nullptr, barrier_worker, (void *)10L));
    long serial = (long)barrier_worker((void *)100L);
    pthread_join(a, &ra);
    pthread_join(b, &rb);
    printf("barrier rounds %d %d %d, serial %ld\n", atomic_load(&phase_sum[0]), atomic_load(&phase_sum[1]),
           atomic_load(&phase_sum[2]),
           serial + (long)ra + (long)rb);
    pthread_barrier_destroy(&barrier);

    // 7. 読み書きのロック
    CHECK(pthread_create(&a, nullptr, writer, nullptr));
    CHECK(pthread_create(&b, nullptr, writer, nullptr));
    int seen_max = 0;
    for (int i = 0; i < 1000; i++) {
        pthread_rwlock_rdlock(&rw);
        if (shared_value > seen_max)
            seen_max = shared_value;
        pthread_rwlock_unlock(&rw);
    }
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    printf("rwlock %d (monotonic %d)\n", shared_value, seen_max <= 2000);

    // 8. 重ねて取れる mutex、誤りを調べる mutex、trylock
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t rm;
    pthread_mutex_init(&rm, &ma);
    printf("recursive %d %d %d %d\n", pthread_mutex_lock(&rm), pthread_mutex_lock(&rm), pthread_mutex_unlock(&rm),
           pthread_mutex_unlock(&rm));
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_t em;
    pthread_mutex_init(&em, &ma);
    pthread_mutex_lock(&em);
    printf("errorcheck relock %s, trylock %s\n", pthread_mutex_lock(&em) == EDEADLK ? "EDEADLK" : "?",
           pthread_mutex_trylock(&em) == EBUSY ? "EBUSY" : "?");
    pthread_mutex_unlock(&em);
    printf("errorcheck unlock twice %s\n", pthread_mutex_unlock(&em) == EPERM ? "EPERM" : "?");

    // 9. 時間切れ
    pthread_mutex_t tm = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t tc = PTHREAD_COND_INITIALIZER;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&tm);
    printf("timedwait %s\n", pthread_cond_timedwait(&tc, &tm, &ts) == ETIMEDOUT ? "ETIMEDOUT" : "?");
    pthread_mutex_unlock(&tm);

    // 10. 切り離したスレッド、self・equal、pthread_exit、join の誤り
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    CHECK(pthread_create(&a, &at, detached_fn, nullptr));
    while (!detached_done)
        sched_yield();
    printf("detached done\n");
    CHECK(pthread_create(&a, nullptr, self_fn, nullptr));
    pthread_join(a, &ra);
    printf("self %ld, main equal %d\n", (long)ra, pthread_equal(pthread_self(), main_id));
    CHECK(pthread_create(&a, nullptr, exit_thread_fn, nullptr));
    pthread_join(a, &ra);
    printf("pthread_exit %ld, join self %s\n", (long)ra, pthread_join(pthread_self(), nullptr) == EDEADLK ? "EDEADLK" : "?");
    printf("nproc %s\n", sysconf(_SC_NPROCESSORS_ONLN) >= 1 ? ">= 1" : "?");
    printf("done\n");
    return 0;
}
