// 呼び出しの引数を x0〜x7 に並列に写す（ir_par_move。2026-11-27 から、呼び出しのある関数でも値を x0〜x7 に置く）:
// 引数の並びを入れ替えて呼ぶ（輪）、関数のポインタ、同じ値を 2 つの引数に、定数と混ぜる、戻り値を次の引数に、
// 構造体を 2 語で返す、呼び出しをまたぐ値
#include <stdio.h>
#include <stdint.h>

typedef long (*F2)(long, long);
typedef struct { long a, b; } P;

static volatile long sink;
__attribute__((noinline)) static long sub2(long a, long b) { sink++; return a - b * 3; }
__attribute__((noinline)) static long mix8(long a, long b, long c, long d, long e, long f, long g, long h) {
  sink++;
  return a + 2 * b + 3 * c + 5 * d + 7 * e + 11 * f + 13 * g + 17 * h;
}
__attribute__((noinline)) static int mix3i(int a, int b, int c) { sink++; return a * 100 + b * 10 + c; }

__attribute__((noinline)) static long swap2(long a, long b) { return sub2(b, a); }
__attribute__((noinline)) static long rot3(long a, long b, long c) { return mix8(c, a, b, 0, 0, 0, 0, 1); }
__attribute__((noinline)) static long rev8(long a, long b, long c, long d, long e, long f, long g, long h) {
  return mix8(h, g, f, e, d, c, b, a);
}
__attribute__((noinline)) static long dup(long a, long b) { return mix8(a, a, b, b, 7, a, 1000, -5); }
__attribute__((noinline)) static long viafp(F2 f, long a, long b) { return f(b, a); }
__attribute__((noinline)) static long viafp2(long a, F2 f, long b) { return f(b, a) + f(a, b); }
__attribute__((noinline)) static long chain(long a, long b) { return sub2(sub2(a, b), sub2(b, a)); }
__attribute__((noinline)) static long cross(long a, long b) {
  long x = sub2(a, 1);
  long y = sub2(b, x);
  return mix8(y, x, a, b, x + y, a - b, 3, 4);
}
__attribute__((noinline)) static int ints(int a, int b, int c) { return mix3i(c, a, b) + mix3i(b, c, a); }
__attribute__((noinline)) static P pair(long a, long b) { P p = {b + 1, a - 1}; return p; }
__attribute__((noinline)) static P swapp(P p) { P q = {p.b, p.a}; return q; }
__attribute__((noinline)) static long usep(long a, long b) { P p = swapp(pair(a, b)); return sub2(p.b, p.a); }
__attribute__((noinline)) static long loop(long n, long k) {
  long s = 0;
  for (long i = 0; i < n; i++)
    s += sub2(k, i) + sub2(i, s & 7);
  return s;
}

int main(void) {
  long v[] = {0, 1, -1, 5, 123456789, -987654321, 1L << 40};
  for (int i = 0; i < 7; i++)
    for (int j = 0; j < 7; j++) {
      long a = v[i], b = v[j];
      printf("%ld %ld %ld %ld %ld %ld %ld %ld %d %ld %ld\n", swap2(a, b), rot3(a, b, a ^ b),
             rev8(a, b, 1, 2, 3, 4, a + b, a - b), dup(a, b), viafp(sub2, a, b), viafp2(a, sub2, b), chain(a, b),
             cross(a, b), ints((int)a, (int)b, 7), usep(a, b), loop(i + j, a));
    }
  printf("sink %ld\n", sink);
  return 0;
}
