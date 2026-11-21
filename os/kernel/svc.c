// サービス: 裏で動き続けるプログラム（HTTP サーバーなど）。カーネルの外に置き、コア 3 で動かす
// （ユーザーと合意、2026-11-06。アプリ用のコアとサービス用のコアを分ける。非対称マルチプロセッシング）。
//
// サービスは C のファイルで、main の代わりに次を持つ（例: services/httpd.c）:
//   int  service_start(int argc, char **argv);  // 始めるとき 1 回（0 で成功）。なくてもよい
//   int  service_poll(void);                     // 繰り返し呼ぶ。待たずに戻り、仕事をしたら 0 以外を返す
//   void service_stop(void);                     // 止めるとき（なくてもよい）
// アプリと同じ OS API（ファイル、TCP、printf など）を使える。
//
// コア 3 は、サービスが 1 つでもある間はサービス用に取っておく（smp_reserve。アプリには 3 コアに見え、
// アプリが終わっても止めない）。コア 3 では、すべてのサービスの service_poll を順に呼び続ける（協調型）。
// どのサービスも仕事をしなかったら wfe で眠る（コア 0 の割り込みのたびに sev で起こす。os/drivers/irq.c）。
// サービスが開いたファイルと TCP の接続は持ち主（svc_owner）を付け、アプリが終わっても閉じない。
// サービスの中で例外が起きたら、コア 3 は待機に戻るので、すべてのサービスを止めて後始末する（svc_check）。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "klog.h"
#include "../net/udp_api.h"
#include "svc.h"
#include "../libc/posix.h"
#include "../libc/pipe_os.h"
#include "app.h"
#include "../libc/socket_os.h"
#include "../libc/heap.h"
#include "smp.h"
#include "contap.h"
#include "sysreg.h"
#include "../drivers/timer.h"
#include "../fs/vfs.h"
#include "../net/tcp_api.h"
#include "../slimcc/run.h"

typedef int (*start_fn)(int argc, char **argv);
typedef int (*poll_fn)(void);
typedef void (*stop_fn)(void);

static struct {
    bool used;
    volatile bool active;
    char name[32];
    char path[128];
    SlimccImage *img;
    start_fn start;
    poll_fn poll;
    stop_fn stop;
    int argc;
    char **argv;
    uint64_t busy_ticks;    // service_poll の中にいた時間（cntpct のカウント。top）
    uint64_t polls, work_polls;     // 呼んだ回数、そのうち仕事をした回数（top）
} svcs[SVC_MAX];

// シェル（コア 0）からサービスのコアへの頼みごと
enum { CMD_NONE, CMD_START, CMD_STOP };
static volatile int cmd;
static volatile int cmd_idx;
static volatile int cmd_result;
static volatile bool cmd_done;
static volatile bool runner_on;     // サービスのコアで runner が動いている
static volatile int current;        // サービスのコアで今動いているサービスの番号 + 1
static uint64_t sleeps;             // 眠った回数（top）

static void dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }
static void sev(void) { __asm__ volatile("dsb sy\n\tsev" ::: "memory"); }

int svc_owner(void)
{
    return runner_on && cpu_id() == SVC_CORE ? current : app_owner();
}

// サービスのコアで動き続ける
// サービスのコアは、タイマーのイベントストリーム（os/drivers/timer.c の cpu_event_stream）で、仕事がなくて眠っている
// 間も約 0.85ms ごとに起き、サービスに相手からのデータを見させる。これがないと、次の合図（コア 0 の 10ms ごとの
// タイマー）まで返事が遅れ、ssh の 1 行の往復が 66 → 81ms になった

static void runner(void *arg)
{
    (void)arg;
    malloc_kernel_begin();  // サービスの確保は、アプリの実行中でもカーネル用のヒープから（os/libc/malloc.c）
    cpu_event_stream(true);
    for (;;) {
        if (cmd != CMD_NONE) {
            dmb();
            int i = cmd_idx;
            current = i + 1;
            if (cmd == CMD_START) {
                int r = svcs[i].start ? svcs[i].start(svcs[i].argc, svcs[i].argv) : 0;
                svcs[i].active = r == 0;
                cmd_result = r;
            } else {
                if (svcs[i].stop)
                    svcs[i].stop();
                svcs[i].active = false;
                cmd_result = 0;
            }
            current = 0;
            dmb();
            cmd = CMD_NONE;
            cmd_done = true;
            sev();
        }
        bool busy = false;
        int n = 0;
        for (int i = 0; i < SVC_MAX; i++)
            if (svcs[i].active) {
                n++;
                current = i + 1;
                uint64_t t0 = read_sysreg(cntpct_el0);
                svcs[i].polls++;
                if (svcs[i].poll()) {
                    busy = true;
                    svcs[i].work_polls++;
                }
                svcs[i].busy_ticks += read_sysreg(cntpct_el0) - t0;
                current = 0;
            }
        if (n == 0 && cmd == CMD_NONE) {
            cpu_event_stream(false);
            return;     // サービスがなくなった: コアを返す
        }
        if (!busy && cmd == CMD_NONE) {
            uint64_t t0 = read_sysreg(cntpct_el0);
            sleeps++;
            // 溜まったイベントを消してから眠る（sevl で立てて wfe で 1 回消し、次の wfe で眠る）。Cortex-A53 では、
            // アトミック操作（ldaxr・stlxr。サービスの中のロックの取り外しなど）の後の wfe はすぐ戻り（実機で確かめた。
            // tools/perf/wfe.c）、サービスのコアは一度も眠れていなかった（2026-11-16、top で見つけた。1 秒に 36 万回
            // 起き、コア 3 はいつも 99%）。消した直後に届いた合図は取りこぼすが、イベントストリームで 1ms 以内に起きる
            __asm__ volatile("sevl\n\twfe\n\twfe" ::: "memory");
            smp_add_idle(SVC_CORE, read_sysreg(cntpct_el0) - t0);
        }
    }
}

static void free_service(int i)
{
    vfs_close_owner(i + 1);
    socket_close_owner(i + 1);
    pipe_close_owner(i + 1);
    mman_close_owner(i + 1);
    tcp_api_close_owner(i + 1);
    udp_api_close_owner(i + 1);
    if (svcs[i].img)
        slimcc_unload_program(svcs[i].img);
    if (svcs[i].argv) {
        for (int k = 0; k < svcs[i].argc; k++)
            free(svcs[i].argv[k]);
        free(svcs[i].argv);
    }
    memset(&svcs[i], 0, sizeof(svcs[i]));
}

// サービスのコアが止まっていたら後始末する。例外で止まっていたら true
static bool reap(void)
{
    if (!runner_on || smp_busy(SVC_CORE))
        return false;
    bool ok = smp_wait(SVC_CORE);
    runner_on = false;
    smp_reserve(SVC_CORE, false);
    if (ok)
        return false;
    printf("svc: the service core stopped by an exception; all services are stopped\n");
    klog_printed("svc: the service core stopped by an exception; all services are stopped");
    contap_close();     // telnet などがつないでいたコンソールの写しも閉じる
    for (int i = 0; i < SVC_MAX; i++)
        if (svcs[i].used)
            free_service(i);
    cmd = CMD_NONE;
    return true;
}

void svc_check(void)
{
    if (runner_on && !smp_busy(SVC_CORE))
        reap();
}

static bool start_runner(void)
{
    if (runner_on)
        return true;
    if (smp_cores() < SVC_CORE + 1 || smp_busy(SVC_CORE))
        return false;   // コア 3 がないか、アプリが使っている
    smp_reserve(SVC_CORE, true);
    runner_on = true;
    if (!smp_run_reserved(SVC_CORE, runner, nullptr)) {
        runner_on = false;
        smp_reserve(SVC_CORE, false);
        return false;
    }
    return true;
}

// サービスのコアに頼み、終わるのを待つ。結果（失敗・例外なら -1）
static int post(int c, int i)
{
    cmd_idx = i;
    cmd_done = false;
    dmb();
    cmd = c;
    sev();
    for (;;) {
        if (cmd_done)
            return cmd_result;
        if (!smp_busy(SVC_CORE)) {
            // runner が（サービスがなくなって）戻ったか、例外で止まった
            if (reap())
                return -1;
            if (!start_runner()) {
                cmd = CMD_NONE;
                return -1;
            }
        }
    }
}

static char *copy_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    return p ? memcpy(p, s, n) : nullptr;
}

static const char *base_name(const char *path, char *buf, size_t size)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(buf, size, "%s", b);
    char *dot = strrchr(buf, '.');
    if (dot && !strcmp(dot, ".c"))
        *dot = '\0';
    return buf;
}

static int find(const char *name)
{
    for (int i = 0; i < SVC_MAX; i++)
        if (svcs[i].used && !strcmp(svcs[i].name, name))
            return i;
    return -1;
}

int svc_start(const char *path, int argc, char **argv)
{
    svc_check();
    char name[32];
    base_name(path, name, sizeof(name));
    if (find(name) >= 0) {
        printf("svc: %s is already running\n", name);
        return -1;
    }
    int i = 0;
    while (i < SVC_MAX && svcs[i].used)
        i++;
    if (i == SVC_MAX) {
        printf("svc: too many services\n");
        return -1;
    }
    SlimccImage *img = slimcc_load_program(path);  // ソース（キャッシュあり）か実行ファイル
    if (!img) {
        klog_printed("svc: cannot load %s (compile error or no such file)", path);
        return -1;
    }
    poll_fn poll = (poll_fn)slimcc_program_symbol(img, "service_poll");
    if (!poll) {
        printf("svc: %s: service_poll() がありません\n", path);
        slimcc_unload_program(img);
        return -1;
    }
    svcs[i].used = true;
    snprintf(svcs[i].name, sizeof(svcs[i].name), "%s", name);
    snprintf(svcs[i].path, sizeof(svcs[i].path), "%s", path);
    svcs[i].img = img;
    svcs[i].start = (start_fn)slimcc_program_symbol(img, "service_start");
    svcs[i].poll = poll;
    svcs[i].stop = (stop_fn)slimcc_program_symbol(img, "service_stop");
    // 引数は、サービスが動いている間とっておく
    svcs[i].argv = calloc(argc + 1, sizeof(char *));
    svcs[i].argc = argc;
    for (int k = 0; k < argc; k++)
        svcs[i].argv[k] = copy_string(argv[k]);
    if (!start_runner()) {
        printf("svc: core %d is not available\n", SVC_CORE);
        free_service(i);
        return -1;
    }
    int r = post(CMD_START, i);
    if (r != 0) {
        if (svcs[i].used) {     // 例外で止まったときは、もう後始末してある
            printf("svc: %s: service_start failed (%d)\n", name, r);
            klog_printed("svc: %s: service_start failed (%d)", name, r);
            free_service(i);
        }
        svc_check();
        return -1;
    }
    klog_printf("svc: started %s", path);
    return 0;
}

int svc_stop(const char *name)
{
    svc_check();
    int i = find(name);
    if (i < 0) {
        printf("svc: %s is not running\n", name);
        return -1;
    }
    post(CMD_STOP, i);
    klog_printf("svc: stopped %s", name);
    if (svcs[i].used)
        free_service(i);
    // 最後のサービスなら、runner が戻ってコアが空くのを待つ
    for (int k = 0; k < 1000000 && runner_on && smp_busy(SVC_CORE); k++)
        sev();
    svc_check();
    return 0;
}

// top（os/kernel/sysstat.c）: i 番目の枠のサービスの名前と、service_poll の中にいた時間。枠が空なら false
bool svc_stat(int i, char *name, size_t size, uint64_t *busy_ticks, uint64_t *polls, uint64_t *work_polls)
{
    if (i < 0 || i >= SVC_MAX || !svcs[i].used)
        return false;
    snprintf(name, size, "%s", svcs[i].name);
    *busy_ticks = svcs[i].busy_ticks;
    *polls = svcs[i].polls;
    *work_polls = svcs[i].work_polls;
    return true;
}

uint64_t svc_sleeps(void)
{
    return sleeps;
}

void svc_list(void)
{
    svc_check();
    int n = 0;
    for (int i = 0; i < SVC_MAX; i++)
        if (svcs[i].used) {
            printf("  %-12s %s%s\n", svcs[i].name, svcs[i].path, svcs[i].active ? "" : " (stopped)");
            n++;
        }
    if (!n)
        printf("  (no services)\n");
    printf("app cores: %d%s\n", smp_cores(), runner_on ? " (core 3 runs the services)" : "");
}

void svc_autostart(void)
{
    FILE *f = fopen("/etc/services.txt", "r");
    if (!f)
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *argv[16];
        int argc = 0;
        for (char *t = strtok(line, " \t\r\n"); t && argc < 15; t = strtok(nullptr, " \t\r\n"))
            argv[argc++] = t;
        if (argc == 0 || argv[0][0] == '#')
            continue;
        argv[argc] = nullptr;
        printf("svc: starting %s\n", argv[0]);
        svc_start(argv[0], argc, argv);
    }
    fclose(f);
}
