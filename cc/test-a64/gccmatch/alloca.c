// __builtin_alloca（2026-11-21。中間表現で扱うようにした）: ループの中、スタックの引数の呼び出しと一緒、大きいもの
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static long sum(long *p, int n, long a2, long a3, long a4, long a5, long a6, long a7,
                                           long s8, long s9)
{
    long s = s8 * 1000 + s9;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

__attribute__((noinline)) static long use_alloca(int n)
{
    long total = 0;
    char *keep[8];
    for (int k = 0; k < 8; k++) {
        long *p = __builtin_alloca(sizeof(long) * (n + k));
        for (int i = 0; i < n + k; i++)
            p[i] = i * (k + 1);
        keep[k] = (char *)p;
        total += sum(p, n + k, 2, 3, 4, 5, 6, 7, k, n);
    }
    // 前に取ったものが壊れていない
    for (int k = 0; k < 8; k++)
        total += ((long *)keep[k])[n - 1];
    char *big = __builtin_alloca(100000);
    memset(big, 7, 100000);
    total += big[0] + big[99999];
    return total;
}

int main(void)
{
    printf("alloca %ld %ld %ld\n", use_alloca(1), use_alloca(10), use_alloca(1000));
    return 0;
}
