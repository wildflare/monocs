// __builtin_*_overflow（ir.inc の ir_ckd_arith）が GCC と同じ結果か（run.sh の 8c）
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
static unsigned long h;
static unsigned long long uv[] = {0, 1, 2, 3, 0xffffffffULL, 0x100000000ULL, ULLONG_MAX / 2, ULLONG_MAX / 3, ULLONG_MAX - 1, ULLONG_MAX};
static long long sv[] = {0, 1, -1, 2, -2, INT_MAX, INT_MIN, LLONG_MAX, LLONG_MIN, LLONG_MAX / 2, LLONG_MIN / 2, 3000000000LL};
static unsigned uw[] = {0, 1, 2, 0xffff, 0x10000, 0x7fffffff, 0x80000000u, UINT_MAX - 1, UINT_MAX};
static int iw[] = {0, 1, -1, 2, -2, 46341, -46341, INT_MAX, INT_MIN, INT_MAX / 2};
void fu(void) {
  for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) {
    unsigned long long r; int o;
    o = __builtin_add_overflow(uv[i], uv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_sub_overflow(uv[i], uv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_mul_overflow(uv[i], uv[j], &r); h = h * 31 + r + o * 7;
  }
}
void fs(void) {
  for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) {
    long long r; int o;
    o = __builtin_add_overflow(sv[i], sv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_sub_overflow(sv[i], sv[j], &r); h = h * 31 + r + o * 7;
  }
}
void fsm(void) {
  for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) {
    long long r; int o;
    o = __builtin_mul_overflow(sv[i], sv[j], &r); h = h * 31 + r + o * 7;
  }
}
void fw(void) {
  for (int i = 0; i < 9; i++) for (int j = 0; j < 9; j++) {
    unsigned r; int o;
    o = __builtin_add_overflow(uw[i], uw[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_sub_overflow(uw[i], uw[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_mul_overflow(uw[i], uw[j], &r); h = h * 31 + r + o * 7;
  }
}
void fi(void) {
  for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) {
    int r; int o;
    o = __builtin_add_overflow(iw[i], iw[j], &r); h = h * 31 + (unsigned)r + o * 7;
    o = __builtin_sub_overflow(iw[i], iw[j], &r); h = h * 31 + (unsigned)r + o * 7;
    o = __builtin_mul_overflow(iw[i], iw[j], &r); h = h * 31 + (unsigned)r + o * 7;
  }
}
int main(void) {
  fu(); printf("u %lx\n", h); fs(); printf("s %lx\n", h); fsm(); printf("sm %lx\n", h); fw(); printf("w %lx\n", h); fi(); printf("i %lx\n", h);
  return 0;
}
int unused(void) {
  unsigned long long uv[] = {0, 1, 2, 3, 0xffffffffULL, 0x100000000ULL, ULLONG_MAX / 2, ULLONG_MAX / 3, ULLONG_MAX - 1, ULLONG_MAX};
  long long sv[] = {0, 1, -1, 2, -2, INT_MAX, INT_MIN, LLONG_MAX, LLONG_MIN, LLONG_MAX / 2, LLONG_MIN / 2, 3000000000LL};
  unsigned uw[] = {0, 1, 2, 0xffff, 0x10000, 0x7fffffff, 0x80000000u, UINT_MAX - 1, UINT_MAX};
  int iw[] = {0, 1, -1, 2, -2, 46341, -46341, INT_MAX, INT_MIN, INT_MAX / 2};
  unsigned long h = 0;
  for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) {
    unsigned long long r; int o;
    o = __builtin_add_overflow(uv[i], uv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_sub_overflow(uv[i], uv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_mul_overflow(uv[i], uv[j], &r); h = h * 31 + r + o * 7;
  }
  for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) {
    long long r; int o;
    o = __builtin_add_overflow(sv[i], sv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_sub_overflow(sv[i], sv[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_mul_overflow(sv[i], sv[j], &r); h = h * 31 + r + o * 7;
  }
  for (int i = 0; i < 9; i++) for (int j = 0; j < 9; j++) {
    unsigned r; int o;
    o = __builtin_add_overflow(uw[i], uw[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_sub_overflow(uw[i], uw[j], &r); h = h * 31 + r + o * 7;
    o = __builtin_mul_overflow(uw[i], uw[j], &r); h = h * 31 + r + o * 7;
  }
  for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) {
    int r; int o;
    o = __builtin_add_overflow(iw[i], iw[j], &r); h = h * 31 + (unsigned)r + o * 7;
    o = __builtin_sub_overflow(iw[i], iw[j], &r); h = h * 31 + (unsigned)r + o * 7;
    o = __builtin_mul_overflow(iw[i], iw[j], &r); h = h * 31 + (unsigned)r + o * 7;
  }
  printf("%lx\n", h);
  return 0;
}
