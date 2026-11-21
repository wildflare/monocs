// picolibc にない（または Monocs では意味の違う）POSIX の関数。アプリには OS API 表（os/api/api.def）で渡す。
// 端末のエディタ（curses のアプリ）などを、Linux 向けのソースのまま動かすため。
//
// Monocs にはプロセスがない（アプリは 1 つずつ、カーネルと同じ空間で動く）ので:
//   getpid: アプリを動かすたびに変わる番号（起動したときのタイマーの値から作る）。動いている間は同じ
//   kill:   今のアプリの番号に信号 0（生きているかの確認）なら成功、ほかは ESRCH・EPERM
//   popen:  一時ファイルと system()（下の popen の説明）
// picolibc の getpid（いつも 1 を返す）・popen（fork と execve を使う）より先にリンクされる（ldtoa.c と同じ）。

// カーネルは -std=c23（ISO）でビルドするので、POSIX の宣言（PATH_MAX、clockid_t など）を見えるようにする
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utime.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <unistd.h>
#include "../kernel/clock.h"
#include "posix.h"
#include "env.h"
#include "signal_os.h"
#include "heap.h"
#include "../kernel/app.h"
#include "../shell/shell.h"
#include "../drivers/timer.h"
#include "../kernel/sysreg.h"
#include "../kernel/smp.h"
#include "../fs/vfs.h"
#include <malloc.h>
// アプリ用のヘッダー（Linux と同じ形。picolibc のものを置き換える）を、カーネルも同じものを読むようにパスで
#include "../api/include/sys/resource.h"
#include "../api/include/dlfcn.h"

// ---- プロセスの番号 ----

static pid_t app_pid = 1;

// アプリの CPU 時間（getrusage）。アプリが使えるコア（コア 0〜2。起きていないコア・サービスのコアは除く）が
// 眠っていなかった時間の、起動からの合計。シェルの time の user と同じ数え方（os/shell/shell.c）
static uint64_t app_cpu_base;
static uint64_t cpu_busy_us(void);
static mode_t app_umask = 022;  // umask（下の「プロセス」）

// getopt の状態（picolibc の libc/stdlib/getopt.c。optind が 0 以下なら、次の getopt が初めから読み直す）
extern char *optarg;
extern int optind, opterr, optopt;

// アプリを始める: 外側（シェル、または system() を呼んだアプリ）の状態を控えて、新しいプロセスと同じ状態にする
static void locale_names_reset(const char *name);

void posix_app_begin(PosixApp *outer)
{
    outer->pid = app_pid;
    outer->optarg = optarg;
    outer->optind = optind;
    outer->opterr = opterr;
    outer->optopt = optopt;
    outer->locale = setlocale(LC_ALL, nullptr);
    outer->cpu_base = app_cpu_base;
    app_cpu_base = cpu_busy_us();
    outer->umask = app_umask;
    app_umask = 022;
    signal_app_begin(&outer->sig);  // シグナルの関数は SIG_DFL から（os/libc/signal.c）
    setlocale(LC_ALL, "C");     // Linux の新しいプロセスと同じく「C」から（前のアプリの setlocale は残さない）
    locale_names_reset("C");
    // 31 ビットの正の数。0・1 は避ける（1 は picolibc の getpid が返していた値）
    uint64_t t = read_sysreg(cntpct_el0);
    t ^= t >> 17;
    t *= 0x9E3779B97F4A7C15ULL;
    app_pid = (pid_t)(t >> 33) | 2;
    // getopt: optind を 0 にして（picolibc の「初めから」）、空の引数で 1 回呼ぶと、内部の状態も初めに戻り、
    // optind は 1 になる（Linux の新しいプロセスと同じ）
    optind = 0;
    getopt(0, nullptr, "");
    optarg = nullptr;
    opterr = 1;
    optopt = '?';
    env_app_begin();
}

// アプリが終わった: 環境変数（os/libc/env.c）と、外側の状態に戻す
static void popen_app_end(void);
static void tmpfile_app_end(void);

void posix_app_end(const PosixApp *outer)
{
    popen_app_end();
    tmpfile_app_end();
    env_app_end();
    signal_app_end(&outer->sig);
    app_pid = outer->pid;
    app_cpu_base = outer->cpu_base;
    app_umask = outer->umask;
    optarg = outer->optarg;
    optind = outer->optind;
    opterr = outer->opterr;
    optopt = outer->optopt;
    if (outer->locale) {
        setlocale(LC_ALL, outer->locale);
        locale_names_reset(outer->locale);
    }
}

// LC_CTYPE と LC_ALL のほかの分類の名前（app_setlocale）。アプリを始めるとき・終わったときに全体の名前にそろえる
#define LOCALE_CATS 7       // LC_ALL（0）〜 LC_MESSAGES（6）
static char cat_names[LOCALE_CATS][32];

static void locale_names_reset(const char *name)
{
    for (int i = 0; i < LOCALE_CATS; i++)
        snprintf(cat_names[i], sizeof cat_names[0], "%s", name ? name : "C");
}

static bool locale_name_ok(const char *name)
{
    if (!strcmp(name, "C") || !strcmp(name, "POSIX"))
        return true;
    const char *dot = strchr(name, '.');
    if (!dot)
        return false;
    // 文字の種類（. の後、@ の前）が UTF-8・utf8（大文字・小文字は問わない）
    char cs[8];
    size_t n = strcspn(dot + 1, "@");
    if (n >= sizeof cs)
        return false;
    for (size_t i = 0; i < n; i++)
        cs[i] = (char)tolower((unsigned char)dot[1 + i]);
    cs[n] = '\0';
    return !strcmp(cs, "utf-8") || !strcmp(cs, "utf8");
}

// setlocale（アプリから。OS API 表）: picolibc の setlocale は "" をいつも「C」にするので、Linux（POSIX）と同じく
// 環境変数 LC_ALL、LC_CTYPE（LC_ALL・LC_CTYPE を選ぶとき）、LANG の順に見る。Monocs の既定は LANG=C.UTF-8
// （os/libc/syscalls.c）。picolibc が区別するのは文字の種類（LC_CTYPE）だけで、ほかの分類は名前を受け取るだけ
char *app_setlocale(int category, const char *name)
{
    if (name && !*name) {
        const char *v = getenv("LC_ALL");
        if ((!v || !*v) && (category == LC_ALL || category == LC_CTYPE))
            v = getenv("LC_CTYPE");
        if (!v || !*v)
            v = getenv("LANG");
        name = v && *v ? v : "C";
    }
    // 名前を受け付けるのは、Linux の glibc にいつもあるもの（C・POSIX）と、文字の種類が UTF-8 のもの
    // （C.UTF-8 と同じに扱う: 小数点・日付の形などは C のまま）だけ。ほかは Linux でその言語のロケールを入れて
    // いないときと同じく NULL（picolibc の setlocale はどんな名前でも受け付け、Lua のテストが pt_BR を受け付けたと
    // 見て「3,4」を数として読めず止まった。2026-11-20）
    if (name && !locale_name_ok(name)) {
        errno = ENOENT;
        return nullptr;
    }
    // picolibc が区別するのは LC_CTYPE だけで、ほかの分類を聞くと全体の名前を返す。glibc と同じく、分類ごとに
    // 最後に選んだ名前を返す（setlocale(LC_NUMERIC, "C") は "C"）
    if (category != LC_ALL && category != LC_CTYPE && category > 0 && category < LOCALE_CATS) {
        if (!setlocale(category, name ? name : nullptr))
            return nullptr;
        if (name)   // glibc と同じく、POSIX は C と同じ名前にする
            snprintf(cat_names[category], sizeof cat_names[0], "%s", strcmp(name, "POSIX") ? name : "C");
        return cat_names[category][0] ? cat_names[category] : (char *)"C";
    }
    char *r = setlocale(category, name);
    if (r && name && category == LC_ALL)
        locale_names_reset(r);
    return r;
}

pid_t getpid(void)
{
    return app_pid;
}

// kill: 今のアプリ（getpid の番号、0 は自分の組）にだけ送れる。0 は生きているかの確認。ほかは raise と同じ
// （os/libc/signal.c）
int kill(pid_t pid, int sig)
{
    if (pid != app_pid && pid != 0) {
        errno = ESRCH;
        return -1;
    }
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return sig == 0 ? 0 : raise(sig);
}

// popen・pclose（2026-11-14）: Monocs のパイプ（a | b）と同じく、一時ファイルを使う（同時には動かない）。
//   "r": その場でコマンドを最後まで動かして、標準出力を一時ファイルに書き、それを読む FILE を返す
//   "w": 一時ファイルに書く FILE を返し、pclose のときに、それを標準入力にしてコマンドを動かす
// pclose は system() と同じ形の終了状態。一時ファイルは /.popenN（入れ子の段とで別の名前）。アプリが pclose せずに
// 終わったら、一時ファイルを消す（posix_app_end）。コア 0 のアプリからだけ（ほかは NULL、ENOSYS）
#define POPEN_MAX 8
static struct {
    FILE *f;
    int depth;          // popen したアプリの入れ子の段（app_depth）
    bool write;
    int status;         // "r" のときの終了状態
    char *cmd;          // "w" のときに pclose で動かすコマンド（カーネル用のヒープ）
    char path[32];
} pops[POPEN_MAX];

FILE *popen(const char *command, const char *mode)
{
    if (!command || !mode || (mode[0] != 'r' && mode[0] != 'w')) {
        errno = EINVAL;
        return nullptr;
    }
    if (cpu_id() != 0 || !app_is_running()) {
        errno = ENOSYS;
        return nullptr;
    }
    int i = 0;
    while (i < POPEN_MAX && pops[i].f)
        i++;
    if (i == POPEN_MAX) {
        errno = EMFILE;
        return nullptr;
    }
    snprintf(pops[i].path, sizeof pops[i].path, "/tmp/.popen%d", i + POPEN_MAX * app_depth());
    bool w = mode[0] == 'w';
    FILE *f;
    if (w) {
        malloc_kernel_begin();
        char *cmd = strdup(command);
        malloc_kernel_end();
        if (!cmd || !(f = fopen(pops[i].path, "w"))) {
            free(cmd);
            return nullptr;
        }
        pops[i].cmd = cmd;
        pops[i].status = 0;
    } else {
        pops[i].status = shell_system_redirect(command, nullptr, pops[i].path);
        f = fopen(pops[i].path, "r");
        if (!f) {
            remove(pops[i].path);
            return nullptr;
        }
        pops[i].cmd = nullptr;
    }
    pops[i].f = f;
    pops[i].depth = app_depth();
    pops[i].write = w;
    return f;
}

int pclose(FILE *stream)
{
    int i = 0;
    while (i < POPEN_MAX && (!stream || pops[i].f != stream))
        i++;
    if (i == POPEN_MAX) {
        errno = ECHILD;
        return -1;
    }
    pops[i].f = nullptr;
    fclose(stream);
    int status = pops[i].status;
    if (pops[i].write)
        status = shell_system_redirect(pops[i].cmd, pops[i].path, nullptr);
    free(pops[i].cmd);
    pops[i].cmd = nullptr;
    remove(pops[i].path);
    return status;
}

// アプリが pclose せずに終わった: 一時ファイルを消す（FILE はアプリの終わりに閉じている）
static void popen_app_end(void)
{
    int depth = app_depth() + 1;    // 終わったアプリの段（app_call はもう抜けている）
    for (int i = 0; i < POPEN_MAX; i++)
        if (pops[i].f && pops[i].depth >= depth) {
            pops[i].f = nullptr;
            free(pops[i].cmd);
            pops[i].cmd = nullptr;
            remove(pops[i].path);
        }
}

// fflush（アプリから。OS API の表）: fflush(NULL) は、picolibc のこの設定（__STDIO_EXIT_FLUSH なし）では NULL を
// FILE として錠前を取りに行って落ちる（2026-11-16、Lua の io.popen の前の fflush(NULL) で見つけた）。Linux と同じく
// 書き出しの途中のものを書き出す: 標準出力と標準エラー（picolibc には開いた FILE の一覧がないので、fopen した
// ファイルは書き出さない。popen・system の前に要るのは画面に出すもの）
int app_fflush(FILE *f)
{
    if (f)
        return fflush(f);
    fflush(stdout);
    fflush(stderr);
    return 0;
}

// ---- 一時ファイル（tmpnam・tmpfile。2026-11-16。Lua の os.tmpname・io.tmpfile） ----
// Linux と同じく /tmp に置く（なければ作る）。名前は /tmp/tmpNNNNNN（L_tmpnam は 20。アプリには run.c が
// __L_tmpnam=20 を渡す）。FAT は開いたままのファイルを消せないので、tmpfile のファイルはアプリの終わりに消す
// （popen の一時ファイルと同じ）。tmpnam で名前を取っただけのものは、アプリが消す（Linux と同じ）

#define TMP_DIR     "/tmp"
#define TMPFILE_MAX 16

static unsigned tmp_seq;
static struct {
    char path[24];
    int depth;          // tmpfile を呼んだアプリの入れ子の段
} tmps[TMPFILE_MAX];

char *tmpnam(char *s)
{
    static char buf[20];
    char *out = s ? s : buf;
    mkdir(TMP_DIR, 0777);
    for (int tries = 0; tries < 1000; tries++) {
        unsigned v = (unsigned)(timer_now_us() ^ (uint64_t)++tmp_seq * 2654435761u) % 1000000;
        snprintf(out, 20, TMP_DIR "/tmp%06u", v);
        struct stat st;
        if (stat(out, &st) != 0)
            return out;
    }
    return nullptr;
}

FILE *tmpfile(void)
{
    int i = 0;
    while (i < TMPFILE_MAX && tmps[i].path[0])
        i++;
    char name[20];
    if (i == TMPFILE_MAX || !tmpnam(name)) {
        errno = EMFILE;
        return nullptr;
    }
    FILE *f = fopen(name, "w+b");
    if (!f)
        return nullptr;
    snprintf(tmps[i].path, sizeof tmps[i].path, "%s", name);
    tmps[i].depth = app_depth();
    return f;
}

// アプリが終わった: tmpfile のファイルを消す（FILE はアプリの終わりに閉じている）
static void tmpfile_app_end(void)
{
    int depth = app_depth() + 1;    // 終わったアプリの段
    for (int i = 0; i < TMPFILE_MAX; i++)
        if (tmps[i].path[0] && tmps[i].depth >= depth) {
            remove(tmps[i].path);
            tmps[i].path[0] = '\0';
        }
}

// ---- ファイル ----

// FAT にはシンボリックリンクがないので、lstat は stat と同じ
int lstat(const char *restrict path, struct stat *restrict st)
{
    return stat(path, st);
}

// readlink: FAT にはシンボリックリンクがない（あるものはリンクでないので EINVAL。Linux と同じ）。symlink・link は
// 作れない（Linux の vfat と同じ EPERM）
ssize_t readlink(const char *restrict path, char *restrict buf, size_t size)
{
    (void)buf;
    (void)size;
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    errno = EINVAL;
    return -1;
}

int symlink(const char *target, const char *path)
{
    (void)target;
    (void)path;
    errno = EPERM;
    return -1;
}

int link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = EPERM;
    return -1;
}

// utimes（マイクロ秒の形）: FAT は書き換えた日時を 2 秒の単位で持つだけ（utime と同じ。os/fs/vfs.c）
int utimes(const char *path, const struct timeval tv[2])
{
    if (!tv)
        return utime(path, nullptr);
    struct utimbuf t = {.actime = tv[0].tv_sec, .modtime = tv[1].tv_sec};
    return utime(path, &t);
}

// ---- プロセス（2026-11-20、vim）----
// Monocs にはプロセスがない（同時に動くアプリは 1 つ。別のプログラムは system()・popen で、アプリの中で入れ子に動く）。
// fork・exec・dup・wait は、Linux のプログラムがつながるように同じ名前で置き、いつも失敗する（ENOSYS）。pipe は
// 同じプログラムの中のもの（os/libc/pipe.c）。
// picolibc をプロセスのない機械で使うときの代わりの関数と同じ考え
pid_t fork(void) { errno = ENOSYS; return -1; }
pid_t vfork(void) { errno = ENOSYS; return -1; }
int execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOSYS; return -1; }
int execvp(const char *file, char *const argv[]) { (void)file; (void)argv; errno = ENOSYS; return -1; }
int execve(const char *path, char *const argv[], char *const envp[])
{
    (void)path;
    (void)argv;
    (void)envp;
    errno = ENOSYS;
    return -1;
}
int dup(int fd) { (void)fd; errno = ENOSYS; return -1; }
int dup2(int fd, int fd2) { (void)fd; (void)fd2; errno = ENOSYS; return -1; }
pid_t wait(int *status) { (void)status; errno = ECHILD; return -1; }   // 子のプロセスはない（Linux と同じ ECHILD）
pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)status;
    (void)options;
    errno = ECHILD;
    return -1;
}

// umask: FAT には許可の印がないので、覚えて返すだけ（アプリごと。既定 022。Linux と同じ。posix_app_begin で 022）

mode_t umask(mode_t mask)
{
    mode_t old = app_umask;
    app_umask = mask & 0777;
    return old;
}

// ---- ユーザー ----
// ユーザーは root 1 人（番号 0、ホームは /root）。Linux で root として動かしたときと同じ答え
// setuid・setgid: root（0）にだけ変えられる（ほかのユーザーはいない。Linux の root の setuid(0) と同じく成功）
int setuid(uid_t uid) { if (uid == 0) return 0; errno = EPERM; return -1; }
int setgid(gid_t gid) { if (gid == 0) return 0; errno = EPERM; return -1; }
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

static struct passwd root_pw;

static struct passwd *root_passwd(void)
{
    root_pw = (struct passwd){.pw_name = (char *)"root", .pw_uid = 0, .pw_gid = 0,
                              .pw_dir = (char *)"/root", .pw_shell = (char *)"/bin/sh"};
    return &root_pw;
}

struct passwd *getpwuid(uid_t uid)
{
    return uid == 0 ? root_passwd() : nullptr;
}

struct passwd *getpwnam(const char *name)
{
    return name && strcmp(name, "root") == 0 ? root_passwd() : nullptr;
}

// グループも root（0）だけ
static char *root_members[] = {(char *)"root", nullptr};
static struct group root_gr;

static struct group *root_group(void)
{
    root_gr = (struct group){.gr_name = (char *)"root", .gr_gid = 0, .gr_mem = root_members};
    return &root_gr;
}

struct group *getgrgid(gid_t gid)
{
    return gid == 0 ? root_group() : nullptr;
}

struct group *getgrnam(const char *name)
{
    return name && strcmp(name, "root") == 0 ? root_group() : nullptr;
}

int access(const char *path, int mode)
{
    (void)mode;     // 読み書きの権限はない（FAT）。あれば何でもできる
    struct stat st;
    return stat(path, &st);
}

// 絶対パスにし、「.」「..」と重なった「/」を取り除く。ファイルがなければ NULL（POSIX と同じ）。
// シンボリックリンクはない（FAT）
char *realpath(const char *restrict path, char *restrict resolved)
{
    if (!path || !*path) {
        errno = ENOENT;
        return nullptr;
    }
    char buf[PATH_MAX];
    size_t len = 0;
    if (path[0] != '/') {
        if (!getcwd(buf, sizeof(buf)))
            return nullptr;
        len = strlen(buf);
        if (len == 1)
            len = 0;    // "/"
    }
    for (const char *p = path; *p;) {
        while (*p == '/')
            p++;
        const char *q = p;
        while (*q && *q != '/')
            q++;
        size_t n = (size_t)(q - p);
        if (n == 0 || (n == 1 && p[0] == '.')) {
            // 何もしない
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (len > 0 && buf[len - 1] != '/')
                len--;
            if (len > 0)
                len--;  // 区切りも
        } else {
            if (len + 1 + n + 1 > sizeof(buf)) {
                errno = ENAMETOOLONG;
                return nullptr;
            }
            buf[len++] = '/';
            memcpy(buf + len, p, n);
            len += n;
        }
        p = q;
    }
    if (len == 0)
        buf[len++] = '/';
    buf[len] = '\0';
    struct stat st;
    if (stat(buf, &st) != 0)
        return nullptr;
    if (!resolved && !(resolved = malloc(len + 1))) {
        errno = ENOMEM;
        return nullptr;
    }
    memcpy(resolved, buf, len + 1);
    return resolved;
}

// ---- struct tm の tm_gmtoff・tm_zone（2026-11-20） ----
// アプリの struct tm は glibc と同じく tm_gmtoff・tm_zone を持つ（cc/slimcc_headers/include/time.h）。picolibc の
// localtime などは先頭の 9 個しか書かないので、アプリにはここの版を渡す（OS API の表で差し替える）。カーネルの
// GCC 版は picolibc の小さい struct tm を見るので、ここは自前の形 AppTm で書く（先頭は struct tm と同じ）
extern char *const tzname[2];

// 地方時の tm（t の時刻）に、UTC からのずれと時間帯の名前を入れる
static void fill_local_zone(AppTm *tm, time_t t)
{
    AppTm u = *tm;
    time_t as_utc = timegm((struct tm *)&u);
    tm->gmtoff = (long)(as_utc - t);
    tm->zone = tzname[tm->f[8] > 0];
}

AppTm *app_localtime_r(const time_t *t, AppTm *out)
{
    if (!localtime_r(t, (struct tm *)out))
        return nullptr;
    fill_local_zone(out, *t);
    return out;
}

AppTm *app_localtime(const time_t *t)
{
    static AppTm buf;
    return app_localtime_r(t, &buf);
}

AppTm *app_gmtime_r(const time_t *t, AppTm *out)
{
    if (!gmtime_r(t, (struct tm *)out))
        return nullptr;
    out->gmtoff = 0;
    out->zone = "GMT";      // glibc と同じ
    return out;
}

AppTm *app_gmtime(const time_t *t)
{
    static AppTm buf;
    return app_gmtime_r(t, &buf);
}

// mktime・timegm は tm を正規化して書き直すので、ずれと名前も書き直す（glibc と同じ）
time_t app_mktime(AppTm *tm)
{
    time_t t = mktime((struct tm *)tm);
    if (t != (time_t)-1)
        fill_local_zone(tm, t);
    return t;
}

time_t app_timegm(AppTm *tm)
{
    time_t t = timegm((struct tm *)tm);
    tm->gmtoff = 0;
    tm->zone = "GMT";
    return t;
}

// ---- 時刻と待ち ----

static uint64_t cpu_busy_us(void)
{
    uint64_t now = timer_now_us(), busy = 0, f = read_sysreg(cntfrq_el0);
    for (int c = 0; c < 4; c++) {
        uint64_t idle;
        if (c == 0) {
            idle = timer_idle_us();
        } else {
            int st = smp_core_state(c);
            if (st == 0 || st == 4)
                continue;   // 起きていない・サービスのコア
            uint64_t k = smp_idle_ticks(c);
            idle = k / f * 1000000 + k % f * 1000000 / f;
        }
        busy += idle < now ? now - idle : 0;
    }
    return busy;
}

// getrusage（Linux と同じ struct rusage。os/api/include/sys/resource.h）: RUSAGE_SELF・RUSAGE_THREAD の ru_utime は、
// アプリを始めてからのアプリのコアの CPU 時間（Monocs は OS とアプリを分けないので ru_stime は 0）。ru_maxrss は
// アプリ用のヒープの大きさ（KB）。ほかは 0。RUSAGE_CHILDREN はすべて 0（system() の子もアプリの中で動き、SELF に入る）
int getrusage(int who, struct rusage *ru)
{
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
        errno = EINVAL;
        return -1;
    }
    memset(ru, 0, sizeof *ru);
    if (who == RUSAGE_CHILDREN)
        return 0;
    uint64_t now = cpu_busy_us();
    uint64_t us = now > app_cpu_base ? now - app_cpu_base : 0;
    ru->ru_utime.tv_sec = (time_t)(us / 1000000);
    ru->ru_utime.tv_usec = (suseconds_t)(us % 1000000);
    struct mallinfo mi = mallinfo();
    ru->ru_maxrss = (long)(mi.arena / 1024);
    return 0;
}

// getrlimit・setrlimit: Monocs の決まった大きさを返す（開けるファイルは fd 0〜255（os/fs/vfs.h の VFS_MAX_FILES）、
// スタックはコアごとに 8MB（os/boot/linker.ld）。ほかは上限なし）。下げるのは受け付けるだけで、守らせはしない
static rlim_t rlimit_of(int resource)
{
    switch (resource) {
    case RLIMIT_NOFILE: return VFS_MAX_FILES;
    case RLIMIT_STACK:  return 8 * 1024 * 1024;
    case RLIMIT_CORE:   return 0;
    default:            return RLIM_INFINITY;
    }
}

int getrlimit(int resource, struct rlimit *rlim)
{
    if (resource < 0 || resource >= RLIMIT_NLIMITS) {
        errno = EINVAL;
        return -1;
    }
    rlim->rlim_cur = rlim->rlim_max = rlimit_of(resource);
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    if (resource < 0 || resource >= RLIMIT_NLIMITS || rlim->rlim_cur > rlim->rlim_max) {
        errno = EINVAL;
        return -1;
    }
    if (rlim->rlim_max > rlimit_of(resource)) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

// ---- 共有ライブラリ（dlfcn.h）----
// Monocs には共有ライブラリ（.so）を読み込む仕組みがない（ライブラリはソースのまま /usr/lib に置き、-l名前 で
// 一緒にコンパイルする）。Linux と同じ名前と形で用意し、dlopen はいつも失敗して理由を dlerror で返す
static const char *dl_error;

void *dlopen(const char *file, int mode)
{
    (void)file;
    (void)mode;
    dl_error = "dlopen: Monocs cannot load shared libraries (use a library from /usr/lib with -lNAME)";
    return nullptr;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    dl_error = "dlsym: no shared library is loaded";
    return nullptr;
}

int dlclose(void *handle)
{
    (void)handle;
    dl_error = "dlclose: no shared library is loaded";
    return -1;
}

char *dlerror(void)
{
    const char *e = dl_error;
    dl_error = nullptr;     // Linux と同じく、読んだら消す
    return (char *)e;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    uint64_t us = timer_now_us();
    if (clock_id == CLOCK_REALTIME) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);     // os/libc/syscalls.c（RTC がないので起動を 2026-01-01 とする）
        tp->tv_sec = tv.tv_sec;
        tp->tv_nsec = tv.tv_usec * 1000;
        return 0;
    }
    if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_MONOTONIC_RAW && clock_id != CLOCK_MONOTONIC_COARSE) {
        errno = EINVAL;
        return -1;
    }
    tp->tv_sec = (time_t)(us / 1000000);
    tp->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

// 日時を設定する（os/kernel/clock.c。CLOCK_REALTIME だけ。権限はない（単一特権））
int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    if (clock_id != CLOCK_REALTIME || tp->tv_nsec < 0 || tp->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    clock_set((int64_t)tp->tv_sec * 1000000 + tp->tv_nsec / 1000);
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    if (tv)
        clock_set((int64_t)tv->tv_sec * 1000000 + tv->tv_usec);
    return 0;
}

// 待つ間は眠る（timer_delay_us。Ctrl-C で止められる）。シグナルはないので途中で起きない
// usleep（POSIX.1-2001 にあり、2008 で消えたが、Linux のプログラムでよく使う）。sleep は picolibc のもの
// （nanosleep を呼ぶ）
int usleep(useconds_t us)
{
    timer_delay_us(us);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    // シグナルの関数が呼ばれたら、残りの時間を返して早く戻る（-1、EINTR。Linux と同じ。os/libc/signal.c）
    uint64_t end = timer_now_us() + (uint64_t)req->tv_sec * 1000000 + (uint64_t)(req->tv_nsec + 999) / 1000;
    uint32_t sigs = signal_delivered();
    for (;;) {
        uint64_t now = timer_now_us();
        if (now >= end)
            break;
        // 1 ティック（10ms）より長く残っていれば、次の割り込みまで眠る（cpu_idle がシグナルを渡す）。残りは
        // timer_delay_us（1 ティックより短い待ちは眠らずに回る）
        if (end - now > 1000000 / TIMER_HZ)
            cpu_idle();
        else
            timer_delay_us(end - now);
        if (signal_delivered() != sigs) {
            now = timer_now_us();
            uint64_t left = now < end ? end - now : 0;
            if (rem)
                *rem = (struct timespec){(time_t)(left / 1000000), (long)(left % 1000000) * 1000};
            errno = EINTR;
            return -1;
        }
    }
    if (rem)
        *rem = (struct timespec){0};
    return 0;
}
