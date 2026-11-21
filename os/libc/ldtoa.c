// 浮動小数点と 10 進の正確な変換。picolibc の __ldtoa_engine・__dtoa_engine・__atold_engine・
// strtold の代わり
//
// picolibc の実装は long double の中で 10 のべき乗を掛けて近似するので、printf の %Le・%Lg・%Lf の
// 最後の桁や strtold の最後のビットが正しくない。ここでは値を 10 進の桁の並びで正確に持ち、
// 2 のべき乗の掛け算・割り算を桁の上で行って、偶数への丸め（最近接）で正しく丸める。
// カーネルのオブジェクトとして libc.a より先にリンクされるので、picolibc の同名の関数は使われない。
//
// 引数と戻り値の約束は picolibc の libc/stdio/dtoa.h と ldtoa_engine.c、atold_engine.c に合わせる
// （struct dtoa の形、桁数の上限 LDTOA_MAX_DIG = 34、%f のときの桁数の決め方）。
// 確認: tools/picolibc/test_ldtoa.c（Linux の glibc と比べる）

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#ifdef LDTOA_TEST
#define ENGINE(name) test_##name
#else
#define ENGINE(name) name
#endif

// picolibc の libc/stdio/dtoa.h と同じ
#define DTOA_MINUS 1
#define DTOA_ZERO 2
#define DTOA_INF 4
#define DTOA_NAN 8
#define LDTOA_MAX_DIG 34

struct dtoa {
  int32_t exp;
  uint8_t flags;
  char digits[LDTOA_MAX_DIG];
};

// picolibc の _u128（__int128 があればその型、なければ構造体）。どちらも x0（下位）、x1（上位）で渡る
typedef struct {
  uint64_t lo, hi;
} U128;

//
// 10 進の桁の並び: 値 = 0.d[0]d[1]...d[nd-1] × 10^dp
// binary128 の値は最大で約 16,500 桁（2^-16494 の 10 進展開）なので、それより大きく取り、正確に持つ
//

#define MAX_DIGITS 17200 // 左へずらすときの余裕（20 桁）を含む

// コアごとに持つ（複数のコアから同時に strtod・printf を呼べるように）
static _Thread_local struct {
  uint8_t d[MAX_DIGITS];
  int nd;
  int dp;
  bool trunc; // 桁が入りきらず、0 でない桁を捨てた（ここでは起きない大きさにしている）
} dec;

static void dec_trim(void) {
  while (dec.nd > 0 && dec.d[dec.nd - 1] == 0)
    dec.nd--;
  if (dec.nd == 0)
    dec.dp = 0;
}

// 128 ビットの整数を入れる
static void dec_set(uint64_t hi, uint64_t lo) {
  uint8_t tmp[40];
  int n = 0;
  while (hi || lo) {
    // (hi:lo) を 10 で割る
    uint64_t rh = hi % 10;
    hi /= 10;
    // 余り rh × 2^64 + lo を 10 で割る（2^64 = 10 × 1844674407370955161 + 6）
    uint64_t q1 = lo / 10, r1 = lo % 10;
    uint64_t t = rh * 6 + r1;
    lo = rh * 1844674407370955161ULL + q1 + t / 10;
    tmp[n++] = t % 10;
  }
  dec.nd = n;
  dec.dp = n;
  dec.trunc = false;
  for (int i = 0; i < n; i++)
    dec.d[i] = tmp[n - 1 - i];
  dec_trim();
}

// 2^k を掛ける（k は 1〜60）。右の桁から計算し、増える桁（20 桁以内）の分だけ後ろから書く
static void dec_lshift(int k) {
  int end = dec.nd + 20;
  int w = end;
  uint64_t n = 0;
  for (int r = dec.nd - 1; r >= 0; r--) {
    n += (uint64_t)dec.d[r] << k;
    uint64_t q = n / 10;
    dec.d[--w] = n - q * 10;
    n = q;
  }
  while (n) {
    uint64_t q = n / 10;
    dec.d[--w] = n - q * 10;
    n = q;
  }
  int len = end - w;
  memmove(dec.d, dec.d + w, len);
  dec.dp += len - dec.nd;
  dec.nd = len;
  dec_trim();
}

// 2^k で割る（k は 1〜60）。左の桁から割っていく
static void dec_rshift(int k) {
  int r = 0, w = 0;
  uint64_t n = 0;
  for (; n >> k == 0; r++) {
    if (r >= dec.nd) {
      if (n == 0) {
        dec.nd = 0;
        dec.dp = 0;
        return;
      }
      while (n >> k == 0) {
        n *= 10;
        r++;
      }
      break;
    }
    n = n * 10 + dec.d[r];
  }
  dec.dp -= r - 1;
  uint64_t mask = (1ULL << k) - 1;
  for (; r < dec.nd; r++) {
    uint64_t c = n >> k;
    n &= mask;
    dec.d[w++] = c;
    n = n * 10 + dec.d[r];
  }
  while (n) {
    uint64_t c = n >> k;
    n &= mask;
    if (w < MAX_DIGITS)
      dec.d[w++] = c;
    else if (c)
      dec.trunc = true;
    n *= 10;
  }
  dec.nd = w;
  dec_trim();
}

static void dec_shift(int k) {
  while (k > 0) {
    int s = k > 60 ? 60 : k;
    dec_lshift(s);
    k -= s;
  }
  while (k < 0) {
    int s = -k > 60 ? 60 : -k;
    dec_rshift(s);
    k += s;
  }
}

// nd 桁に丸めるとき、切り上げるか（偶数への丸め）
static bool dec_round_up(int nd) {
  if (nd < 0 || nd >= dec.nd)
    return false;
  if (dec.d[nd] == 5 && nd + 1 == dec.nd) {
    // ちょうど半分
    if (dec.trunc)
      return true;
    return nd > 0 && dec.d[nd - 1] % 2 != 0;
  }
  return dec.d[nd] >= 5;
}

// 先頭から nd 桁に丸める（nd が 0 以下なら、10^dp の位で丸める）
static void dec_round(int nd) {
  if (nd < 0) {
    dec.nd = 0;
    return;
  }
  if (nd >= dec.nd)
    return;
  if (dec_round_up(nd)) {
    int i = nd - 1;
    for (; i >= 0; i--) {
      if (dec.d[i] < 9) {
        dec.d[i]++;
        dec.nd = i + 1;
        return;
      }
    }
    // すべて 9 だった: 1 になり、小数点が 1 つ右へ
    dec.d[0] = 1;
    dec.nd = 1;
    dec.dp++;
    return;
  }
  dec.nd = nd;
  dec_trim();
}

//
// binary128 → 10 進（printf）
//

typedef union {
  long double f;
  struct {
    uint64_t lo, hi;
  } w;
} F128;

// 仮数部（整数 hi:lo）× 2^e2 を 10 進にして、dtoa に入れる（printf が使う形。picolibc の約束どおり）
static int to_decimal(uint64_t hi, uint64_t lo, int e2, struct dtoa *dtoa, int max_digits,
                      bool fmode, int max_decimals) {
  if (max_digits > LDTOA_MAX_DIG)
    max_digits = LDTOA_MAX_DIG;
  dec_set(hi, lo);
  dec_shift(e2);

  int decexp = dec.dp - 1; // 先頭の桁の 10 進の指数
  int nd = max_digits;
  if (fmode) {
    // 小数点以下 max_decimals 桁までを出す（10^-max_decimals の位で丸める）
    nd = max_decimals + decexp + 1;
    if (nd > max_digits)
      nd = max_digits;
  }
  dec_round(nd);
  if (dec.nd == 0) {
    // 丸めて 0 になった（%f で、表示する桁より小さい）
    dtoa->digits[0] = '0';
    dtoa->exp = -max_decimals;
    return 1;
  }
  decexp = dec.dp - 1;
  int n = max_digits;
  if (fmode) {
    n = max_decimals + decexp + 1;
    if (n > max_digits)
      n = max_digits;
    if (n < 1)
      n = 1;
  }
  for (int i = 0; i < n; i++)
    dtoa->digits[i] = '0' + (i < dec.nd ? dec.d[i] : 0);
  dtoa->exp = decexp;
  return n;
}

int ENGINE(__ldtoa_engine)(long double x, struct dtoa *dtoa, int max_digits, bool fmode,
                           int max_decimals) {
  F128 v = {.f = x};
  dtoa->flags = 0;
  if (v.w.hi >> 63)
    dtoa->flags |= DTOA_MINUS;
  int e = (v.w.hi >> 48) & 0x7FFF;
  uint64_t fhi = v.w.hi & 0x0000FFFFFFFFFFFFULL, flo = v.w.lo;
  if (e == 0x7FFF) {
    dtoa->flags |= (fhi || flo) ? DTOA_NAN : DTOA_INF;
    return max_digits;
  }
  if (e == 0 && !fhi && !flo) {
    dtoa->flags |= DTOA_ZERO;
    dtoa->digits[0] = '0';
    dtoa->exp = 0;
    return 1;
  }
  // 値 = 仮数部（整数） × 2^e2
  int e2;
  if (e == 0) {
    e2 = 1 - 16383 - 112;
  } else {
    fhi |= 1ULL << 48;
    e2 = e - 16383 - 112;
  }
  return to_decimal(fhi, flo, e2, dtoa, max_digits, fmode, max_decimals);
}

// double（printf の %e・%f・%g）。picolibc の __dtoa_engine（ryu）は、要求された桁数が最短の
// 表記より多いと、最短の桁の後ろを 0 で埋める（例: %.17g の 0.1 が 0.1 になる。正しくは
// 0.10000000000000001）ので、long double と同じ方法で正しく丸める
int ENGINE(__dtoa_engine)(uint64_t x, struct dtoa *dtoa, int max_digits, bool fmode,
                          int max_decimals) {
  dtoa->flags = 0;
  if (x >> 63)
    dtoa->flags |= DTOA_MINUS;
  int e = (x >> 52) & 0x7FF;
  uint64_t frac = x & 0x000FFFFFFFFFFFFFULL;
  if (e == 0x7FF) {
    dtoa->flags |= frac ? DTOA_NAN : DTOA_INF;
    return 0;
  }
  if (e == 0 && !frac) {
    dtoa->flags |= DTOA_ZERO;
    dtoa->digits[0] = '0';
    dtoa->exp = 0;
    return 1;
  }
  int e2;
  if (e == 0) {
    e2 = 1 - 1023 - 52;
  } else {
    frac |= 1ULL << 52;
    e2 = e - 1023 - 52;
  }
  return to_decimal(0, frac, e2, dtoa, max_digits, fmode, max_decimals);
}

//
// 10 進 → binary128（strtold、scanf）。値 = m10 × 10^e10 を、最近接・偶数への丸めで binary128 にする
//

// 2^n を何回掛ければ（割れば）10 進の位が 1 つ動くか（Go の strconv の方法に倣う）
static int shift_for(int dp) {
  static const uint8_t powtab[] = {1, 3, 6, 9, 13, 16, 19, 23, 26};
  return dp < (int)sizeof(powtab) ? powtab[dp] : 27;
}

// 2 進の浮動小数点の形式
typedef struct {
  int mantbits; // 仮数部のビット数（暗黙の 1 を除く）
  int ebits;    // 指数部のビット数
  int dp_over;  // 10 進の桁の位置（dec.dp）がこれより大きければ無限大
  int dp_under; // これより小さければ 0
} Fmt;

static const Fmt fmt_f128 = {112, 15, 4934, -4967};
static const Fmt fmt_f64 = {52, 11, 310, -330};
static const Fmt fmt_f32 = {23, 8, 40, -50};

// 直前の dec_to_bin で丸めが起きたか、丸める前の値が最小の正規化数より小さかったか
// （strtod などの ERANGE に使う。glibc と同じく、アンダーフローは丸める前に判定する）
static _Thread_local bool dec_inexact, dec_tiny;

// (hi:lo) を左に n ビット
static void shl128(uint64_t *hi, uint64_t *lo, int n) {
  if (n >= 64) {
    *hi = *lo << (n - 64);
    *lo = 0;
  } else if (n > 0) {
    *hi = (*hi << n) | (*lo >> (64 - n));
    *lo <<= n;
  }
}

// 指数部が最大（無限大）のビット列
static void bits_inf(const Fmt *f, uint64_t *hi, uint64_t *lo) {
  *hi = 0;
  *lo = (1ULL << f->ebits) - 1;
  shl128(hi, lo, f->mantbits);
}

// dec の値を、最近接・偶数への丸めで形式 f のビット列（符号を除く）にする
static void dec_to_bin(const Fmt *f, uint64_t *rhi, uint64_t *rlo) {
  dec_inexact = true;
  dec_tiny = true;
  *rhi = *rlo = 0;
  if (dec.nd == 0) {
    dec_inexact = false;
    return;
  }
  if (dec.dp > f->dp_over) {
    dec_tiny = false;
    bits_inf(f, rhi, rlo);
    return;
  }
  if (dec.dp < f->dp_under)
    return;

  // [0.5, 1) に収める。値 = dec × 2^exp
  int exp = 0;
  while (dec.dp > 0) {
    int n = shift_for(dec.dp);
    dec_shift(-n);
    exp += n;
  }
  while (dec.dp < 0 || (dec.dp == 0 && dec.d[0] < 5)) {
    int n = dec.dp == 0 ? 1 : shift_for(-dec.dp);
    dec_shift(n);
    exp -= n;
  }
  // 値 = 1.xxx × 2^(exp - 1)
  exp--;
  const int bias = -((1 << (f->ebits - 1)) - 1);
  const int emax = (1 << f->ebits) - 1;
  dec_tiny = exp < bias + 1;
  if (exp < bias + 1) {
    // 非正規化数
    int n = bias + 1 - exp;
    dec_shift(-n);
    exp += n;
  }
  if (exp - bias >= emax) {
    bits_inf(f, rhi, rlo);
    return;
  }

  // mantbits + 1 ビットの整数を取り出す（偶数への丸め）
  dec_shift(1 + f->mantbits);
  int nd = dec.dp;
  bool up = dec_round_up(nd);
  dec_inexact = dec.trunc || dec.nd > nd;
  uint64_t hi = 0, lo = 0;
  for (int i = 0; i < nd; i++) {
    // (hi:lo) = (hi:lo) × 10 + digit
    uint64_t digit = i < dec.nd ? dec.d[i] : 0;
    uint64_t lo8 = lo << 3, lo2 = lo << 1;
    uint64_t hi10 = (hi << 3) + (lo >> 61) + (hi << 1) + (lo >> 63);
    uint64_t nlo = lo8 + lo2;
    hi10 += nlo < lo8;
    uint64_t nlo2 = nlo + digit;
    hi10 += nlo2 < nlo;
    hi = hi10;
    lo = nlo2;
  }
  if (up) {
    lo++;
    hi += lo == 0;
  }
  // 丸めで 2^(mantbits + 1) になった
  int top = f->mantbits + 1;
  bool carry = top >= 64 ? (hi >> (top - 64)) != 0 : (hi != 0 || (lo >> top) != 0);
  if (carry) {
    lo = (lo >> 1) | (hi << 63);
    hi >>= 1;
    exp++;
    if (exp - bias >= emax) {
      bits_inf(f, rhi, rlo);
      return;
    }
  }
  // 暗黙の 1 があるか（なければ非正規化数）
  int mb = f->mantbits;
  bool normal = mb >= 64 ? (hi >> (mb - 64)) & 1 : (lo >> mb) & 1;
  uint64_t ebits = normal ? (uint64_t)(exp - bias) : 0;
  // 仮数部（暗黙の 1 を除く）と指数部を詰める
  if (mb >= 64)
    hi &= (1ULL << (mb - 64)) - 1;
  else {
    hi = 0;
    lo &= (1ULL << mb) - 1;
  }
  uint64_t ehi = 0, elo = ebits;
  shl128(&ehi, &elo, mb);
  *rhi = hi | ehi;
  *rlo = lo | elo;
}

long double ENGINE(__atold_engine)(U128 m10, int e10) {
  F128 r;
  if (!m10.hi && !m10.lo)
    return 0.0L;
  dec_set(m10.hi, m10.lo);
  if (dec.dp + e10 > 4934 || dec.dp + e10 < -4967)
    dec.dp = dec.dp + e10 > 0 ? 4935 : -4968;
  else
    dec.dp += e10;
  dec_to_bin(&fmt_f128, &r.w.hi, &r.w.lo);
  return r.f;
}

//
// strtold: 10 進は数字をすべて読み（picolibc は 32 桁で 10 進のまま丸めるので、正しく丸まらない）、
// 16 進の浮動小数点も、値を正確に 10 進の桁にしてから同じ方法で丸める
//

static bool is_space(int c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

static int lower(int c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static bool match_ci(const char *s, const char *word) {
  for (; *word; s++, word++)
    if (lower((unsigned char)*s) != *word)
      return false;
  return true;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = lower(c);
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

// 指数部（e または p の後）。読めたら値を返し *pp を進める。読めなければ 0（*pp は動かさない）
static long read_exponent(const char **pp) {
  const char *p = *pp + 1;
  int sign = 1;
  if (*p == '+' || *p == '-')
    sign = *p++ == '-' ? -1 : 1;
  if (*p < '0' || *p > '9')
    return 0;
  long e = 0;
  for (; *p >= '0' && *p <= '9'; p++)
    if (e < 1000000)
      e = e * 10 + (*p - '0');
  *pp = p;
  return sign * e;
}

// 文字列を読み、形式 f のビット列（符号を含む）にする。読めた終わりを *endptr に入れる
static void parse_float(const char *nptr, char **endptr, const Fmt *f, uint64_t *rhi,
                        uint64_t *rlo) {
  const char *p = nptr;
  while (is_space((unsigned char)*p))
    p++;
  bool neg = false;
  if (*p == '+' || *p == '-')
    neg = *p++ == '-';
  int sign_bit = f->mantbits + f->ebits; // 符号のビットの位置

  uint64_t hi = 0, lo = 0;
  bool any = false, nonzero = false;
  if (match_ci(p, "inf")) {
    p += match_ci(p, "infinity") ? 8 : 3;
    bits_inf(f, &hi, &lo);
    any = true;
  } else if (match_ci(p, "nan")) {
    p += 3;
    if (*p == '(') {
      const char *q = p + 1;
      while ((*q >= '0' && *q <= '9') || (lower(*q) >= 'a' && lower(*q) <= 'z') || *q == '_')
        q++;
      if (*q == ')')
        p = q + 1;
    }
    // quiet NaN（仮数部の最上位ビット）
    bits_inf(f, &hi, &lo);
    uint64_t qhi = 0, qlo = 1;
    shl128(&qhi, &qlo, f->mantbits - 1);
    hi |= qhi;
    lo |= qlo;
    any = true;
  } else if (p[0] == '0' && lower(p[1]) == 'x' &&
             (hexval((unsigned char)p[2]) >= 0 ||
              (p[2] == '.' && hexval((unsigned char)p[3]) >= 0))) {
    // 16 進: 仮数部の 128 ビットを持ち、はみ出した桁は sticky にする
    p += 2;
    uint64_t mhi = 0, mlo = 0;
    long bexp = 0;
    bool sticky = false, seen_dot = false;
    for (;; p++) {
      if (*p == '.' && !seen_dot) {
        seen_dot = true;
        continue;
      }
      int h = hexval((unsigned char)*p);
      if (h < 0)
        break;
      any = true;
      if (mhi >> 60) {
        // 128 ビットに入らない
        if (h)
          sticky = true;
        if (!seen_dot)
          bexp += 4;
      } else {
        mhi = (mhi << 4) | (mlo >> 60);
        mlo = (mlo << 4) | h;
        if (seen_dot)
          bexp -= 4;
      }
    }
    if (lower(*p) == 'p')
      bexp += read_exponent(&p);
    nonzero = mhi || mlo;
    if (nonzero) {
      int lead = 127;
      while (!((lead >= 64 ? mhi >> (lead - 64) : mlo >> lead) & 1))
        lead--;
      int half = 1 << (f->ebits - 1);
      if (lead + bexp > half + 2) {
        bits_inf(f, &hi, &lo); // 無限大
        dec_inexact = true;
        dec_tiny = false;
      } else if (lead + bexp < -(half + f->mantbits + 10)) {
        dec_inexact = dec_tiny = true; // 最小の非正規化数の半分より小さい: 0
      } else {
        dec_set(mhi, mlo);
        dec_shift((int)bexp);
        if (sticky)
          dec.trunc = true; // 捨てたビットがある: 値はわずかに大きい
        dec_to_bin(f, &hi, &lo);
      }
    }
  } else {
    // 10 進: 数字をすべて dec に入れる（先頭の 0 は除く）
    dec.nd = 0;
    dec.dp = 0;
    dec.trunc = false;
    bool seen_dot = false, leading = true;
    for (;; p++) {
      if (*p == '.' && !seen_dot) {
        seen_dot = true;
        continue;
      }
      if (*p < '0' || *p > '9')
        break;
      any = true;
      int c = *p - '0';
      if (leading && c == 0) {
        if (seen_dot)
          dec.dp--;
        continue;
      }
      leading = false;
      if (!seen_dot)
        dec.dp++;
      if (dec.nd < MAX_DIGITS - 40)
        dec.d[dec.nd++] = c;
      else if (c)
        dec.trunc = true;
    }
    if (any && lower(*p) == 'e') {
      long e = read_exponent(&p);
      long dp = dec.dp + e;
      dec.dp = dp > 100000 ? 100000 : dp < -100000 ? -100000 : (int)dp;
    }
    dec_trim();
    nonzero = dec.nd != 0;
    dec_to_bin(f, &hi, &lo);
  }

  if (!any) {
    if (endptr)
      *endptr = (char *)nptr;
    *rhi = *rlo = 0;
    return;
  }
  if (endptr)
    *endptr = (char *)p;
  // 範囲外: 無限大になった、または丸めが起きて 0・非正規化数になった（glibc と同じ。
  // ちょうど表せる非正規化数はアンダーフローではない）
  if (nonzero) {
    uint64_t ehi = hi, elo = lo;
    // 指数部を取り出す
    int mb = f->mantbits;
    uint64_t e = mb >= 64 ? ehi >> (mb - 64) : (elo >> mb) | (ehi << (64 - mb));
    e &= (1ULL << f->ebits) - 1;
    if (e == (1ULL << f->ebits) - 1 || (dec_tiny && dec_inexact))
      errno = ERANGE;
  }
  if (neg) {
    uint64_t shi = 0, slo = 1;
    shl128(&shi, &slo, sign_bit);
    hi |= shi;
    lo |= slo;
  }
  *rhi = hi;
  *rlo = lo;
}

long double ENGINE(strtold)(const char *nptr, char **endptr) {
  F128 r;
  parse_float(nptr, endptr, &fmt_f128, &r.w.hi, &r.w.lo);
  return r.f;
}

double ENGINE(strtod)(const char *nptr, char **endptr) {
  uint64_t hi, lo;
  parse_float(nptr, endptr, &fmt_f64, &hi, &lo);
  union { uint64_t u; double d; } v = {.u = lo};
  return v.d;
}

float ENGINE(strtof)(const char *nptr, char **endptr) {
  uint64_t hi, lo;
  parse_float(nptr, endptr, &fmt_f32, &hi, &lo);
  union { uint32_t u; float f; } v = {.u = (uint32_t)lo};
  return v.f;
}
