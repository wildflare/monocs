// 資源の使用量と上限（Linux の glibc と同じ形と値。picolibc のものを置き換える。tools/mkimg.sh が /usr/include に）
// picolibc の sys/resource.h は struct rusage が ru_utime・ru_stime だけで、getrusage を _XOPEN_SOURCE のときだけ
// 宣言する（glibc はいつも）。SQLite の shell.c などが使う。getrusage は os/libc/posix.c
#ifndef _SYS_RESOURCE_H_
#define _SYS_RESOURCE_H_
#define _SYS_RESOURCE_H 1

#include <sys/cdefs.h>
#include <sys/_types.h>
#include <sys/_timeval.h>

_BEGIN_STD_C

#ifndef _RLIM_T_DECLARED
typedef __rlim_t rlim_t;
#define _RLIM_T_DECLARED
#endif

#ifndef _ID_T_DECLARED
typedef __id_t id_t;
#define _ID_T_DECLARED
#endif

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

struct rusage {
    struct timeval ru_utime;    // ユーザーの CPU 時間
    struct timeval ru_stime;    // システムの CPU 時間
    long ru_maxrss;             // 使ったメモリの最大（KB）
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define RLIM_INFINITY  ((rlim_t)-1)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9
#define RLIMIT_NLIMITS 16

int getrusage(int who, struct rusage *usage);
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

_END_STD_C

#endif
