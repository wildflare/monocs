// cc/rt/softfp128.c を、Pi 5 の libgcc とビット単位で比べる（結果と FPSR の例外フラグ、4 つの丸めモード）
//   gcc -O2 -DRT_TEST -o test_softfp test_softfp.c softfp128.c && ./test_softfp [回数]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union {
  long double f;
  struct {
    uint64_t lo, hi;
  } w;
} F128;

// libgcc
long double __addtf3(long double, long double);
long double __subtf3(long double, long double);
long double __multf3(long double, long double);
long double __divtf3(long double, long double);
int __eqtf2(long double, long double);
int __netf2(long double, long double);
int __lttf2(long double, long double);
int __letf2(long double, long double);
int __gttf2(long double, long double);
int __getf2(long double, long double);
int __fixtfsi(long double);
long __fixtfdi(long double);
unsigned __fixunstfsi(long double);
unsigned long __fixunstfdi(long double);
long double __floatsitf(int);
long double __floatditf(long);
long double __floatunsitf(unsigned);
long double __floatunditf(unsigned long);
long double __extendsftf2(float);
long double __extenddftf2(double);
float __trunctfsf2(long double);
double __trunctfdf2(long double);

// cc/rt/softfp128.c（RT_TEST のときの名前）
long double rt___addtf3(long double, long double);
long double rt___subtf3(long double, long double);
long double rt___multf3(long double, long double);
long double rt___divtf3(long double, long double);
int rt___eqtf2(long double, long double);
int rt___netf2(long double, long double);
int rt___lttf2(long double, long double);
int rt___letf2(long double, long double);
int rt___gttf2(long double, long double);
int rt___getf2(long double, long double);
int rt___fixtfsi(long double);
long rt___fixtfdi(long double);
unsigned rt___fixunstfsi(long double);
unsigned long rt___fixunstfdi(long double);
long double rt___floatsitf(int);
long double rt___floatditf(long);
long double rt___floatunsitf(unsigned);
long double rt___floatunditf(unsigned long);
long double rt___extendsftf2(float);
long double rt___extenddftf2(double);
float rt___trunctfsf2(long double);
double rt___trunctfdf2(long double);

static uint64_t fpsr_get(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, fpsr" : "=r"(v));
  return v & 0x1F;
}
static void fpsr_clear(void) {
  __asm__ volatile("msr fpsr, %0" : : "r"((uint64_t)0));
}
static void set_rmode(int rm) {
  uint64_t v;
  __asm__ volatile("mrs %0, fpcr" : "=r"(v));
  v = (v & ~(3ULL << 22)) | ((uint64_t)rm << 22);
  __asm__ volatile("msr fpcr, %0" : : "r"(v));
}

static uint64_t rng = 0x123456789ABCDEFULL;
static uint64_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

// いろいろな値（特殊な値と境界を多めに）
static F128 rnd_f128(void) {
  F128 x;
  uint64_t r = rnd();
  int kind = r % 16;
  uint64_t sign = (rnd() & 1) << 63;
  uint64_t fhi = rnd() & 0xFFFFFFFFFFFFULL, flo = rnd();
  if ((r >> 8) % 4 == 0) {
    // 仮数部の端だけ立てる
    fhi = (r >> 12) % 3 == 0 ? 0 : (r >> 12) % 3 == 1 ? 0xFFFFFFFFFFFFULL : fhi;
    flo = (r >> 16) % 3 == 0 ? 0 : (r >> 16) % 3 == 1 ? ~0ULL : (flo & 0xFF);
  }
  uint64_t e;
  switch (kind) {
  case 0: e = 0; break;                                  // 0 と非正規化数
  case 1: e = 0x7FFF; break;                             // 無限大と NaN
  case 2: e = 1 + rnd() % 3; break;                      // 最小の正規化数の近く
  case 3: e = 0x7FFE - rnd() % 3; break;                 // 最大の近く
  case 4: fhi = flo = 0; e = rnd() % 0x8000; break;      // 2 のべき乗
  case 5: e = 16383 + rnd() % 130 - 65; break;           // 整数の範囲
  default: e = 16383 + (int64_t)(rnd() % 400) - 200; break;
  }
  x.w.hi = sign | (e << 48) | fhi;
  x.w.lo = flo;
  return x;
}

// b を a の近く（指数が近い）にする
static F128 near(F128 a) {
  F128 b = rnd_f128();
  if (rnd() % 2) {
    int64_t e = (a.w.hi >> 48) & 0x7FFF;
    e += (int64_t)(rnd() % 5) - 2;
    if (e < 0)
      e = 0;
    if (e > 0x7FFF)
      e = 0x7FFF;
    b.w.hi = (b.w.hi & ~(0x7FFFULL << 48)) | ((uint64_t)e << 48);
    if (rnd() % 2) {
      b.w.hi = (b.w.hi & (0xFFFFULL << 48)) | (a.w.hi & 0xFFFFFFFFFFFFULL);
      b.w.lo = a.w.lo ^ (rnd() % 8);
    }
  }
  return b;
}

static long fails, total;

static void report(const char *op, int rm, F128 a, F128 b, const char *got, const char *want) {
  if (fails < 30)
    printf("FAIL %s rm=%d a=%016llx%016llx b=%016llx%016llx got %s want %s\n", op, rm,
           (unsigned long long)a.w.hi, (unsigned long long)a.w.lo, (unsigned long long)b.w.hi,
           (unsigned long long)b.w.lo, got, want);
  fails++;
}

#define CHECK_F128(name, expr_rt, expr_ref)                                                   \
  do {                                                                                        \
    fpsr_clear();                                                                             \
    F128 r1 = {.f = expr_rt};                                                                 \
    uint64_t f1 = fpsr_get();                                                                 \
    fpsr_clear();                                                                             \
    F128 r2 = {.f = expr_ref};                                                                \
    uint64_t f2 = fpsr_get();                                                                 \
    total++;                                                                                  \
    if (r1.w.hi != r2.w.hi || r1.w.lo != r2.w.lo || f1 != f2) {                               \
      char g[64], w[64];                                                                      \
      snprintf(g, sizeof(g), "%016llx%016llx/%02llx", (unsigned long long)r1.w.hi,            \
               (unsigned long long)r1.w.lo, (unsigned long long)f1);                          \
      snprintf(w, sizeof(w), "%016llx%016llx/%02llx", (unsigned long long)r2.w.hi,            \
               (unsigned long long)r2.w.lo, (unsigned long long)f2);                          \
      report(name, rm, a, b, g, w);                                                           \
    }                                                                                         \
  } while (0)

#define CHECK_INT(name, type, expr_rt, expr_ref)                                              \
  do {                                                                                        \
    fpsr_clear();                                                                             \
    type r1 = expr_rt;                                                                        \
    uint64_t f1 = fpsr_get();                                                                 \
    fpsr_clear();                                                                             \
    type r2 = expr_ref;                                                                       \
    uint64_t f2 = fpsr_get();                                                                 \
    total++;                                                                                  \
    if (r1 != r2 || f1 != f2) {                                                               \
      char g[64], w[64];                                                                      \
      snprintf(g, sizeof(g), "%llx/%02llx", (unsigned long long)r1, (unsigned long long)f1);  \
      snprintf(w, sizeof(w), "%llx/%02llx", (unsigned long long)r2, (unsigned long long)f2);  \
      report(name, rm, a, b, g, w);                                                           \
    }                                                                                         \
  } while (0)

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 1000000;
  const char *only = argc > 2 ? argv[2] : NULL;
#define WANT(op) (!only || !strcmp(only, op))
  for (long i = 0; i < n; i++) {
    int rm = i % 4;
    set_rmode(rm);
    F128 a = rnd_f128();
    F128 b = (i / 4) % 2 ? near(a) : rnd_f128();
    if (WANT("add")) CHECK_F128("add", rt___addtf3(a.f, b.f), __addtf3(a.f, b.f));
    if (WANT("sub")) CHECK_F128("sub", rt___subtf3(a.f, b.f), __subtf3(a.f, b.f));
#ifdef TEST_ALL
    if (WANT("mul")) CHECK_F128("mul", rt___multf3(a.f, b.f), __multf3(a.f, b.f));
    if (WANT("div")) CHECK_F128("div", rt___divtf3(a.f, b.f), __divtf3(a.f, b.f));
    if (WANT("cmp")) {
      CHECK_INT("eq", int, rt___eqtf2(a.f, b.f), __eqtf2(a.f, b.f));
      CHECK_INT("ne", int, rt___netf2(a.f, b.f), __netf2(a.f, b.f));
      CHECK_INT("lt", int, rt___lttf2(a.f, b.f), __lttf2(a.f, b.f));
      CHECK_INT("le", int, rt___letf2(a.f, b.f), __letf2(a.f, b.f));
      CHECK_INT("gt", int, rt___gttf2(a.f, b.f), __gttf2(a.f, b.f));
      CHECK_INT("ge", int, rt___getf2(a.f, b.f), __getf2(a.f, b.f));
      CHECK_INT("eq_self", int, rt___eqtf2(a.f, a.f), __eqtf2(a.f, a.f));
    }
    if (WANT("conv")) {
      CHECK_INT("fixtfsi", int, rt___fixtfsi(a.f), __fixtfsi(a.f));
      CHECK_INT("fixtfdi", long, rt___fixtfdi(a.f), __fixtfdi(a.f));
      CHECK_INT("fixunstfsi", unsigned, rt___fixunstfsi(a.f), __fixunstfsi(a.f));
      CHECK_INT("fixunstfdi", unsigned long, rt___fixunstfdi(a.f), __fixunstfdi(a.f));
      int si = (int)rnd() >> (rnd() % 32);
      long di = (long)rnd() >> (rnd() % 64);
      CHECK_F128("floatsitf", rt___floatsitf(si), __floatsitf(si));
      CHECK_F128("floatditf", rt___floatditf(di), __floatditf(di));
      CHECK_F128("floatunsitf", rt___floatunsitf((unsigned)si), __floatunsitf((unsigned)si));
      CHECK_F128("floatunditf", rt___floatunditf((unsigned long)di), __floatunditf((unsigned long)di));
      union { uint32_t u; float f; } sf = {.u = (uint32_t)rnd()};
      union { uint64_t u; double d; } df = {.u = rnd()};
      if (rnd() % 4 == 0) sf.u = (sf.u & 0x807FFFFF) | ((rnd() % 2) ? 0 : 0x7F800000);
      if (rnd() % 4 == 0) df.u = (df.u & 0x800FFFFFFFFFFFFFULL) | ((rnd() % 2) ? 0 : 0x7FF0000000000000ULL);
      CHECK_F128("extendsftf2", rt___extendsftf2(sf.f), __extendsftf2(sf.f));
      CHECK_F128("extenddftf2", rt___extenddftf2(df.d), __extenddftf2(df.d));
      union { float f; uint32_t u; } t1, t2;
      fpsr_clear(); t1.f = rt___trunctfsf2(a.f); uint64_t g1 = fpsr_get();
      fpsr_clear(); t2.f = __trunctfsf2(a.f); uint64_t g2 = fpsr_get();
      total++;
      if (t1.u != t2.u || g1 != g2) {
        char g[32], w[32];
        snprintf(g, sizeof(g), "%08x/%02llx", t1.u, (unsigned long long)g1);
        snprintf(w, sizeof(w), "%08x/%02llx", t2.u, (unsigned long long)g2);
        report("trunctfsf2", rm, a, b, g, w);
      }
      union { double d; uint64_t u; } d1, d2;
      fpsr_clear(); d1.d = rt___trunctfdf2(a.f); g1 = fpsr_get();
      fpsr_clear(); d2.d = __trunctfdf2(a.f); g2 = fpsr_get();
      total++;
      if (d1.u != d2.u || g1 != g2) {
        char g[40], w[40];
        snprintf(g, sizeof(g), "%016llx/%02llx", (unsigned long long)d1.u, (unsigned long long)g1);
        snprintf(w, sizeof(w), "%016llx/%02llx", (unsigned long long)d2.u, (unsigned long long)g2);
        report("trunctfdf2", rm, a, b, g, w);
      }
    }
#endif
  }
  set_rmode(0);
  printf("%ld checks, %ld failures\n", total, fails);
  return fails != 0;
}
