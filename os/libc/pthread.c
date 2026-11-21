// POSIX のスレッド（os/api/include/pthread.h）。スレッド 1 つに空いたコアを 1 つまるごと渡す（スケジューラなし）。
//
// - スレッドの記録（Thr）はコアとは別に持つ（終わって join されていないものの戻り値を残すため）。コアの仕事は
//   os/kernel/smp.c の smp_run_thread（例外・exit をアプリ全体に広げる印付き）
// - 待つ関数は、条件を調べては wfe で眠る（mutex を外す・条件変数を知らせる・スレッドが終わるたびに sev で
//   起こす）。コア 0 はタイマーの割り込みでも起きるので、待っている間も Ctrl-C で止められ、シグナルの関数も呼ぶ
// - アプリが終わったら（app_call）、まだ動いているスレッドのコアは smp_abort_level で止まる。記録は
//   pthread_app_end で消す
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../api/include/pthread.h"
#include "pthread_os.h"
#include "signal_os.h"
#include "../kernel/app.h"
#include "../kernel/smp.h"
#include "../kernel/sysreg.h"
#include "../drivers/timer.h"

#define MAX_THREADS 32

enum { T_FREE, T_RUNNING, T_DONE };

typedef struct {
    volatile int state;
    pthread_t id;
    int core;
    int level;          // 作ったアプリの入れ子の深さ
    bool detached;
    void *(*start)(void *);
    void *arg;
    void *ret;
    jmp_buf exit_jb;    // pthread_exit で戻るところ（そのコアのスタックの上）
    void *keys[PTHREAD_KEYS_MAX];
    char name[16];
} Thr;

static Thr thr[MAX_THREADS];
static Thr *volatile on_core[SMP_MAX_CORES];    // そのコアで動いているスレッド（コア 0 は NULL = main）
static void *main_keys[PTHREAD_KEYS_MAX];       // main のスレッドの pthread_setspecific
static atomic_flag table_lock;
static pthread_t next_id = 1;                   // main は 1

static struct {
    volatile bool used;
    void (*destructor)(void *);
} keys[PTHREAD_KEYS_MAX];

#define MAIN_ID ((pthread_t)1)

// 待つ・起こす（2026-11-16）: 何かを変えた側は wake で通し番号（wake_gen）を増やして sev する。待つ側は、条件を
// 見る前に wake_mark で通し番号を読み、条件が合わなければ wait_change(印) で眠る。
// wait_change は、溜まったイベントを消してから（sevl; wfe）、通し番号を ldaxr で見張って wfe する。見張っている
// 番地にほかのコアが書けば、見張りが外れて wfe が起きる（Linux の __cmpwait と同じ）ので、条件を見た後に起こされても
// 取りこぼさない。前は wfe だけで、Cortex-A53 ではアトミック操作の後の wfe がすぐ戻るため（os/kernel/svc.c）、
// 待っているコアはいつも 100% で回っていた（tools/perf/pthwait.c で確かめた）
static atomic_uint wake_gen;

static void wake(void)
{
    atomic_fetch_add_explicit(&wake_gen, 1, memory_order_release);
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

static unsigned wake_mark(void)
{
    return atomic_load_explicit(&wake_gen, memory_order_acquire);
}

// 通し番号が mark から変わるまで眠る（割り込み・sev でも起きる。起きたら呼んだ側が条件を見直す）。
// コア 0 なら、届いたシグナルの関数も呼ぶ
static void wait_change(unsigned mark)
{
    unsigned v;
    __asm__ volatile("sevl\n\twfe\n\tldaxr %w0, [%1]" : "=&r"(v) : "r"(&wake_gen) : "memory");
    if (v == mark) {
        uint64_t t0 = read_sysreg(cntpct_el0);
        __asm__ volatile("wfe" ::: "memory");
        cpu_idle_account(read_sysreg(cntpct_el0) - t0);     // 眠っていた時間（top）
    } else {
        __asm__ volatile("clrex" ::: "memory");
    }
    if (cpu_id() == 0)
        signal_point();
}

static void table_take(void)
{
    while (atomic_flag_test_and_set_explicit(&table_lock, memory_order_acquire))
        __asm__ volatile("yield");
}

static void table_give(void)
{
    atomic_flag_clear_explicit(&table_lock, memory_order_release);
}

static Thr *find(pthread_t id)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (thr[i].state != T_FREE && thr[i].id == id)
            return &thr[i];
    return nullptr;
}

static Thr *self_thr(void)
{
    int c = cpu_id();
    return c == 0 ? nullptr : on_core[c];
}

static int self_tag(void)
{
    return cpu_id() + 1;    // mutex の持ち主（スレッドはコアと 1 対 1）
}

// 時刻 abstime（clock）を過ぎたか
static bool past(const struct timespec *abstime, int clock)
{
    if (!abstime)
        return false;
    struct timespec now;
    clock_gettime(clock == CLOCK_MONOTONIC ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
    return now.tv_sec > abstime->tv_sec || (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec);
}

// 時間切れのある待ち: コア 1〜3 は割り込みで起きない（時間切れに気づけない）ので、待つ間だけタイマーのイベント
// ストリームを入れ、約 0.85ms ごとに起きて時間を見る（前は yield で回り続けていた）。コア 0 はタイマーの割り込み
// （10ms ごと）で起きる
static void wait_timed(const struct timespec *abstime, unsigned mark)
{
    if (abstime && cpu_id() != 0) {
        cpu_event_stream(true);
        wait_change(mark);
        cpu_event_stream(false);
    } else {
        wait_change(mark);
    }
}

// ---- スレッド ----

static void run_destructors(Thr *t)
{
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
            void *v = t->keys[k];
            if (v && keys[k].used && keys[k].destructor) {
                t->keys[k] = nullptr;
                keys[k].destructor(v);
                any = true;
            }
        }
        if (!any)
            break;
    }
}

static void trampoline(void *arg)
{
    Thr *t = arg;
    if (setjmp(t->exit_jb) == 0)
        t->ret = t->start(t->arg);
    run_destructors(t);
    table_take();
    on_core[t->core] = nullptr;
    if (t->detached)
        t->state = T_FREE;
    else
        t->state = T_DONE;
    table_give();
    wake();
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    // 作れるのはアプリ（コア 0）とそのスレッドから（サービス（コア 3）からは作れない）
    if (!app_is_running() || (cpu_id() != 0 && !self_thr()))
        return EAGAIN;
    table_take();
    Thr *t = nullptr;
    for (int i = 0; i < MAX_THREADS && !t; i++)
        if (thr[i].state == T_FREE)
            t = &thr[i];
    if (!t) {
        table_give();
        return EAGAIN;
    }
    memset(t, 0, sizeof *t);
    t->state = T_RUNNING;
    t->id = ++next_id;
    t->level = app_depth();
    t->detached = attr && attr->detachstate == PTHREAD_CREATE_DETACHED;
    t->start = start;
    t->arg = arg;
    // 空いたコアを探す。スレッドが終わった直後のコアは、仕事を終えるまで少し待つ
    int core = -1;
    for (int pass = 0; pass < 2 && core < 0; pass++) {
        for (int c = 1; c < SMP_MAX_CORES && core < 0; c++) {
            if (on_core[c])
                continue;
            if (pass == 1)
                for (int k = 0; k < 100000 && smp_busy(c); k++)
                    __asm__ volatile("yield");
            on_core[c] = t;
            t->core = c;
            if (smp_run_thread(c, trampoline, t))
                core = c;
            else
                on_core[c] = nullptr;
        }
    }
    if (core < 0) {
        t->state = T_FREE;
        table_give();
        return EAGAIN;  // 空いたコアがない（スレッドは、コアの数までしか同時に動かせない）
    }
    if (thread)
        *thread = t->id;
    table_give();
    return 0;
}

int pthread_join(pthread_t id, void **retval)
{
    if (id == pthread_self())
        return EDEADLK;
    table_take();
    Thr *t = find(id);
    if (!t || t->detached) {
        table_give();
        return t ? EINVAL : ESRCH;
    }
    table_give();
    for (;;) {
        unsigned mark = wake_mark();
        if (t->state != T_RUNNING)
            break;
        wait_change(mark);
    }
    table_take();
    if (retval)
        *retval = t->ret;
    t->state = T_FREE;
    table_give();
    return 0;
}

int pthread_detach(pthread_t id)
{
    table_take();
    Thr *t = find(id);
    int r = 0;
    if (!t)
        r = ESRCH;
    else if (t->detached)
        r = EINVAL;
    else if (t->state == T_DONE)
        t->state = T_FREE;
    else
        t->detached = true;
    table_give();
    return r;
}

void pthread_exit(void *retval)
{
    Thr *t = self_thr();
    if (t) {
        t->ret = retval;
        longjmp(t->exit_jb, 1);
    }
    // main のスレッド: ほかのスレッドが終わるのを待ってから、アプリを終わらせる（Linux と同じ）
    for (;;) {
        unsigned mark = wake_mark();
        bool any = false;
        for (int i = 0; i < MAX_THREADS; i++)
            if (thr[i].state == T_RUNNING && thr[i].level >= app_depth())
                any = true;
        if (!any)
            break;
        wait_change(mark);
    }
    app_exit_with_handlers(0);
}

pthread_t pthread_self(void)
{
    Thr *t = self_thr();
    return t ? t->id : MAIN_ID;
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

int pthread_cancel(pthread_t)
{
    return ENOSYS;
}

int pthread_setcancelstate(int, int *old)
{
    if (old)
        *old = PTHREAD_CANCEL_ENABLE;
    return 0;
}

int pthread_setcanceltype(int, int *old)
{
    if (old)
        *old = PTHREAD_CANCEL_DEFERRED;
    return 0;
}

void pthread_testcancel(void)
{
}

int pthread_setname_np(pthread_t id, const char *name)
{
    if (strlen(name) >= 16)
        return ERANGE;
    Thr *t = id == MAIN_ID ? nullptr : find(id);
    if (id != MAIN_ID && !t)
        return ESRCH;
    if (t)
        strcpy(t->name, name);
    return 0;
}

int pthread_getname_np(pthread_t id, char *name, size_t len)
{
    Thr *t = id == MAIN_ID ? nullptr : find(id);
    if (id != MAIN_ID && !t)
        return ESRCH;
    const char *n = t ? t->name : "main";
    if (strlen(n) >= len)
        return ERANGE;
    strcpy(name, n);
    return 0;
}

// シグナルは main のスレッド（コア 0）にだけ届く。ほかのスレッドへの pthread_kill は、アプリに届ける
int pthread_kill(pthread_t id, int sig)
{
    if (id != MAIN_ID && !find(id))
        return ESRCH;
    if (sig == 0)
        return 0;
    return raise(sig) == 0 ? 0 : errno;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    return sigprocmask(how, set, old) == 0 ? 0 : errno;
}

int pthread_attr_init(pthread_attr_t *attr)
{
    *attr = (pthread_attr_t){PTHREAD_CREATE_JOINABLE, 8u << 20};
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *)
{
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state)
{
    *state = attr->detachstate;
    return 0;
}

// スタックはコアごとに 8MB（大きさは変えられない。覚えるだけ）
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    attr->stacksize = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size)
{
    *size = attr->stacksize ? attr->stacksize : 8u << 20;
    return 0;
}

// ---- mutex ----

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    attr->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *)
{
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    *type = attr->type;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *, int pshared)
{
    return pshared == PTHREAD_PROCESS_PRIVATE || pshared == PTHREAD_PROCESS_SHARED ? 0 : EINVAL;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    *m = (pthread_mutex_t){0, 0, 0, attr ? attr->type : PTHREAD_MUTEX_DEFAULT};
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return m->lock ? EBUSY : 0;
}

static bool try_take(pthread_mutex_t *m)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit((atomic_int *)(void *)&m->lock, &expected, 1,
                                                 memory_order_acquire, memory_order_relaxed))
        return false;
    m->owner = self_tag();
    m->count = 1;
    return true;
}

// 持っているときの扱い（重ねて取る・誤り）。0 なら取れた、-1 なら普通に待つ、ほかは誤りの番号
static int relock(pthread_mutex_t *m)
{
    if (m->lock && m->owner == self_tag()) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        if (m->type == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
    }
    return -1;
}

static int mutex_lock(pthread_mutex_t *m, const struct timespec *abstime)
{
    int r = relock(m);
    if (r >= 0)
        return r;
    for (;;) {
        unsigned mark = wake_mark();
        if (try_take(m))
            return 0;
        if (past(abstime, CLOCK_REALTIME))
            return ETIMEDOUT;
        wait_timed(abstime, mark);
    }
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    return mutex_lock(m, nullptr);
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime)
{
    return mutex_lock(m, abstime);
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    int r = relock(m);
    if (r == 0)
        return 0;
    return try_take(m) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->type != PTHREAD_MUTEX_NORMAL && (!m->lock || m->owner != self_tag()))
        return EPERM;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0)
        return 0;
    m->owner = 0;
    m->count = 0;
    atomic_store_explicit((atomic_int *)(void *)&m->lock, 0, memory_order_release);
    wake();
    return 0;
}

// ---- 条件変数 ----

int pthread_condattr_init(pthread_condattr_t *attr)
{
    attr->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *)
{
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return EINVAL;
    attr->clock = clock;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *, int pshared)
{
    return pshared == PTHREAD_PROCESS_PRIVATE || pshared == PTHREAD_PROCESS_SHARED ? 0 : EINVAL;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    *c = (pthread_cond_t){0, attr ? attr->clock : CLOCK_REALTIME};
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *)
{
    return 0;
}

// 知らせ（seq が変わる）を待つ。知らせは待っているもの全部を起こす（POSIX が認める、余分に起きる場合）
static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    unsigned seq = atomic_load_explicit((atomic_uint *)(void *)&c->seq, memory_order_acquire);
    int count = m->count;       // 重ねて取った mutex も、いったん全部外す
    m->count = 1;
    pthread_mutex_unlock(m);
    int r = 0;
    for (;;) {
        unsigned mark = wake_mark();
        if (atomic_load_explicit((atomic_uint *)(void *)&c->seq, memory_order_acquire) != seq)
            break;
        if (past(abstime, c->clock)) {
            r = ETIMEDOUT;
            break;
        }
        wait_timed(abstime, mark);
    }
    for (;;) {
        unsigned mark = wake_mark();
        if (try_take(m))
            break;
        wait_change(mark);
    }
    m->count = count;
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return cond_wait(c, m, nullptr);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    return cond_wait(c, m, abstime);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    atomic_fetch_add_explicit((atomic_uint *)(void *)&c->seq, 1, memory_order_release);
    wake();
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    return pthread_cond_signal(c);
}

// ---- once・key ----

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit((atomic_int *)(void *)&once->state, &expected, 1,
                                                memory_order_acquire, memory_order_relaxed)) {
        fn();
        atomic_store_explicit((atomic_int *)(void *)&once->state, 2, memory_order_release);
        wake();
        return 0;
    }
    for (;;) {
        unsigned mark = wake_mark();
        if (atomic_load_explicit((atomic_int *)(void *)&once->state, memory_order_acquire) == 2)
            break;
        wait_change(mark);
    }
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    table_take();
    for (unsigned k = 0; k < PTHREAD_KEYS_MAX; k++) {
        if (!keys[k].used) {
            keys[k].used = true;
            keys[k].destructor = destructor;
            main_keys[k] = nullptr;
            for (int i = 0; i < MAX_THREADS; i++)
                thr[i].keys[k] = nullptr;
            table_give();
            *key = k;
            return 0;
        }
    }
    table_give();
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !keys[key].used)
        return EINVAL;
    keys[key].used = false;
    return 0;
}

static void **key_slot(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !keys[key].used)
        return nullptr;
    Thr *t = self_thr();
    return t ? &t->keys[key] : &main_keys[key];
}

void *pthread_getspecific(pthread_key_t key)
{
    void **s = key_slot(key);
    return s ? *s : nullptr;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    void **s = key_slot(key);
    if (!s)
        return EINVAL;
    *s = (void *)value;
    return 0;
}

// ---- 読み書きのロック・バリア・スピンロック ----

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *)
{
    rw->state = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw)
{
    return rw->state ? EBUSY : 0;
}

static bool rw_try_read(pthread_rwlock_t *rw)
{
    int s = atomic_load_explicit((atomic_int *)(void *)&rw->state, memory_order_relaxed);
    return s >= 0 && atomic_compare_exchange_weak_explicit((atomic_int *)(void *)&rw->state, &s, s + 1,
                                                           memory_order_acquire, memory_order_relaxed);
}

static bool rw_try_write(pthread_rwlock_t *rw)
{
    int s = 0;
    return atomic_compare_exchange_strong_explicit((atomic_int *)(void *)&rw->state, &s, -1, memory_order_acquire,
                                                   memory_order_relaxed);
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
    for (;;) {
        unsigned mark = wake_mark();
        if (rw_try_read(rw))
            break;
        wait_change(mark);
    }
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw)
{
    for (int i = 0; i < 4; i++)     // weak の CAS は、ほかのものに当たらなくても失敗することがある
        if (rw_try_read(rw))
            return 0;
    return EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
    for (;;) {
        unsigned mark = wake_mark();
        if (rw_try_write(rw))
            break;
        wait_change(mark);
    }
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rw)
{
    return rw_try_write(rw) ? 0 : EBUSY;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
    int s = atomic_load_explicit((atomic_int *)(void *)&rw->state, memory_order_relaxed);
    if (s == -1)
        atomic_store_explicit((atomic_int *)(void *)&rw->state, 0, memory_order_release);
    else if (s > 0)
        atomic_fetch_sub_explicit((atomic_int *)(void *)&rw->state, 1, memory_order_release);
    else
        return EPERM;
    wake();
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *, unsigned count)
{
    if (count == 0)
        return EINVAL;
    *b = (pthread_barrier_t){count, 0, 0};
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *)
{
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
    unsigned phase = atomic_load_explicit((atomic_uint *)(void *)&b->phase, memory_order_acquire);
    unsigned n = atomic_fetch_add_explicit((atomic_uint *)(void *)&b->waiting, 1, memory_order_acq_rel) + 1;
    if (n == b->count) {
        atomic_store_explicit((atomic_uint *)(void *)&b->waiting, 0, memory_order_relaxed);
        atomic_fetch_add_explicit((atomic_uint *)(void *)&b->phase, 1, memory_order_release);
        wake();
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    for (;;) {
        unsigned mark = wake_mark();
        if (atomic_load_explicit((atomic_uint *)(void *)&b->phase, memory_order_acquire) != phase)
            break;
        wait_change(mark);
    }
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *s, int)
{
    *s = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *)
{
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *s)
{
    int expected = 0;
    while (!atomic_compare_exchange_weak_explicit((atomic_int *)(void *)s, &expected, 1, memory_order_acquire,
                                                  memory_order_relaxed)) {
        expected = 0;
        __asm__ volatile("yield");
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *s)
{
    int expected = 0;
    return atomic_compare_exchange_strong_explicit((atomic_int *)(void *)s, &expected, 1, memory_order_acquire,
                                                   memory_order_relaxed) ? 0 : EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *s)
{
    atomic_store_explicit((atomic_int *)(void *)s, 0, memory_order_release);
    return 0;
}

int sched_yield(void)
{
    __asm__ volatile("yield");
    return 0;
}

// ---- アプリの出入り（os/slimcc/run.c）----

// アプリ（入れ子の深さ level）が終わった: そのアプリと中のアプリのスレッドの記録を消す（コアは app_call が
// smp_abort_level で止めてある）
void pthread_app_end(int level)
{
    table_take();
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thr[i].state != T_FREE && thr[i].level >= level) {
            if (thr[i].core > 0 && on_core[thr[i].core] == &thr[i])
                on_core[thr[i].core] = nullptr;
            thr[i].state = T_FREE;
        }
    }
    if (level <= 1) {
        memset(main_keys, 0, sizeof main_keys);
        for (int k = 0; k < PTHREAD_KEYS_MAX; k++)
            keys[k].used = false;
    }
    table_give();
}
