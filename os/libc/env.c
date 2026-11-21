// アプリの環境変数: setenv・unsetenv・putenv（2026-11-14）。
// Linux では、子のプロセスが変えた環境変数は親（シェル）に残らない。Monocs のアプリはシェルと同じ環境変数の
// 一覧（picolibc の environ）を使うので、アプリが最初に変えるときに一覧を控え、アプリが終わったら元に戻す。
// 変えないアプリには負担がない。値はカーネル用のヒープに置く（アプリ用のヒープはアプリの終わりに捨てるので、
// そこに置くと、シェルが消えた文字列を読む）。入れ子のアプリ（system()）は段ごとに控える。
// putenv は、渡した文字列そのものではなく写しを一覧に入れる（POSIX では文字列そのものが入り、後から書き換えると
// 値も変わる。アプリの文字列はアプリが終わると消えるので、写しにする）。

#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "env.h"
#include "heap.h"
#include "../kernel/app.h"
#include "../kernel/sysreg.h"

extern char **environ;

#define ENV_NEST_MAX 10     // アプリの入れ子（8 段）とシェルの分より多く

static char **snap[ENV_NEST_MAX];   // 段ごとの控え（「NAME=VALUE」の写しの並び、NULL で終わる）。なければ NULL
static int level;                   // 動いているアプリの段の数

void env_app_begin(void)
{
    if (level < ENV_NEST_MAX)
        snap[level] = nullptr;
    level++;
}

static void free_snap(char **s)
{
    for (char **p = s; p && *p; p++)
        free(*p);
    free(s);
}

// 今の段の控えがなければ作る（カーネル用のヒープに）
static void take_snapshot(void)
{
    int l = level - 1;
    if (l < 0 || l >= ENV_NEST_MAX || snap[l] || cpu_id() != 0 || !app_is_running())
        return;
    malloc_kernel_begin();  // 控えはアプリが終わった後に使うので、カーネル用のヒープに
    int n = 0;
    for (char **p = environ; p && *p; p++)
        n++;
    char **s = calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; s && i < n; i++)
        if (!(s[i] = strdup(environ[i]))) {
            free_snap(s);
            s = nullptr;
        }
    snap[l] = s;
    malloc_kernel_end();
}

void env_snapshot_now(void)
{
    take_snapshot();
}

void env_app_end(void)
{
    if (level == 0)
        return;
    int l = --level;
    if (l >= ENV_NEST_MAX || !snap[l])
        return;
    char **s = snap[l];
    snap[l] = nullptr;
    malloc_kernel_begin();
    // 控えにない名前を消す（消すと environ が動くので、名前を先に集める）
    int n = 0;
    for (char **p = environ; p && *p; p++)
        n++;
    char **names = calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; names && i < n; i++) {
        const char *eq = strchr(environ[i], '=');
        names[i] = strndup(environ[i], eq ? (size_t)(eq - environ[i]) : strlen(environ[i]));
    }
    for (int i = 0; names && i < n; i++) {
        if (!names[i])
            continue;
        size_t len = strlen(names[i]);
        bool kept = false;
        for (char **p = s; *p && !kept; p++)
            kept = !strncmp(*p, names[i], len) && (*p)[len] == '=';
        if (!kept)
            unsetenv(names[i]);
        free(names[i]);
    }
    free(names);
    // 控えの値に戻す（同じものは触らない）
    for (char **p = s; *p; p++) {
        char *eq = strchr(*p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *now = getenv(*p);
        if (!now || strcmp(now, eq + 1))
            setenv(*p, eq + 1, 1);
        *eq = '=';
    }
    free_snap(s);
    malloc_kernel_end();
}

int app_setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    take_snapshot();
    malloc_kernel_begin();
    int r = setenv(name, value, overwrite);
    malloc_kernel_end();
    return r;
}

int app_unsetenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    take_snapshot();
    malloc_kernel_begin();
    int r = unsetenv(name);
    malloc_kernel_end();
    return r;
}

int app_putenv(char *string)
{
    char *eq = string ? strchr(string, '=') : nullptr;
    if (!eq)    // 「=」のないもの: glibc と同じく、その名前を消す
        return string && *string ? app_unsetenv(string) : (errno = EINVAL, -1);
    if (eq == string) {
        errno = EINVAL;
        return -1;
    }
    take_snapshot();
    malloc_kernel_begin();
    char *name = strndup(string, (size_t)(eq - string));
    int r = name ? setenv(name, eq + 1, 1) : -1;
    free(name);
    malloc_kernel_end();
    return r;
}
