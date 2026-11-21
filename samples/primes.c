// 素数を数える（エラトステネスのふるい）
#include <stdio.h>
#include <stdlib.h>
#include <monocs.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1000000;
    char *sieve = calloc(n + 1, 1);
    if (!sieve) {
        printf("out of memory\n");
        return 1;
    }
    uint64_t t0 = timer_now_us();
    int count = 0;
    for (int i = 2; i <= n; i++) {
        if (sieve[i])
            continue;
        count++;
        for (long j = (long)i * i; j <= n; j += i)
            sieve[j] = 1;
    }
    printf("%d primes <= %d (%llu ms)\n", count, n,
           (unsigned long long)(timer_now_us() - t0) / 1000);
    free(sieve);
    return 0;
}
