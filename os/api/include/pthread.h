// pthread.h: POSIX のスレッド（Monocs。os/libc/pthread.c）
//
// Monocs にはスケジューラがないので、スレッド 1 つに、アプリが使える空いたコア（コア 1〜2。サービスがなければ 3 も）
// を 1 つまるごと渡す。同時に作れるスレッドはその数まで（それより多く作ろうとすると EAGAIN。sysconf の
// _SC_NPROCESSORS_ONLN はコア 0 を含むコアの数）。待つ関数（mutex・条件変数・join など）は、コアを wfe で眠らせて
// 待つ。動いているのは 1 つのアプリだけで、スレッドはそのアプリの中（同じメモリ）で動く。
//   - スレッドの中の exit()・例外（NULL への書き込みなど）は、アプリ全体を終わらせる（Linux と同じ）
//   - main から戻ったとき（exit）、まだ動いているスレッドは止める（Linux と同じ）
//   - シグナル（Ctrl-C など）は main のスレッド（コア 0）に届く
//   - スレッドの中では system()・popen・curses・monocs_core_start は使えない（コア 0 のもの）
//   - 型の中身は glibc と違う（ソースの互換だけ）。pthread_cancel はない（ENOSYS）
#pragma once

#include <sys/features.h>
#include <sched.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long pthread_t;

typedef struct {
    int detachstate;
    size_t stacksize;
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    int type;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

typedef struct {
    volatile int lock;      // 0 = 空き、1 = 使用中
    volatile int owner;     // 持っているスレッド（コアの番号 + 1）
    int count;              // 重ねて取った数（PTHREAD_MUTEX_RECURSIVE）
    int type;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, PTHREAD_MUTEX_NORMAL}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, 0, 0, PTHREAD_MUTEX_RECURSIVE}

typedef struct {
    int clock;
} pthread_condattr_t;

typedef struct {
    volatile unsigned seq;  // signal・broadcast のたびに増える
    int clock;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0, 1}   // 1 = CLOCK_REALTIME（picolibc の値）

typedef struct {
    volatile int state;     // 0 = まだ、1 = 動かしている、2 = 済み
} pthread_once_t;

#define PTHREAD_ONCE_INIT {0}

typedef unsigned pthread_key_t;
#define PTHREAD_KEYS_MAX 64

typedef struct {
    volatile int state;     // 読んでいる数、-1 = 書いている
} pthread_rwlock_t;

typedef struct {
    int dummy;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER {0}

typedef struct {
    unsigned count;
    volatile unsigned waiting;
    volatile unsigned phase;
} pthread_barrier_t;

typedef struct {
    int dummy;
} pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

typedef volatile int pthread_spinlock_t;

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void *)-1)

int sched_yield(void);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
__attribute__((noreturn)) void pthread_exit(void *retval);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int *old);
int pthread_setcanceltype(int type, int *old);
void pthread_testcancel(void);
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char *name, size_t len);

// シグナル（Linux では signal.h。picolibc の signal.h にないのでここで）
#if __POSIX_VISIBLE
#include <signal.h>
int pthread_kill(pthread_t thread, int sig);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
#endif

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setclock(pthread_condattr_t *attr, int clock);   // CLOCK_REALTIME・CLOCK_MONOTONIC
int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_once(pthread_once_t *once, void (*fn)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rw);
int pthread_rwlock_rdlock(pthread_rwlock_t *rw);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw);
int pthread_rwlock_wrlock(pthread_rwlock_t *rw);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rw);
int pthread_rwlock_unlock(pthread_rwlock_t *rw);

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);

int pthread_spin_init(pthread_spinlock_t *s, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);

#ifdef __cplusplus
}
#endif
