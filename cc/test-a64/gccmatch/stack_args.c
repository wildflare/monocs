// スタックで渡す引数（2026-11-21。中間表現で扱うようにした。呼ぶ側・呼ばれる側とも GCC と同じ結果か）
#include <stdio.h>
#include <string.h>

typedef struct { long a, b; } P;            // 16 バイト（x 2 つ）
typedef struct { double x, y, z; } H;        // HFA（v 3 つ）
typedef struct { char s[40]; int n; } Big;  // 参照渡し
typedef struct { char c[3]; } T3;           // 3 バイト

__attribute__((noinline)) long many_i(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7,
                                      char c8, short s9, int i10, unsigned char u11, long l12)
{
    return a0 + a1 * 2 + a2 * 3 + a3 * 4 + a4 * 5 + a5 * 6 + a6 * 7 + a7 * 8 + c8 * 9 + s9 * 10 + i10 * 11 +
           u11 * 12 + l12 * 13;
}

__attribute__((noinline)) double many_f(double d0, double d1, double d2, double d3, double d4, double d5, double d6,
                                        double d7, float f8, double d9, int i, float f10)
{
    return d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + f8 * 2 + d9 * 3 + i * 4 + f10 * 5;
}

__attribute__((noinline)) long structs(long a0, P p1, P p2, P p3, P p4, long a5, P p6, T3 t, Big b, H h1, H h2, H h3)
{
    long s = a0 + p1.a + p1.b * 2 + p2.a * 3 + p2.b + p3.a + p3.b + p4.a * 5 + p4.b * 7 + a5 + p6.a * 11 + p6.b * 13;
    s += t.c[0] + t.c[1] * 2 + t.c[2] * 3 + b.n + b.s[0] + b.s[39];
    s += (long)(h1.x + h2.y * 2 + h3.z * 3);
    b.n = 0; // 呼び出し元のものは変わらない
    return s;
}

// アドレスを取る・書き換える引数
__attribute__((noinline)) int addr_taken(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
{
    int *p = &j;
    *p += i;
    i = 100;
    return a + b + c + d + e + f + g + h + i + j;
}

static long (*fp)(long, long, long, long, long, long, long, long, char, short, int, unsigned char, long) = many_i;

// 再帰（呼ばれる側でもスタックの引数を使って呼ぶ）
__attribute__((noinline)) long rec(int n, long a1, long a2, long a3, long a4, long a5, long a6, long a7, long s8,
                                   long s9)
{
    if (n == 0)
        return s8 * 1000 + s9;
    return rec(n - 1, a1, a2, a3, a4, a5, a6, a7, s8 + a1, s9 + n);
}

int main(void)
{
    printf("many_i %ld\n", many_i(1, 2, 3, 4, 5, 6, 7, 8, -9, -300, 70000, 250, 1L << 40));
    printf("many_f %.17g\n", many_f(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 0.25f, 1e10, -7, 3.5f));
    P p = {1, 2}, q = {3, 4};
    T3 t = {{5, 6, 7}};
    Big b;
    memset(&b, 1, sizeof b);
    b.n = 42;
    H h = {1.5, 2.5, 3.5};
    printf("structs %ld %d\n", structs(10, p, q, p, q, 20, p, t, b, h, h, h), b.n);
    printf("addr %d\n", addr_taken(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    long acc = 0;
    for (int k = 0; k < 100; k++)
        acc += fp(k, k, k, k, k, k, k, k, (char)k, (short)(k * 7), k * k, (unsigned char)(k * 3), -k);
    printf("loop %ld\n", acc);
    printf("rec %ld\n", rec(50, 1, 2, 3, 4, 5, 6, 7, 8, 9));
    return 0;
}
