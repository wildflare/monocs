// アプリの実行と、シェルへの復帰
// exit()、同期例外、Ctrl-C のどれで終わっても、setjmp した app_call に longjmp で戻る。
// アプリも OS も同じスタック・同じ EL1 で動くので、longjmp だけで復帰できる。
//
// 入れ子（2026-11-14）: アプリの中から system() で別のアプリを動かすと、app_call が入れ子になる。
// 動いているアプリごとに「文脈」（戻る場所、終了の値、atexit の一覧）を app_call のスタックの上に作り、
// cur が今いちばん内側のものを指す。exit・例外・Ctrl-C は、いちばん内側のアプリだけを終わらせる
// （Linux で、system() の間の Ctrl-C が子のプログラムに届くのと同じ）。

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "lock.h"
#include "smp.h"
#include "sysreg.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../libc/signal_os.h"

// atexit で登録された関数（アプリごと。POSIX は 32 個以上を求める）。main から戻ったとき・exit のときに、
// 登録の逆の順に呼ぶ（例外・Ctrl-C で止まったときは呼ばない。_Exit・abort も呼ばない）。
// picolibc の atexit は使わない（カーネルと共有の一覧になり、アプリの終わりに呼ばれなかった）
#define ATEXIT_MAX 32

struct app_ctx {
    jmp_buf jb;                         // 終わったときに戻る場所
    int exit_status;
    void (*atexit_fns[ATEXIT_MAX])(void);
    int atexit_count;
    enum app_result abort_reason;       // 例外・Ctrl-C で止めるときの理由
    struct app_ctx *outer;              // ひとつ外側のアプリ（system() を呼んだもの）
    char name[48];                      // argv[0] の最後の部分（top）
};

static struct app_ctx *volatile cur;    // いちばん内側の、動いているアプリ（なければ NULL）
static volatile int depth;              // 動いているアプリの数（入れ子の深さ）
static volatile bool interrupt_pending;
static volatile enum app_result pending_reason = APP_INTERRUPTED;   // interrupt_pending の理由
static volatile int pending_status;         // APP_EXITED・APP_FAULTED のときの終了の値
static volatile bool pending_atexit;        // 止める前に atexit の関数を呼ぶ（スレッドの中の exit）

int app_depth(void)
{
    return depth;
}

int app_owner(void)
{
    return depth <= 1 ? 0 : -(depth - 1);
}

bool app_is_running(void)
{
    return cur != nullptr;
}

int app_atexit(void (*fn)(void))
{
    struct app_ctx *c = cur;
    if (!c || cpu_id() != 0 || c->atexit_count == ATEXIT_MAX)
        return -1;
    c->atexit_fns[c->atexit_count++] = fn;
    return 0;
}

static void run_atexit(struct app_ctx *c)
{
    while (c->atexit_count > 0)
        c->atexit_fns[--c->atexit_count]();   // 先に数を減らす（呼ばれた関数が exit を呼んでも、同じものを繰り返さない）
}

// アプリの exit（OS API 表の exit）: 登録された関数を呼んでから終わる。_Exit・abort はここを通らない
[[noreturn]] void app_exit_with_handlers(int status)
{
    if (cur && cpu_id() != 0) {
        // スレッド（コア 1〜3）の exit: アプリ全体を終わらせる（Linux と同じ）。atexit はコア 0 が呼ぶ
        app_request_abort(APP_EXITED, status, true);
        smp_exit_job();
    }
    if (cur && cpu_id() == 0)
        run_atexit(cur);
    exit(status);   // picolibc の exit → _exit（os/libc/syscalls.c）→ app_exit
}

// app_call の中の、アプリが終わった後の共通の片付け（ctx を外して、外側のアプリに戻す）
static void leave(struct app_ctx *c)
{
    cur = c->outer;
    depth--;
    smp_abort_level(depth + 1);     // このアプリ（と、その中のアプリ）がほかのコアで動かしている仕事を止める
    interrupt_pending = false;
    if (c->outer)
        abort_defer_begin();        // system() に戻る（また後回しにする）
}

enum app_result app_call(app_main_t entry, int argc, char **argv, int *status)
{
    struct app_ctx c;
    c.exit_status = 0;
    c.atexit_count = 0;
    c.outer = cur;
    c.name[0] = '\0';
    if (argc > 0 && argv[0]) {
        const char *base = strrchr(argv[0], '/');
        snprintf(c.name, sizeof c.name, "%s", base && base[1] ? base + 1 : argv[0]);
        size_t n = strlen(c.name);
        if (n > 2 && !strcmp(c.name + n - 2, ".c"))
            c.name[n - 2] = '\0';      // /bin/top.c → top
    }
    int r = setjmp(c.jb);
    if (r == 0) {
        interrupt_pending = false;
        depth++;
        cur = &c;
        // 入れ子のとき、system() は中断を後回しにしている（シェルの解析・コンパイルの途中で外側のアプリが
        // 止まらないように。os/shell/shell.c の shell_system）。子のアプリの間だけ受け付ける
        if (c.outer)
            abort_defer_end();
        int s = entry(argc, argv);
        run_atexit(&c);
        leave(&c);
        *status = s;
        return APP_RETURNED;
    }
    lock_release_all(0); // 同期例外のときは、ロックを持ったまま止まっていることがある
    leave(&c);
    *status = c.exit_status;
    return (enum app_result)r;
}

// いちばん内側の動いているアプリの名前（top。動いていなければ ""）
void app_current_name(char *buf, size_t size)
{
    struct app_ctx *c = cur;
    snprintf(buf, size, "%s", c ? c->name : "");
}

bool app_exit(int status)
{
    struct app_ctx *c = cur;
    if (!c)
        return false;
    if (cpu_id() != 0) {    // スレッドの中の _exit（atexit は呼ばない）
        app_request_abort(APP_EXITED, status, false);
        smp_exit_job();
    }
    c->exit_status = status;
    longjmp(c->jb, APP_EXITED);
}

// 例外から「戻る」先。ELR_EL1 をここに差し替えて eret する。
// 戻り先アドレスは使わないので、どこから来ても longjmp で app_call に戻る
[[noreturn]] static void app_abort_return(void)
{
    struct app_ctx *c = cur;
    if (c->abort_reason == APP_EXITED || (c->abort_reason == APP_FAULTED && pending_status)) {
        c->exit_status = pending_status;    // スレッドから: 終了の値を渡す
        pending_status = 0;
        if (pending_atexit && c->abort_reason == APP_EXITED) {
            pending_atexit = false;
            run_atexit(c);
        }
    } else {
        c->exit_status = -1;
    }
    longjmp(c->jb, c->abort_reason);
}

[[noreturn]] void app_abort_timeout(void)
{
    cur->abort_reason = APP_TIMEDOUT;
    app_abort_return();
}

void app_abort_from_exception(uint64_t *elr, enum app_result reason)
{
    cur->abort_reason = reason;
    *elr = (uint64_t)app_abort_return;
    // 中断を決めたので、Ctrl-C の要求は消す。残すと、app_abort_return に戻る前に次のタイマー割り込みが
    // 来たとき（実機では ^C の表示に UART で 10ms 以上かかる）、同じ中断を繰り返して先に進めない
    interrupt_pending = false;
    pending_reason = APP_INTERRUPTED;
}

void app_request_abort(enum app_result reason, int status, bool run_atexit_fns)
{
    if (!cur)
        return;
    pending_status = status;
    pending_atexit = run_atexit_fns;
    pending_reason = reason;
    __asm__ volatile("dmb ish" ::: "memory");
    interrupt_pending = true;
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");   // コア 0 が wfe で待っていれば起こす
}

enum app_result app_pending_reason(void)
{
    return pending_reason;
}

bool app_request_interrupt(void)
{
    if (!cur)
        return false;
    // Ctrl-C（raw でないとき）: アプリが SIGINT に関数を登録していれば、止めずに渡す（os/libc/signal.c）。
    // Ctrl-\ はいつも止める
    if (!uart_interrupt_is_quit() && signal_on_interrupt_key())
        return true;
    if (!interrupt_pending)
        pending_reason = APP_INTERRUPTED;
    interrupt_pending = true;
    return true;
}

bool app_interrupt_pending(void)
{
    return cur && interrupt_pending;
}

// ---- timeout（os/shell/shell.c の timeout コマンド）----
// 期限ごとに、その timeout が始めるアプリの深さ（level）を覚える。期限を過ぎて、その深さかそれより内側の
// アプリが動いていれば止める。いちばん内側から 1 つずつ止まり（Ctrl-C と同じ）、外側のアプリが system() から
// 戻って動き続けていれば、次のタイマー割り込みでまた止める。level より外のアプリ（timeout を呼んだもの）は止めない
#define TIMEOUT_MAX 8
static struct {
    uint64_t deadline;
    uint64_t kill_after;    // 期限からこれだけたったら必ず止める（0 = なし）
    int level;
    int sig;
    uint32_t signaled;      // シグナルを届けたアプリの深さ（ビット）
    volatile bool fired;
} timeouts[TIMEOUT_MAX];
static volatile int ntimeouts;

int app_timeout_push(uint64_t deadline_us, int sig, uint64_t kill_after_us)
{
    int n = ntimeouts;
    if (n == TIMEOUT_MAX)
        return -1;
    timeouts[n].deadline = deadline_us;
    timeouts[n].kill_after = kill_after_us;
    timeouts[n].level = depth + 1;
    timeouts[n].sig = sig;
    timeouts[n].signaled = 0;
    timeouts[n].fired = false;
    __asm__ volatile("" ::: "memory");
    ntimeouts = n + 1;
    return n;
}

bool app_timeout_pop(int slot)
{
    if (slot < 0 || slot >= ntimeouts)
        return false;
    bool fired = timeouts[slot].fired;
    ntimeouts = slot;
    return fired;
}

bool app_timeout_due(void)
{
    int n = ntimeouts;
    if (n == 0 || !cur)
        return false;
    uint64_t now = timer_now_us();
    for (int i = 0; i < n; i++) {
        if (now < timeouts[i].deadline || depth < timeouts[i].level)
            continue;
        timeouts[i].fired = true;
        if (timeouts[i].sig == SIGNAL_KILL ||
            (timeouts[i].kill_after && now - timeouts[i].deadline >= timeouts[i].kill_after))
            return true;
        uint32_t bit = 1u << (depth & 31);
        if (timeouts[i].signaled & bit)
            continue;       // このアプリには届けた（関数で受けた・無視した）
        timeouts[i].signaled |= bit;
        if (signal_send_timeout(timeouts[i].sig))
            return true;    // 既定の動き（終わる）: 止める
    }
    return false;
}
