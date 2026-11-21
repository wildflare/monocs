// os/libc/ldtoa.c を Linux の glibc（正しく丸める）と比べる
//   gcc -O2 -DLDTOA_TEST -o test_ldtoa tools/picolibc/test_ldtoa.c os/libc/ldtoa.c && ./test_ldtoa [回数]
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dtoa {
  int32_t exp;
  uint8_t flags;
  char digits[34];
};
typedef struct {
  uint64_t lo, hi;
} U128;
int test___ldtoa_engine(long double x, struct dtoa *dtoa, int max_digits, bool fmode, int max_decimals);
long double test___atold_engine(U128 m10, int e10);
long double test_strtold(const char *nptr, char **endptr);
double test_strtod(const char *nptr, char **endptr);
float test_strtof(const char *nptr, char **endptr);
int test___dtoa_engine(uint64_t x, struct dtoa *dtoa, int max_digits, bool fmode, int max_decimals);
#include <errno.h>

typedef union {
  long double f;
  struct {
    uint64_t lo, hi;
  } w;
} F128;

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static long double rnd_ld(int range) {
  F128 v;
  uint64_t e = range ? 16383 + rnd() % (2 * range + 1) - range : rnd() % 0x7FFF;
  if (!range && rnd() % 8 == 0)
    e = rnd() % 3; // 非正規化数の近く
  v.w.hi = (rnd() & 0x8000FFFFFFFFFFFFULL) | (e << 48);
  v.w.lo = rnd();
  if (rnd() % 4 == 0) { // 短い仮数部（ちょうど表せる 10 進の値が多い）
    v.w.lo = 0;
    v.w.hi &= ~0xFFFFFFFFFULL;
  }
  return v.f;
}

static long fails, checks;

// picolibc の vfprintf_float.c の %f と同じ規則で、桁から文字列を作る
static void fmt_f(char *out, struct dtoa *d, int ndigs, int prec) {
  char *p = out;
  if (d->flags & 1)
    *p++ = '-';
  int exp = d->exp;
  int n = exp > 0 ? exp : 0;
  char c;
  do {
    if (n == -1)
      *p++ = '.';
    if (0 <= exp - n && exp - n < ndigs)
      c = d->digits[exp - n];
    else
      c = '0';
    if (--n < -prec)
      break;
    *p++ = c;
  } while (1);
  *p++ = c;
  *p = 0;
}

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 100000;
  char buf[200], want[200];

  for (long i = 0; i < n; i++) {
    // %Le: 有効数字 nd 桁
    long double x = rnd_ld(i % 3 == 0 ? 60 : 0);
    int nd = 1 + rnd() % 34;
    struct dtoa d;
    int got = test___ldtoa_engine(x, &d, nd, false, 0);
    snprintf(want, sizeof(want), "%.*Le", nd - 1, x);
    // 自分の結果を同じ形にする
    char *p = buf;
    if (d.flags & 1)
      *p++ = '-';
    *p++ = d.digits[0];
    if (got > 1) {
      *p++ = '.';
      memcpy(p, d.digits + 1, got - 1);
      p += got - 1;
    }
    int ee = (d.flags & 2) ? 0 : d.exp;
    sprintf(p, "e%c%02d", ee < 0 ? '-' : '+', ee < 0 ? -ee : ee);
    checks++;
    if (strcmp(buf, want)) {
      if (fails++ < 20)
        printf("FAIL e nd=%d: got %s want %s\n", nd, buf, want);
    }

    // %Lf: 小数点以下 prec 桁（34 桁に収まる範囲）
    long double y = rnd_ld(40);
    int prec = rnd() % 25;
    snprintf(want, sizeof(want), "%.*Lf", prec, y);
    int digits_needed = (int)strlen(want) - (y < 0) - (prec > 0);
    if (digits_needed <= 34) {
      got = test___ldtoa_engine(y, &d, 34, true, prec);
      if (d.flags & 2) {
        // 0: picolibc の printf は 0 を 1 桁として出す
        got = 1;
      }
      fmt_f(buf, &d, got, prec);
      checks++;
      if (strcmp(buf, want)) {
        if (fails++ < 20)
          printf("FAIL f prec=%d: got %s want %s\n", prec, buf, want);
      }
    }

    // strtold: 仮数部（38 桁まで）× 10^e
    U128 m;
    int e10;
    if (i % 4 == 0) {
      // 2^113 付近の整数（ちょうど中間の値を含む）
      m.hi = (1ULL << 49) | (rnd() & 0xFFFF);
      m.lo = rnd() | 1;
      e10 = 0;
    } else {
      int digs = 1 + rnd() % 38;
      m.hi = m.lo = 0;
      for (int k = 0; k < digs; k++) {
        // m = m * 10 + digit（128 ビット）
        unsigned __int128 v = ((unsigned __int128)m.hi << 64) | m.lo;
        v = v * 10 + rnd() % 10;
        m.hi = v >> 64;
        m.lo = (uint64_t)v;
      }
      e10 = (int)(rnd() % 10000) - 5000;
      if (i % 4 == 1)
        e10 = (int)(rnd() % 80) - 40;
    }
    unsigned __int128 mv = ((unsigned __int128)m.hi << 64) | m.lo;
    char digits[64];
    int len = 0;
    do {
      digits[len++] = '0' + (int)(mv % 10);
      mv /= 10;
    } while (mv);
    char str[100];
    for (int k = 0; k < len; k++)
      str[k] = digits[len - 1 - k];
    sprintf(str + len, "e%d", e10);
    F128 a = {.f = test___atold_engine(m, e10)}, b = {.f = strtold(str, NULL)};
    checks++;
    if (a.w.hi != b.w.hi || a.w.lo != b.w.lo) {
      if (fails++ < 20)
        printf("FAIL strtold %s: got %016llx%016llx want %016llx%016llx\n", str,
               (unsigned long long)a.w.hi, (unsigned long long)a.w.lo,
               (unsigned long long)b.w.hi, (unsigned long long)b.w.lo);
    }
  }
  // double の printf（17 桁以内の %e と %f）
  for (long i = 0; i < n; i++) {
    union { double d; uint64_t u; } v;
    v.u = rnd();
    if (i % 3 == 0)
      v.u = (v.u & 0x800FFFFFFFFFFFFFULL) | ((1023 + rnd() % 120 - 60) << 52);
    if (i % 7 == 0)
      v.u &= ~0xFFFFFFFFULL; // 短い仮数部
    if (((v.u >> 52) & 0x7FF) == 0x7FF)
      continue;
    struct dtoa d;
    int nd = 1 + rnd() % 17;
    char buf[400], want[400];
    int got = test___dtoa_engine(v.u, &d, nd, false, 0);
    snprintf(want, sizeof(want), "%.*e", nd - 1, v.d);
    char *p = buf;
    if (d.flags & 1)
      *p++ = '-';
    *p++ = d.digits[0];
    if (got > 1) {
      *p++ = '.';
      memcpy(p, d.digits + 1, got - 1);
      p += got - 1;
    }
    int ee = (d.flags & 2) ? 0 : d.exp;
    sprintf(p, "e%c%02d", ee < 0 ? '-' : '+', ee < 0 ? -ee : ee);
    checks++;
    if (strcmp(buf, want) && fails++ < 40)
      printf("FAIL double e nd=%d: got %s want %s\n", nd, buf, want);
    int prec = rnd() % 20;
    snprintf(want, sizeof(want), "%.*f", prec, v.d);
    int digits_needed = (int)strlen(want) - (v.d < 0) - (prec > 0);
    // 17 桁以内（それより先は picolibc の printf が 0 にするので比べない）
    if (digits_needed <= 17) {
      got = test___dtoa_engine(v.u, &d, 17, true, prec);
      if (d.flags & 2)
        got = 1;
      fmt_f(buf, &d, got, prec);
      checks++;
      if (strcmp(buf, want) && fails++ < 40)
        printf("FAIL double f prec=%d: got %s want %s\n", prec, buf, want);
    }
  }

  // strtold そのもの（長い 10 進、16 進、inf・nan、不完全な入力）。値・endptr・errno を比べる
  static const char *const fixed[] = {
    "0", "-0", "  +1.5", "1e", "1e+", ".e5", "0x", "0x.", "0x1p", "0x1.8p3", "-0X.8P-1", "inf",
    "-Infinity", "infinit", "nan", "NaN(abc_1)", "nan(", "1e99999", "1e-99999", "0x1p-16494",
    "0x1p-16495", "0x1p-16496", "0x1.ffffffffffffffffffffffffffff8p16383", "0x1p16384", ".5",
    "5.", "00000000000000000000001", "0.000000000000000000000000000000000000000000001e45",
    "3.14159265358979323846264338327950288419716939937510", "1.18973149535723176508575932662800702e4932",
    "1.18973149535723176508575932662800703e4932", "6.475175119438025110924438958227646552e-4966",
    "3.2375875597190125554622194791138232e-4966", "3.2375875597190125554622194791138233e-4966", "abc",
    // double・float の境界（ちょうど中間と、そのわずかに上下）
    "1.00000000000000011102230246251565404236316680908203125",
    "1.000000000000000111022302462515654042363166809082031251",
    "1.00000000000000011102230246251565404236316680908203124", "9007199254740993",
    "9007199254740993.0000000000001", "2.2250738585072011e-308", "2.2250738585072012e-308",
    "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324",
    "1.7976931348623158e308", "1.7976931348623159e308", "1e23", "8.533e+68", "4.1006e-184",
    "9.998e+307", "9.9538452227e-280", "6.47660115e-260", "7.4e+47", "5.92e+48", "7.35e+66",
    "1.000000059604644775390625", "1.0000000596046447753906250000001", "16777217",
    "3.4028235677973366e38", "3.4028235677973367e38", "1.40129846e-45", "7.0064923e-46",
    "7.00649232162408535461864791644958065640130970938257885878534141944895541342930300743319094181060791015625e-46",
    "1.17549421e-38", "1.1754942e-38",
  };
  for (int k = 0; k < (int)(sizeof(fixed) / sizeof(*fixed)) + (int)(n / 4); k++) {
    char str[300];
    if (k < (int)(sizeof(fixed) / sizeof(*fixed))) {
      snprintf(str, sizeof(str), "%s", fixed[k]);
    } else {
      int kind = rnd() % 3;
      if (kind == 0) {
        // 値を 40 桁で書き、最後の数桁を変える（境界の近く）
        long double x = rnd_ld(k % 2 ? 0 : 50);
        snprintf(str, sizeof(str), "%.*Le", 34 + (int)(rnd() % 10), x);
        char *e = strchr(str, 'e');
        if (e && e - str > 4)
          e[-1] = '0' + rnd() % 10;
      } else if (kind == 1) {
        // 長い 10 進
        int len = 0, digs = 1 + rnd() % 80;
        if (rnd() % 2) str[len++] = '-';
        for (int j = 0; j < digs; j++) {
          if (j == (int)(rnd() % (digs + 1))) str[len++] = '.';
          str[len++] = '0' + rnd() % 10;
        }
        int er = k % 3 == 0 ? 700 : k % 3 == 1 ? 90 : 9900;
        sprintf(str + len, "e%d", (int)(rnd() % er) - er / 2);
      } else {
        // 16 進
        int len = sprintf(str, "0x");
        int digs = 1 + rnd() % 40;
        for (int j = 0; j < digs; j++) {
          if (j == 5) str[len++] = '.';
          str[len++] = "0123456789abcdef"[rnd() % 16];
        }
        sprintf(str + len, "p%d", (int)(rnd() % 33000) - 16500);
      }
    }
    char *e1, *e2;
    errno = 0;
    F128 a = {.f = test_strtold(str, &e1)};
    int er1 = errno;
    errno = 0;
    F128 b = {.f = strtold(str, &e2)};
    int er2 = errno;
    // NaN は符号と quiet ビットだけ比べる（ペイロードは実装による）
    bool nan_a = ((a.w.hi >> 48) & 0x7FFF) == 0x7FFF && ((a.w.hi << 16) || a.w.lo);
    bool nan_b = ((b.w.hi >> 48) & 0x7FFF) == 0x7FFF && ((b.w.hi << 16) || b.w.lo);
    bool same = nan_a && nan_b ? (a.w.hi >> 63) == (b.w.hi >> 63)
                               : a.w.hi == b.w.hi && a.w.lo == b.w.lo;
    // strtod・strtof も同じ入力で
    {
      char *f1, *f2;
      errno = 0;
      union { double d; uint64_t u; } da = {.d = test_strtod(str, &f1)};
      int r1 = errno;
      errno = 0;
      union { double d; uint64_t u; } db = {.d = strtod(str, &f2)};
      int r2 = errno;
      bool dn = ((da.u >> 52) & 0x7FF) == 0x7FF && (da.u << 12) && ((db.u >> 52) & 0x7FF) == 0x7FF && (db.u << 12);
      checks++;
      if ((dn ? (da.u >> 63) != (db.u >> 63) : da.u != db.u) || f1 != f2 || r1 != r2) {
        if (fails++ < 40)
          printf("FAIL strtod(\"%s\"): got %016llx end+%td errno %d, want %016llx end+%td errno %d\n", str,
                 (unsigned long long)da.u, f1 - str, r1, (unsigned long long)db.u, f2 - str, r2);
      }
      errno = 0;
      union { float f; uint32_t u; } fa = {.f = test_strtof(str, &f1)};
      r1 = errno;
      errno = 0;
      union { float f; uint32_t u; } fb = {.f = strtof(str, &f2)};
      r2 = errno;
      bool fn = ((fa.u >> 23) & 0xFF) == 0xFF && (fa.u << 9) && ((fb.u >> 23) & 0xFF) == 0xFF && (fb.u << 9);
      checks++;
      if ((fn ? (fa.u >> 31) != (fb.u >> 31) : fa.u != fb.u) || f1 != f2 || r1 != r2) {
        if (fails++ < 40)
          printf("FAIL strtof(\"%s\"): got %08x end+%td errno %d, want %08x end+%td errno %d\n", str,
                 fa.u, f1 - str, r1, fb.u, f2 - str, r2);
      }
    }
    checks++;
    if (!same || e1 != e2 || er1 != er2) {
      if (fails++ < 40)
        printf("FAIL strtold(\"%s\"): got %016llx%016llx end+%td errno %d, want %016llx%016llx end+%td errno %d\n",
               str, (unsigned long long)a.w.hi, (unsigned long long)a.w.lo, e1 - str, er1,
               (unsigned long long)b.w.hi, (unsigned long long)b.w.lo, e2 - str, er2);
    }
  }
  printf("%ld checks, %ld failures\n", checks, fails);
  return fails != 0;
}
