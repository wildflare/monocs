// サンドボックスの中の時刻・眠る・乱数
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "sandbox_sys.h"

int clock_gettime(clockid_t id, struct timespec *ts)
{
    uint64_t ns = (uint64_t)sb_sys(SB_SYS_CLOCK, id == CLOCK_REALTIME ? 0 : 1, 0, 0);
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return 0;
}

time_t time(time_t *t)
{
    time_t v = (time_t)((uint64_t)sb_sys(SB_SYS_CLOCK, 0, 0, 0) / 1000000000ULL);
    if (t)
        *t = v;
    return v;
}

clock_t clock(void)
{
    return (clock_t)((uint64_t)sb_sys(SB_SYS_CLOCK, 1, 0, 0) / (1000000000ULL / CLOCKS_PER_SEC));
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    sb_sys(SB_SYS_SLEEP, (long)((uint64_t)req->tv_sec * 1000000000ULL + (uint64_t)req->tv_nsec), 0, 0);
    if (rem)
        rem->tv_sec = 0, rem->tv_nsec = 0;
    return 0;
}

int usleep(useconds_t us)
{
    sb_sys(SB_SYS_SLEEP, (long)us * 1000, 0, 0);
    return 0;
}

unsigned sleep(unsigned s)
{
    sb_sys(SB_SYS_SLEEP, (long)s * 1000000000L, 0, 0);
    return 0;
}

int getentropy(void *buf, size_t len)
{
    return len <= 256 && sb_sys(SB_SYS_RANDOM, (long)buf, (long)len, 0) == 0 ? 0 : -1;
}

// rand: 線形合同法（決まった並び。srand で種を変える）
static unsigned long long seed = 1;

void srand(unsigned s)
{
    seed = s;
}

int rand(void)
{
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((seed >> 33) & RAND_MAX);
}
