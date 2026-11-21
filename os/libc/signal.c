// アプリのシグナル: signal sigaction raise sigprocmask sigpending sigsuspend pause（2026-11-14）。
// Monocs にプロセスはないので、届くシグナルは次のものだけ:
//   - Ctrl-C（raw でないとき）: アプリが SIGINT に関数を登録していれば、止めずにその関数を呼ぶ。SIG_IGN なら
//     無視する（raw のときの Ctrl-\ は、いつも止める。逃げ道として残す）
//   - SIGWINCH: telnet・ssh から画面の大きさが知らされたとき（os/kernel/contap.c）
//   - raise・kill(getpid(), sig): その場で
//   - SIGALRM: alarm の時刻が来たとき
// 関数を呼ぶのは、アプリが待っているところ（キー入力・sleep・ネットワークの待ち。os/drivers/timer.c の
// cpu_idle と uart.c の uart_wait_input から signal_point）で、カーネルのロックを持っていないとき。割り込みの中
// からは呼ばない（割り込みの経路でアプリの関数を動かすと、戻り方が難しく、止まったときに戻れない）。
// 待たずに計算し続けるアプリには、Ctrl-C を押しても届かない（raw のときの Ctrl-\ か、SIG_DFL に戻すと止まる）。
// 読み込み（read・getchar など）は、関数を呼んだ後も読み続ける（glibc の signal() の既定の SA_RESTART と同じ。
// SA_RESTART を付けなくても同じ）。nanosleep・sleep は Linux と同じく、関数を呼んだら残りを返して早く戻る。
// 既定の動き（SIG_DFL）は Linux と同じ: SIGCHLD・SIGURG・SIGWINCH・SIGCONT は無視、止める（SIGSTOP など）ものも
// 無視、ほかはアプリを終わらせる（終了の値は 128 + シグナルの番号。atexit は呼ばない）。
// アプリごとに表を持ち、アプリを始めるときは SIG_DFL から（Linux の新しいプロセスと同じ）。system() の子が
// 終わったら、親の表に戻す（posix_app_begin・end）。
// picolibc の signal・raise・sigprocmask より先にリンクされる（wcwidth.c と同じ）。

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include "signal_os.h"
#include "../kernel/app.h"
#include "../kernel/lock.h"
#include "../kernel/sysreg.h"
#include "../drivers/uart.h"
#include "../drivers/timer.h"

static struct sigaction acts[NSIG];
static sigset_t blocked;
static _Atomic uint32_t pending;
static _Atomic uint32_t delivered;
static _Atomic uint32_t delivered_norestart;
static uint64_t alarm_at;       // alarm の時刻（timer_now_us。0 = なし）
static uint64_t alarm_interval; // setitimer の繰り返しの間（マイクロ秒。0 = 1 回だけ）    // そのうち SA_RESTART のない関数（待っていた操作は EINTR で戻る）

static bool in_app(void)
{
    return cpu_id() == 0 && app_is_running();
}

typedef struct {
    struct sigaction act[NSIG];
    sigset_t mask;
    uint32_t pending;
    uint64_t alarm_at, alarm_interval;
} Saved;
static_assert(sizeof(Saved) <= sizeof(SignalState), "SignalState が小さい");

void signal_app_begin(SignalState *outer)
{
    Saved *s = (Saved *)outer->bytes;
    memcpy(s->act, acts, sizeof acts);
    s->mask = blocked;
    s->pending = atomic_exchange(&pending, 0);
    s->alarm_at = alarm_at;
    s->alarm_interval = alarm_interval;
    memset(acts, 0, sizeof acts);   // SIG_DFL
    blocked = 0;
    alarm_at = alarm_interval = 0;
}

void signal_app_end(const SignalState *outer)
{
    const Saved *s = (const Saved *)outer->bytes;
    memcpy(acts, s->act, sizeof acts);
    blocked = s->mask;
    atomic_store(&pending, s->pending);
    alarm_at = s->alarm_at;
    alarm_interval = s->alarm_interval;
}

static bool valid(int sig)
{
    return sig > 0 && sig < NSIG;
}

// 既定の動きで無視するもの
static bool default_ignores(int sig)
{
    switch (sig) {
    case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return true;
    default:
        return false;
    }
}

// sig の関数を呼ぶ（SIG_DFL なら既定の動き）
static void deliver(int sig)
{
    struct sigaction *a = &acts[sig];
    void (*h)(int) = a->sa_handler;
    if (h == SIG_IGN)
        return;
    if (h == SIG_DFL) {
        if (!default_ignores(sig))
            app_exit(128 + sig);    // Linux でシグナルで終わったとき、シェルの $? は 128 + 番号
        return;
    }
    sigset_t old = blocked;
    blocked |= a->sa_mask;
    if (!(a->sa_flags & SA_NODEFER))
        blocked |= (sigset_t)1 << sig;
    int flags = a->sa_flags;
    void (*sa)(int, siginfo_t *, void *) = a->sa_sigaction;
    if (flags & SA_RESETHAND)
        a->sa_handler = SIG_DFL;
    atomic_fetch_add(&delivered, 1);
    if (!(flags & SA_RESTART))
        atomic_fetch_add(&delivered_norestart, 1);
    if (flags & SA_SIGINFO) {
        siginfo_t info = {.si_signo = sig, .si_pid = getpid()};
        sa(sig, &info, nullptr);
    } else {
        h(sig);
    }
    blocked = old;
}

void signal_point(void)
{
    if ((!atomic_load_explicit(&pending, memory_order_relaxed) && !alarm_at) || !in_app() || lock_depth(0) != 0)
        return;
    if (alarm_at && timer_now_us() >= alarm_at) {   // alarm の時刻が来た（SIG_DFL ならアプリを終わらせる）
        alarm_at = alarm_interval ? timer_now_us() + alarm_interval : 0;  // setitimer の繰り返し
        atomic_fetch_or(&pending, (uint32_t)1 << SIGALRM);
    }
    for (int sig = 1; sig < NSIG; sig++) {
        uint32_t bit = (uint32_t)1 << sig;
        if ((atomic_load(&pending) & bit) && !(blocked & bit)) {
            atomic_fetch_and(&pending, ~bit);
            deliver(sig);
        }
    }
}

uint32_t signal_delivered(void)
{
    return atomic_load(&delivered);
}

uint32_t signal_delivered_norestart(void)
{
    return atomic_load(&delivered_norestart);
}

bool signal_on_interrupt_key(void)
{
    void (*h)(int) = acts[SIGINT].sa_handler;
    if (h == SIG_DFL)
        return false;
    if (h != SIG_IGN)
        atomic_fetch_or(&pending, (uint32_t)1 << SIGINT);
    return true;
}

void signal_post(int sig)
{
    if (!valid(sig))
        return;
    void (*h)(int) = acts[sig].sa_handler;
    if (h != SIG_DFL && h != SIG_IGN)
        atomic_fetch_or(&pending, (uint32_t)1 << sig);
}

void signal_post_winch(void)
{
    signal_post(SIGWINCH);
}

static_assert(SIGNAL_KILL == SIGKILL, "SIGNAL_KILL");

bool signal_send_timeout(int sig)
{
    if (!valid(sig))
        sig = SIGTERM;
    void (*h)(int) = acts[sig].sa_handler;
    if (h == SIG_IGN)
        return false;
    if (h == SIG_DFL)
        return !default_ignores(sig);
    atomic_fetch_or(&pending, (uint32_t)1 << sig);
    return false;
}

// ---- アプリから呼ぶもの（OS API 表） ----

int sigaction(int sig, const struct sigaction *restrict act, struct sigaction *restrict old)
{
    if (!valid(sig) || (act && (sig == SIGKILL || sig == SIGSTOP))) {
        errno = EINVAL;
        return -1;
    }
    if (old)
        *old = acts[sig];
    if (act) {
        acts[sig] = *act;
        acts[sig].sa_mask &= ~(((sigset_t)1 << SIGKILL) | ((sigset_t)1 << SIGSTOP));
    }
    return 0;
}

_sig_func_ptr signal(int sig, _sig_func_ptr handler)
{
    // glibc と同じ BSD の意味: 呼ばれた後も関数は残り、読み込みは続ける（SA_RESTART）
    struct sigaction act = {.sa_handler = handler, .sa_flags = SA_RESTART}, old;
    if (sigaction(sig, &act, &old) != 0)
        return SIG_ERR;
    return old.sa_handler;
}

int raise(int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    if (!in_app())
        return 0;   // カーネル・サービス（abort は、この後 _exit で終わる）
    uint32_t bit = (uint32_t)1 << sig;
    if (blocked & bit) {
        atomic_fetch_or(&pending, bit);
        return 0;
    }
    deliver(sig);
    return 0;
}

int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict old)
{
    if (old)
        *old = blocked;
    if (set) {
        sigset_t s = *set & ~(((sigset_t)1 << SIGKILL) | ((sigset_t)1 << SIGSTOP));
        switch (how) {
        case SIG_BLOCK:
            blocked |= s;
            break;
        case SIG_UNBLOCK:
            blocked &= ~s;
            break;
        case SIG_SETMASK:
            blocked = s;
            break;
        default:
            errno = EINVAL;
            return -1;
        }
        signal_point();     // 止めるのをやめたもので、届いていたものを渡す（Linux は戻る前に渡す）
    }
    return 0;
}

int sigpending(sigset_t *set)
{
    *set = atomic_load(&pending) & blocked;
    return 0;
}

// 関数が 1 回呼ばれるまで待つ（-1、EINTR）
int sigsuspend(const sigset_t *mask)
{
    sigset_t old = blocked;
    blocked = *mask & ~(((sigset_t)1 << SIGKILL) | ((sigset_t)1 << SIGSTOP));
    uint32_t start = signal_delivered();
    while (signal_delivered() == start) {
        signal_point();
        if (signal_delivered() != start)
            break;
        uart_wait_input();  // 次の割り込み（キー・タイマー）まで眠る
    }
    blocked = old;
    errno = EINTR;
    return -1;
}

int pause(void)
{
    return sigsuspend(&blocked);
}

// alarm: sec 秒後に SIGALRM（0 なら取り消し）。前の alarm の残りの秒（切り上げ）を返す。ほかのシグナルと同じく、
// アプリが待っているところで届く（計算し続けている間は、次に待つまで遅れる）
unsigned alarm(unsigned sec)
{
    uint64_t now = timer_now_us();
    unsigned left = alarm_at > now ? (unsigned)((alarm_at - now + 999999) / 1000000) : (alarm_at ? 1 : 0);
    alarm_at = sec ? now + (uint64_t)sec * 1000000 : 0;
    alarm_interval = 0;
    return left;
}

// setitimer・getitimer（ITIMER_REAL だけ。alarm と同じ時計で、届くのも同じくアプリが待っているところ。
// 2026-11-20、vim の正規表現の時間切れ）。ITIMER_VIRTUAL・ITIMER_PROF（CPU 時間）は EINVAL
static void itimer_now(struct itimerval *v)
{
    uint64_t now = timer_now_us();
    uint64_t left = alarm_at > now ? alarm_at - now : (alarm_at ? 1 : 0);
    v->it_value.tv_sec = (time_t)(left / 1000000);
    v->it_value.tv_usec = (suseconds_t)(left % 1000000);
    v->it_interval.tv_sec = (time_t)(alarm_interval / 1000000);
    v->it_interval.tv_usec = (suseconds_t)(alarm_interval % 1000000);
}

int getitimer(int which, struct itimerval *v)
{
    if (which != ITIMER_REAL) {
        errno = EINVAL;
        return -1;
    }
    itimer_now(v);
    return 0;
}

int setitimer(int which, const struct itimerval *restrict v, struct itimerval *restrict old)
{
    if (which != ITIMER_REAL || v->it_value.tv_usec < 0 || v->it_value.tv_usec >= 1000000 ||
        v->it_interval.tv_usec < 0 || v->it_interval.tv_usec >= 1000000) {
        errno = EINVAL;
        return -1;
    }
    if (old)
        itimer_now(old);
    uint64_t value = (uint64_t)v->it_value.tv_sec * 1000000 + (uint64_t)v->it_value.tv_usec;
    alarm_interval = (uint64_t)v->it_interval.tv_sec * 1000000 + (uint64_t)v->it_interval.tv_usec;
    alarm_at = value ? timer_now_us() + value : 0;
    if (!value)
        alarm_interval = 0;
    return 0;
}
