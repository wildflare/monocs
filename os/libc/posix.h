// picolibc にない POSIX の関数（os/libc/posix.c）
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include "signal_os.h"

// アプリごとの状態（getpid の番号、getopt の状態）。アプリを動かす前に posix_app_begin で外側のものを控えて
// 新しくし、終わったら posix_app_end で戻す（入れ子のアプリ（system()）の後も、外側のアプリの状態に戻る）。
// 環境変数も、アプリが変えたものを戻す（os/libc/env.c）
typedef struct {
    pid_t pid;
    char *optarg;
    int optind, opterr, optopt;
    const char *locale;     // setlocale(LC_ALL, NULL) の名前（picolibc の静的な文字列）
    SignalState sig;        // シグナルの関数の表など（os/libc/signal.c）
    uint64_t cpu_base;      // getrusage の起点（アプリを始めたときの、アプリのコアの眠っていなかった時間）
    mode_t umask;
} PosixApp;

void posix_app_begin(PosixApp *outer);
void posix_app_end(const PosixApp *outer);

char *app_setlocale(int category, const char *name);   // アプリの setlocale（"" は環境変数から）

void mman_close_owner(int owner);  // 持ち主が終わったら mmap を外す（os/libc/mman.c）

// アプリに渡す struct tm（glibc と同じく tm_gmtoff・tm_zone 付き。先頭は picolibc の struct tm と同じ）と、それを
// 埋める localtime などの版（os/libc/posix.c。OS API の表で差し替える。2026-11-20）
#include <time.h>
typedef struct {
    int f[9];               // tm_sec 〜 tm_isdst
    long gmtoff;
    const char *zone;
} AppTm;
AppTm *app_localtime(const time_t *t);
AppTm *app_localtime_r(const time_t *t, AppTm *out);
AppTm *app_gmtime(const time_t *t);
AppTm *app_gmtime_r(const time_t *t, AppTm *out);
time_t app_mktime(AppTm *tm);
time_t app_timegm(AppTm *tm);
