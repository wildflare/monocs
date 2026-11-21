// sys/sysinfo.h: get_nprocs（glibc と同じ名前。Monocs の sysconf を呼ぶ）
#pragma once
#include <unistd.h>

static inline int get_nprocs(void)
{
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

static inline int get_nprocs_conf(void)
{
    return (int)sysconf(_SC_NPROCESSORS_CONF);
}
