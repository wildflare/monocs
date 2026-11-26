// 1 ビットを見る分岐（tbz・tbnz。2026-11-27）: _Bool を返す関数の結果、x & 2^k（いろいろな型・ビットの位置）。
// 32KB より大きな関数では tbz が届かないので、逆の条件で飛び越して b で飛ぶ形になる（ir_relax_tbz）
#include <stdio.h>
#include <stdint.h>

static volatile int sink;
static int tab[64];

static _Bool odd(int x) { sink++; sink++; sink++; sink++; return tab[x & 63] & 1; }
static _Bool big(long x) { sink++; sink++; sink++; sink++; return x > 1000; }

#define BITS(T, NAME)                                                                   \
  static int NAME(T x) {                                                                \
    int r = 0;                                                                          \
    if (x & ((T)1 << 0)) r += 1;                                                        \
    if (x & ((T)1 << 1)) r += 2;                                                        \
    if (!(x & ((T)1 << 3))) r += 4;                                                     \
    if (x & ((T)1 << (sizeof(T) * 8 - 1))) r += 8;                                      \
    if (sizeof(T) >= 4 && (x & ((T)1 << (sizeof(T) * 4)))) r += 16;                     \
    if (sizeof(T) >= 4 && !(x & ((T)1 << (sizeof(T) * 8 - 2)))) r += 32;                \
    return r;                                                                           \
  }
BITS(unsigned char, b_uc)
BITS(signed char, b_sc)
BITS(unsigned short, b_us)
BITS(int, b_i)
BITS(unsigned, b_u)
BITS(long, b_l)
BITS(unsigned long, b_ul)

// 32KB を超える関数（volatile の読み書き 1 つが数命令）
#define S1 sink = sink * 3 + 1;
#define S10 S1 S1 S1 S1 S1 S1 S1 S1 S1 S1
#define S100 S10 S10 S10 S10 S10 S10 S10 S10 S10 S10
#define S1000 S100 S100 S100 S100 S100 S100 S100 S100 S100 S100
static int huge(long x, int y) {
  int r = 0;
  if (x & 4) {
    S1000 S1000 S1000
    r += 1;
  }
  if (odd(y)) {
    S1000 S1000 S1000
    r += 2;
  } else if (x & (1L << 40)) {
    S1000
    r += 4;
  }
  for (int i = 0; i < 3; i++)
    if (!(x & (1L << (60 + 0))))
      r += 8;
  return r;
}

int main(void) {
  for (int i = 0; i < 64; i++)
    tab[i] = i * 7 + (i >> 2);
  long vals[] = {0, 1, 2, 3, 5, 8, 9, 127, 128, 255, -1, -2, -128, 0x7fff, 0x8000, 0x10000, 0x7fffffff,
                 (long)0x80000000u, 0x100000000L, (long)0x8000000000000000ul, 0x4000000000000000L,
                 0x123456789abcdefL, -0x123456789abcdefL};
  int n = sizeof(vals) / sizeof(*vals);
  for (int i = 0; i < n; i++) {
    long v = vals[i];
    printf("%lx: %d %d %d %d %d %d %d", v, b_uc(v), b_sc(v), b_us(v), b_i(v), b_u(v), b_l(v), b_ul(v));
    printf(" odd %d big %d", odd(i) ? 1 : 0, big(v) ? 5 : 6);
    if (odd(i) && !big(v))
      printf(" both");
    printf("\n");
  }
  long hx[] = {0, 4, 1L << 40, (1L << 40) | 4, 1L << 60, -1};
  for (int i = 0; i < 6; i++)
    for (int y = 0; y < 3; y++)
      printf("huge %lx %d: %d\n", hx[i], y, huge(hx[i], y));
  printf("sink %d\n", sink);
  return 0;
}
