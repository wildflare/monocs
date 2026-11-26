// 定数の計算の直し（2026-11-27）: (x ± c1) ± c2 のまとめ（後置の増減）、負の即値（add・sub・cmp → sub・add・cmn）、
// 定数の掛け算をずらす命令と足し算・引き算に（(2^k ± 1) × 2^m）、4 バイトの読み出し・int の戻り値を符号拡張しない
// （下位 32 ビットだけを使うとき）。いろいろな型と、負の値・あふれの境目で GCC と比べる
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

static volatile int vsink;
__attribute__((noinline)) static int neg_int(int x) { vsink++; return -x - 7; }
__attribute__((noinline)) static unsigned uret(unsigned x) { vsink++; return x * 3u; }

#define MULS(T, NAME)                                                                                  \
  __attribute__((noinline)) static T NAME(T x) {                                                       \
    return (T)(x * 3) ^ (T)(x * 5) ^ (T)(x * 7) ^ (T)(x * 9) ^ (T)(x * 12) ^ (T)(x * 15) ^ (T)(x * 24) ^ \
           (T)(x * 40) ^ (T)(x * 56) ^ (T)(x * 96) ^ (T)(x * 1023) ^ (T)(x * 1025) ^ (T)(x * 6) ^      \
           (T)(x * 31) ^ (T)(x * 33) ^ (T)(x * 17);                                                    \
  }
MULS(int, mul_i)
MULS(unsigned, mul_u)
MULS(long, mul_l)
MULS(unsigned long, mul_ul)
MULS(short, mul_s)
MULS(unsigned char, mul_uc)

typedef struct { long a; int n; int m[3]; } S;
__attribute__((noinline)) static long postinc(S *s, long *p, int i) {
  long r = 0;
  for (int k = 0; k < 3; k++) {
    r += s->m[i++ & 1];
    r += *p++;
    s->n--;
    r += s->n;
  }
  long q = (long)s->m[2];         // 負の int を 64 ビットで
  return r * 7 + q + i;
}

__attribute__((noinline)) static int cmps(long x, int y, unsigned z) {
  int r = 0;
  if (x == -1) r |= 1;
  if (x < -5) r |= 2;
  if (y >= -100) r |= 4;
  if (y == -4095) r |= 8;
  if ((int)z == -2) r |= 16;
  if (x + -3 > 0) r |= 32;
  if (y - -9 < 0) r |= 64;
  return r;
}

__attribute__((noinline)) static long addneg(long x, int y, unsigned z, unsigned long w) {
  return (x + -1) ^ (x - -4095) ^ (long)(y + -77) ^ (long)(z + -1u) ^ (long)(w - -3ul) ^ (long)((y - 5) + 5) ^
         (long)((z + 7u) - 7u);
}

__attribute__((noinline)) static long rets(int x) {
  long a = neg_int(x);            // int の戻り値を 64 ビットで（符号拡張が要る）
  int b = neg_int(x + 1);         // 下位 32 ビットだけ
  unsigned long c = uret((unsigned)x);
  return a * 3 + b + (long)c + (neg_int(x) < 0);
}

int main(void) {
  long vals[] = {0, 1, -1, 2, -2, 7, -7, 100, -100, 4095, -4095, 4096, INT_MAX, INT_MIN, (long)UINT_MAX,
                 LONG_MAX, LONG_MIN, 0x123456789L, -0x123456789L};
  int n = sizeof(vals) / sizeof(*vals);
  for (int i = 0; i < n; i++) {
    long v = vals[i];
    long sm = v % 1000000;          // 符号つきの計算はあふれない値で（あふれは未定義動作）
    printf("%d %u %ld %lu %d %u\n", mul_i((int)sm), mul_u((unsigned)v), mul_l(sm), mul_ul((unsigned long)v),
           mul_s((short)(sm % 1000)), mul_uc((unsigned char)v));
    S s = {sm, (int)sm, {(int)sm, -3, (int)-sm}};
    long arr[3] = {sm, -sm, 5};
    printf("%ld %d %ld %ld\n", postinc(&s, arr, (int)sm), cmps(sm, (int)sm, (unsigned)v),
           addneg(sm, (int)sm, (unsigned)v, (unsigned long)v), rets((int)(v & 0xffff) - 300));
  }
  return 0;
}
