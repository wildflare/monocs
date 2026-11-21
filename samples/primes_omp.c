// 素数の数を OpenMP で分担して数える（primes_pthread.c と同じ計算を、#pragma omp の 1 行で）。
//   Monocs: cc -o primes_omp -fopenmp primes_omp.c && ./primes_omp     Linux: gcc -O2 -fopenmp primes_omp.c
// 大きい数ほど時間がかかるので schedule(dynamic, 1000)（1000 個ずつ、空いたスレッドが取る）
#include <omp.h>
#include <stdio.h>

#define LIMIT 2000000

static int is_prime(int n)
{
    if (n < 2)
        return 0;
    if (n % 2 == 0)
        return n == 2;
    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0)
            return 0;
    return 1;
}

int main(void)
{
    int max = omp_get_max_threads();
    printf("%d threads at most\n", max);
    for (int n = 1; n <= max; n++) {
        double t = omp_get_wtime();
        int count = 0;
#pragma omp parallel for num_threads(n) reduction(+ : count) schedule(dynamic, 1000)
        for (int i = 0; i < LIMIT; i++)
            count += is_prime(i);
        printf("%d thread(s): %d primes < %d, %.0f ms\n", n, count, LIMIT, (omp_get_wtime() - t) * 1000);
    }
    return 0;
}
