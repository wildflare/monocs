// コア間のロック（lock.h）と、picolibc のロックの関数
// 出典: picolibc libc/misc/lock.c（__retarget_lock_* の仕様。静的なロックは __lock___libc_recursive_mutex だけ）
//
// picolibc は single-thread=false でビルドし、malloc・atexit・環境変数などは 1 つの再帰ロック
// （__libc_recursive_mutex）、ファイル（fopen した FILE）はファイルごとのロックで守る。
// コンソール（stdin・stdout・stderr）にはロックがないので、コアの出力は文字の単位で混ざりうる。
// Cortex-A53 には LSE がないので、ldaxr/stxr（C11 のアトミック。-mno-outline-atomics）で取る。

#include <sys/lock.h>
#include "lock.h"
#include "smp.h"
#include "sysreg.h"
#include "../drivers/uart.h"

struct __lock {
    struct klock k;
};

struct __lock __lock___libc_recursive_mutex = { KLOCK_INIT };

// __retarget_lock_init で配るロック（FILE ごと）。ヒープから取らないのは、
// malloc のロックの中から呼ばれることがあり、後始末で全部をたどる必要があるため
#define POOL_SIZE 64
static struct __lock pool[POOL_SIZE];
static atomic_bool pool_used[POOL_SIZE];

// 後始末でたどる、そのほかの静的なロック（VFS）
extern struct klock vfs_lock, vfs_fc_lock;

// コアごとの「持っている OS のロックの数」。ロックを取るたび・外すたびに、そのコアが書く。
// コアごとに別のキャッシュラインに置く（4 つの int を並べると、3 コアが同じラインを奪い合った）
static struct {
    alignas(CACHE_LINE) volatile int n;
} depth_of[SMP_MAX_CORES];
#define depth(core) (depth_of[core].n)

static uint64_t irq_save(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    return daif;
}

static void irq_restore(uint64_t daif)
{
    write_sysreg(daif, daif);
}

// defer: 持っている間、そのコアの中断を後回しにする（OS・C ライブラリのロック）。
// depth は取れたときに増やす（待っている間は中断できるように。待つ間も後回しにすると、ほかのコアが
// 取り直し続けたときに Ctrl-C が効かなくなる）。取ってから depth を増やすまでは割り込みを止める
static void take(struct klock *l, bool defer)
{
    int me = cpu_id();
    if (l->owner == me) {
        l->count++;
        if (defer)
            depth(me)++;
        return;
    }
    for (;;) {
        uint64_t daif = irq_save();
        if (!atomic_exchange_explicit(&l->word, 1, memory_order_acquire)) {
            l->owner = me;
            l->count = 1;
            if (defer)
                depth(me)++;
            irq_restore(daif);
            return;
        }
        irq_restore(daif);
        while (atomic_load_explicit(&l->word, memory_order_relaxed))
            __asm__ volatile("yield");
    }
}

static void give(struct klock *l, bool defer)
{
    int me = cpu_id();
    if (l->owner != me || l->count <= 0) {
        uart_puts("\n*** lock: release by a core that does not own the lock\n");
        return;
    }
    if (--l->count == 0) {
        l->owner = -1;
        atomic_store_explicit(&l->word, 0, memory_order_release);
    }
    if (!defer)
        return;
    // 外してから depth を減らす（その間に割り込みが来ても、中断は後回しになるだけ）。
    // 後回しにしていた中断（コア 1〜3）。コア 0 の Ctrl-C は次のタイマー割り込みで止まる
    if (--depth(me) == 0 && me != 0)
        smp_check_abort();
}

void abort_defer_begin(void)
{
    depth(cpu_id())++;
}

void abort_defer_end(void)
{
    int me = cpu_id();
    if (depth(me) > 0 && --depth(me) == 0 && me != 0)
        smp_check_abort(); // 後回しにしていた中断（コア 0 の Ctrl-C は次のタイマー割り込みで止まる）
}

void klock_acquire(struct klock *l) { take(l, true); }
void klock_release(struct klock *l) { give(l, true); }

// アプリ用（monocs_mutex_lock）。中断は後回しにしない（外し忘れても Ctrl-C で止められるように）。
// 中断で持ったまま止まっても、ロックはアプリの領域にあり、アプリと一緒に捨てられる
void app_mutex_lock(struct klock *l) { take(l, false); }
void app_mutex_unlock(struct klock *l) { give(l, false); }

int lock_depth(int core)
{
    return depth(core);
}

static void force_release(struct klock *l, int core)
{
    if (l->owner != core)
        return;
    l->count = 0;
    l->owner = -1;
    atomic_store_explicit(&l->word, 0, memory_order_release);
}

void lock_release_all(int core)
{
    if (depth(core) == 0)
        return;
    force_release(&__lock___libc_recursive_mutex.k, core);
    force_release(&vfs_lock, core);
    force_release(&vfs_fc_lock, core);
    for (int i = 0; i < POOL_SIZE; i++)
        force_release(&pool[i].k, core);
    depth(core) = 0;
}

// picolibc のロックの関数（sys/lock.h）。すべて再帰できるロックにする
void __retarget_lock_init(_LOCK_T *lock)
{
    for (int i = 0; i < POOL_SIZE; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&pool_used[i], &expected, true)) {
            pool[i].k = (struct klock)KLOCK_INIT;
            *lock = &pool[i];
            return;
        }
    }
    // 使い切ったら、ライブラリ全体のロックを共用する（遅くなるが正しく動く）
    *lock = &__lock___libc_recursive_mutex;
}

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
    __retarget_lock_init(lock);
}

void __retarget_lock_close(_LOCK_T lock)
{
    if (lock >= pool && lock < pool + POOL_SIZE)
        atomic_store(&pool_used[lock - pool], false);
}

void __retarget_lock_close_recursive(_LOCK_T lock)
{
    __retarget_lock_close(lock);
}

void __retarget_lock_acquire(_LOCK_T lock)
{
    klock_acquire(&lock->k);
}

void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
    klock_acquire(&lock->k);
}

void __retarget_lock_release(_LOCK_T lock)
{
    klock_release(&lock->k);
}

void __retarget_lock_release_recursive(_LOCK_T lock)
{
    klock_release(&lock->k);
}
