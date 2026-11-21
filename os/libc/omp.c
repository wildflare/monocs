// OpenMP のランタイム（コンパイラの側は cc/omp.inc。アプリ用のヘッダーは os/api/include/omp.h）。
// pthread の関数だけで作る（Monocs では os/libc/pthread.c。MONOCS_OMP_LINUX を定義すれば Linux でも動く。
// tools/test/openmp.sh が Linux で GCC の -fopenmp と比べるのに使う）。
//
// チーム: __monocs_omp_parallel が、max_threads - 1 本のスレッドを作る（作れた分だけ。Monocs は空いたコアの数まで）。
// 作ったスレッドは、チームの大きさが決まるまで待ってから、切り出した関数を動かす。main（呼んだスレッド）は
// 番号 0。入れ子の parallel は、1 人のチームで動かす。
// 繰り返しの分け方: static（チャンクなし: 連続した塊を 1 つずつ。チャンク c: c ずつ順に）、dynamic（チームで
// 共有する数え上げから c ずつ取る。guided も同じ）。for の終わりには、nowait でなければバリア。
#define _GNU_SOURCE
#ifdef MONOCS_OMP_LINUX
#include <pthread.h>
#else
#include "../api/include/pthread.h"
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../api/include/omp.h"
#include "omp_os.h"

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 1001   // os/libc/syscalls.c と同じ（picolibc にない）
#endif

#define OMP_MAX_THREADS 16
#define DYN_SLOTS 8

typedef struct {
    void (*fn)(void **);
    void **ctx;
    int n;                          // チームの人数
    atomic_int go;                  // 人数が決まった
    pthread_barrier_t bar;
    atomic_int single;              // 最後に取られた single の番号
    pthread_mutex_t dyn_lock;
    struct {
        atomic_llong next;
        long long seq;
    } dyn[DYN_SLOTS];
} Team;

static _Thread_local Team *cur_team;
static _Thread_local int cur_tid;
static _Thread_local int single_count;  // このスレッドが通った single の数
static _Thread_local long long loop_seq;
static _Thread_local int level;         // parallel の入れ子の深さ
static pthread_mutex_t critical_lock = PTHREAD_MUTEX_INITIALIZER;
static int max_threads;                 // omp_set_num_threads（0 なら決めていない）

typedef struct {
    Team *team;
    int tid;
    pthread_t th;
} Worker;

static void enter(Team *t, int tid)
{
    cur_team = t;
    cur_tid = tid;
    single_count = 0;
    loop_seq = 0;
    level++;
}

static void *worker_main(void *arg)
{
    Worker *w = arg;
    while (!atomic_load_explicit(&w->team->go, memory_order_acquire))
        sched_yield();
    enter(w->team, w->tid);
    w->team->fn(w->team->ctx);
    return NULL;
}

int omp_get_max_threads(void)
{
    if (max_threads > 0)
        return max_threads;
    const char *e = getenv("OMP_NUM_THREADS");
    int n = e ? atoi(e) : 0;
    if (n <= 0)
        n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0)
        n = 1;
    return n > OMP_MAX_THREADS ? OMP_MAX_THREADS : n;
}

void omp_set_num_threads(int n)
{
    max_threads = n > OMP_MAX_THREADS ? OMP_MAX_THREADS : n;
}

void __monocs_omp_parallel(void (*fn)(void **), void **ctx, int nthreads, int cond)
{
    Team *t = calloc(1, sizeof *t);
    if (!t) {
        fn(ctx);
        return;
    }
    t->fn = fn;
    t->ctx = ctx;
    pthread_mutex_init(&t->dyn_lock, NULL);
    int want = nthreads > 0 ? nthreads : omp_get_max_threads();
    if (want > OMP_MAX_THREADS)
        want = OMP_MAX_THREADS;
    if (!cond || level > 0)
        want = 1;           // if(0)・入れ子は 1 人で
    Worker w[OMP_MAX_THREADS];
    int made = 0;
    for (int i = 1; i < want; i++) {
        w[made].team = t;
        w[made].tid = made + 1;
        if (pthread_create(&w[made].th, NULL, worker_main, &w[made]) != 0)
            break;          // 空いたコアがない: 作れた分だけのチーム
        made++;
    }
    t->n = made + 1;
    pthread_barrier_init(&t->bar, NULL, (unsigned)t->n);
    atomic_store_explicit(&t->go, 1, memory_order_release);

    Team *saved = cur_team;
    int saved_tid = cur_tid, saved_single = single_count;
    long long saved_seq = loop_seq;
    enter(t, 0);
    fn(ctx);
    level--;
    for (int i = 0; i < made; i++)
        pthread_join(w[i].th, NULL);
    cur_team = saved;
    cur_tid = saved_tid;
    single_count = saved_single;
    loop_seq = saved_seq;
    pthread_barrier_destroy(&t->bar);
    pthread_mutex_destroy(&t->dyn_lock);
    free(t);
}

// 繰り返しの回数（op: 0 <、1 <=、2 >、3 >=、4 !=）
long long __monocs_omp_trip(long long lb, long long ub, long long st, int op)
{
    if (st == 0)
        return 0;
    if (op == 4)
        op = st > 0 ? 0 : 2;
    switch (op) {
    case 0: return st > 0 && ub > lb ? (ub - lb + st - 1) / st : 0;
    case 1: return st > 0 && ub >= lb ? (ub - lb) / st + 1 : 0;
    case 2: return st < 0 && lb > ub ? (lb - ub - st - 1) / -st : 0;
    case 3: return st < 0 && lb >= ub ? (lb - ub) / -st + 1 : 0;
    }
    return 0;
}

static int team_n(void)
{
    return cur_team ? cur_team->n : 1;
}

// it: [0] 分け方（0 static、1 dynamic）、[1] チャンク、[2] 回数、[3] 次（static: 塊の番号、dynamic: 枠）、
// [4] 済んだか（static でチャンクなし）、[5] 使わない
void __monocs_omp_loop_init(long long *it, int kind, long long chunk, long long n)
{
    it[0] = kind;
    it[1] = chunk;
    it[2] = n;
    it[4] = 0;
    if (kind == 0) {
        it[3] = cur_tid;
        return;
    }
    if (it[1] <= 0)
        it[1] = 1;
    long long seq = ++loop_seq;
    int slot = (int)(seq % DYN_SLOTS);
    it[3] = slot;
    if (!cur_team)
        return;
    pthread_mutex_lock(&cur_team->dyn_lock);
    if (cur_team->dyn[slot].seq != seq) {   // この繰り返しに最初に来た
        cur_team->dyn[slot].seq = seq;
        atomic_store(&cur_team->dyn[slot].next, 0);
    }
    pthread_mutex_unlock(&cur_team->dyn_lock);
}

int __monocs_omp_loop_next(long long *it, long long *from, long long *to)
{
    long long n = it[2], chunk = it[1];
    int nt = team_n();
    if (it[0] == 0 && chunk <= 0) {
        if (it[4])
            return 0;
        it[4] = 1;
        long long q = n / nt, r = n % nt, tid = cur_tid;
        *from = tid * q + (tid < r ? tid : r);
        *to = *from + q + (tid < r ? 1 : 0);
        return *from < *to;
    }
    if (it[0] == 0) {
        long long start = it[3] * chunk;
        if (start >= n)
            return 0;
        *from = start;
        *to = start + chunk < n ? start + chunk : n;
        it[3] += nt;
        return 1;
    }
    long long start;
    if (cur_team) {
        start = atomic_fetch_add(&cur_team->dyn[it[3]].next, chunk);
    } else {
        start = it[4];
        it[4] += chunk;
    }
    if (start >= n)
        return 0;
    *from = start;
    *to = start + chunk < n ? start + chunk : n;
    return 1;
}

void __monocs_omp_barrier(void)
{
    if (cur_team && cur_team->n > 1)
        pthread_barrier_wait(&cur_team->bar);
}

void __monocs_omp_loop_end(long long *it, int nowait)
{
    (void)it;
    if (!nowait)
        __monocs_omp_barrier();
}

int __monocs_omp_single(void)
{
    if (!cur_team)
        return 1;
    int s = ++single_count, expected = s - 1;
    return atomic_compare_exchange_strong(&cur_team->single, &expected, s);
}

void __monocs_omp_lock(void)
{
    pthread_mutex_lock(&critical_lock);
}

void __monocs_omp_unlock(void)
{
    pthread_mutex_unlock(&critical_lock);
}

void __monocs_omp_flush(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

int omp_get_thread_num(void)
{
    return cur_team ? cur_tid : 0;
}

int omp_get_num_threads(void)
{
    return team_n();
}

int omp_get_num_procs(void)
{
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

int omp_in_parallel(void)
{
    return team_n() > 1;
}

int omp_get_level(void)
{
    return level;
}

int omp_get_dynamic(void)
{
    return 0;
}

void omp_set_dynamic(int)
{
}

int omp_get_nested(void)
{
    return 0;
}

void omp_set_nested(int)
{
}

double omp_get_wtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

double omp_get_wtick(void)
{
    return 1e-6;
}

void omp_init_lock(omp_lock_t *l)
{
    l->locked = 0;
}

void omp_destroy_lock(omp_lock_t *)
{
}

int omp_test_lock(omp_lock_t *l)
{
    int expected = 0;
    return atomic_compare_exchange_strong((atomic_int *)(void *)&l->locked, &expected, 1);
}

void omp_set_lock(omp_lock_t *l)
{
    while (!omp_test_lock(l))
        sched_yield();
}

void omp_unset_lock(omp_lock_t *l)
{
    atomic_store((atomic_int *)(void *)&l->locked, 0);
}

static int self_id(void)
{
    return (int)(pthread_self() & 0x7fffffff) + 1;
}

void omp_init_nest_lock(omp_nest_lock_t *l)
{
    l->locked = 0;
    l->owner = 0;
    l->count = 0;
}

void omp_destroy_nest_lock(omp_nest_lock_t *)
{
}

int omp_test_nest_lock(omp_nest_lock_t *l)
{
    int me = self_id();
    if (l->locked && l->owner == me)
        return ++l->count;
    int expected = 0;
    if (!atomic_compare_exchange_strong((atomic_int *)(void *)&l->locked, &expected, 1))
        return 0;
    l->owner = me;
    l->count = 1;
    return 1;
}

void omp_set_nest_lock(omp_nest_lock_t *l)
{
    while (!omp_test_nest_lock(l))
        sched_yield();
}

void omp_unset_nest_lock(omp_nest_lock_t *l)
{
    if (--l->count == 0) {
        l->owner = 0;
        atomic_store((atomic_int *)(void *)&l->locked, 0);
    }
}
