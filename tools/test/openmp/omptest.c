// OpenMP の試験（tools/test/openmp.sh）。GCC の -fopenmp（libgomp）、Linux の slimcc -fopenmp（os/libc/omp.c）、
// Monocs で同じ出力になること。出力はスレッドの順に左右されないものだけにする。
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 100000

struct point {
    int x, y;
};

static long long square(int i)
{
    return (long long)i * i;
}

static int data[N];

int main(void)
{
#ifdef _OPENMP
    printf("_OPENMP defined\n");
#endif
    int maxt = omp_get_max_threads();

    // 1. チームの人数（reduction）
    int team = 0;
#pragma omp parallel reduction(+ : team)
    team += 1;
    printf("team == max_threads: %d\n", team == maxt);

    // 2. parallel for と reduction（long long）
    long long sum = 0;
#pragma omp parallel for reduction(+ : sum)
    for (int i = 1; i <= 1000000; i++)
        sum += i;
    printf("sum %lld\n", sum);

    // 3. 配列を埋める（外の配列・関数の呼び出し）
#pragma omp parallel for
    for (int i = 0; i < N; i++)
        data[i] = (int)(square(i) % 1000);
    long long check = 0;
    for (int i = 0; i < N; i++)
        check += data[i] * (long long)(i % 7);
    printf("fill %lld\n", check);

    // 4. schedule
    long long s1 = 0, s2 = 0, s3 = 0;
#pragma omp parallel for schedule(static, 7) reduction(+ : s1)
    for (int i = 0; i < 1000; i++)
        s1 += i * 3;
#pragma omp parallel for schedule(dynamic, 3) reduction(+ : s2)
    for (int i = 0; i < 1000; i++)
        s2 += i * 5;
#pragma omp parallel for schedule(dynamic) reduction(+ : s3)
    for (int i = 0; i < 777; i++)
        s3 += i;
    printf("schedule %lld %lld %lld\n", s1, s2, s3);

    // 5. いろいろな繰り返しの形
    long long a = 0, b = 0, c = 0, d = 0, e = 0;
#pragma omp parallel for reduction(+ : a)
    for (int i = 0; i < 100; i += 2)
        a += i;
#pragma omp parallel for reduction(+ : b)
    for (int i = 100; i > 0; i--)
        b += i;
#pragma omp parallel for reduction(+ : c)
    for (int i = 99; i >= 3; i -= 3)
        c += i;
#pragma omp parallel for reduction(+ : d)
    for (long i = 5; 50 > i; i = i + 5)
        d += i;
    int k;
#pragma omp parallel for reduction(+ : e)
    for (k = 0; k != 64; ++k)
        e += k * k;
    printf("loops %lld %lld %lld %lld %lld\n", a, b, c, d, e);

    // 6. private・firstprivate
    int base = 10, tmp = -1;
    long long fp = 0;
#pragma omp parallel for firstprivate(base) private(tmp) reduction(+ : fp)
    for (int i = 0; i < 100; i++) {
        tmp = base + i;
        fp += tmp;
    }
    printf("firstprivate %lld base %d tmp %d\n", fp, base, tmp);

    // 7. critical・atomic
    int crit = 0, atom = 0;
#pragma omp parallel for
    for (int i = 0; i < 10000; i++) {
#pragma omp critical
        crit++;
#pragma omp atomic
        atom += 2;
    }
    printf("critical %d atomic %d\n", crit, atom);

    // 8. single・master・barrier
    int singles = 0, masters = 0, phase_ok = 1;
    int stage[16] = {0};
#pragma omp parallel
    {
#pragma omp single
        singles++;
#pragma omp master
        masters++;
        int me = omp_get_thread_num();
        stage[me] = 1;
#pragma omp barrier
        for (int t = 0; t < omp_get_num_threads(); t++)
            if (stage[t] != 1)
                phase_ok = 0;
    }
    printf("single %d master %d barrier %d\n", singles, masters, phase_ok);

    // 9. max・min・*・&&・||
    int mx = -1000, mn = 1000, all = 1, any = 0;
    long long prod = 1;
#pragma omp parallel for reduction(max : mx) reduction(min : mn) reduction(&& : all) reduction(|| : any)
    for (int i = 0; i < 500; i++) {
        int v = (i * 37) % 501 - 250;
        if (v > mx)
            mx = v;
        if (v < mn)
            mn = v;
        all = all && (v > -300);
        any = any || (v == 0);
    }
#pragma omp parallel for reduction(* : prod)
    for (int i = 1; i <= 15; i++)
        prod *= i;
    printf("max %d min %d all %d any %d prod %lld\n", mx, mn, all, any, prod);

    // 10. 構造体・ポインタ・2 次元の配列・中で宣言した変数
    struct point pts[64];
    int(*grid)[8] = malloc(sizeof(int[8][8]));
    int *ptr = data;
#pragma omp parallel for
    for (int i = 0; i < 64; i++) {
        int local = i * 2;
        pts[i].x = local;
        pts[i].y = ptr[i];
        grid[i / 8][i % 8] = local + 1;
    }
    long long ps = 0;
    for (int i = 0; i < 64; i++)
        ps += pts[i].x + pts[i].y + grid[i / 8][i % 8];
    printf("struct %lld\n", ps);
    free(grid);

    // 11. num_threads・if
    int n2 = 0, n1 = 0;
#pragma omp parallel num_threads(2) reduction(+ : n2)
    n2 += 1;
#pragma omp parallel if (0) reduction(+ : n1)
    n1 += 1;
    printf("num_threads(2) %d if(0) %d\n", n2 <= 2 && n2 >= 1, n1);

    // 12. nowait と、parallel の中の for
    long long w1 = 0, w2 = 0;
#pragma omp parallel
    {
#pragma omp for nowait reduction(+ : w1)
        for (int i = 0; i < 1000; i++)
            w1 += i;
#pragma omp for reduction(+ : w2)
        for (int i = 0; i < 1000; i++)
            w2 += 2 * i;
    }
    printf("inner for %lld %lld\n", w1, w2);

    // 13. thread_num の範囲、wtime
    int bad = 0;
#pragma omp parallel reduction(+ : bad)
    {
        int t = omp_get_thread_num(), n = omp_get_num_threads();
        if (t < 0 || t >= n)
            bad++;
    }
    double t0 = omp_get_wtime(), t1 = omp_get_wtime();
    printf("thread_num ok %d, wtime ok %d\n", bad == 0, t1 >= t0);

    // 14. 入れ子（中は 1 人で動く）
    int outer = 0;
#pragma omp parallel reduction(+ : outer)
    {
        int inner = 0;
#pragma omp parallel reduction(+ : inner)
        inner += 1;
        if (inner >= 1)
            outer += 1;
    }
    printf("nested %d\n", outer == maxt);

    // 15. omp_lock_t
    omp_lock_t lock;
    omp_init_lock(&lock);
    int locked = 0;
#pragma omp parallel for
    for (int i = 0; i < 1000; i++) {
        omp_set_lock(&lock);
        locked++;
        omp_unset_lock(&lock);
    }
    omp_destroy_lock(&lock);
    printf("lock %d\n", locked);
    printf("done\n");
    return 0;
}
