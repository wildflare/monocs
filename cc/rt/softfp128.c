// long double（IEEE 754 binary128）のソフトウェア演算。libgcc の同名の関数の代わり
//
// slimcc-a64 は long double の演算・比較・変換をこれらの関数の呼び出しにする（codegen.c）。
// 結果、丸めモード（FPCR.RMode）、例外フラグ（FPSR の IOC・DZC・OFC・UFC・IXC）は libgcc と同じにする。
// 確認: cc/rt/test_softfp.c（Pi 5 上で libgcc と結果・フラグをビット単位で比べる）
//
// 128 ビットの値は 64 ビット 2 つ（hi、lo）で表す（slimcc は __int128 を _BitInt(128) として扱い遅いため）。
// 出典: IEEE 754-2008（binary128 の形式、丸め、例外）、ARM DDI 0487（FPCR、FPSR のビットの位置）

#include <stdbool.h>
#include <stdint.h>

#ifdef RT_TEST
#define RT(name) rt_##name // テストでは libgcc と並べて呼ぶので、名前を変える
#else
#define RT(name) name
#endif

typedef union {
  long double f;
  struct {
    uint64_t lo, hi;
  } w;
} F128;

typedef struct {
  uint64_t hi, lo;
} U128;

// 例外フラグ（FPSR のビット）
#define EX_INVALID 0x01
#define EX_DIVZERO 0x02
#define EX_OVERFLOW 0x04
#define EX_UNDERFLOW 0x08
#define EX_INEXACT 0x10

enum { RM_NEAREST, RM_UP, RM_DOWN, RM_ZERO };

#define EXP_MAX 0x7FFF
#define BIAS 16383
#define FRAC_HI_MASK 0x0000FFFFFFFFFFFFULL // hi の仮数部（48 ビット）
#define HIDDEN_HI 0x0001000000000000ULL    // hi の暗黙の 1（ビット 112）
#define QUIET_HI 0x0000800000000000ULL     // NaN の quiet ビット

// FPCR の丸めモード、FPSR の例外フラグ（マクロにする。理由は下の 128 ビットの操作と同じ）
#define round_mode() ({                                                       \
  uint64_t rm_fpcr;                                                           \
  __asm__ volatile("mrs %0, fpcr" : "=r"(rm_fpcr));                           \
  (int)((rm_fpcr >> 22) & 3);                                                 \
})

#define raise_flags(e) do {                                                   \
  int rf_ex = (e);                                                            \
  if (rf_ex) {                                                                \
    uint64_t rf_fpsr;                                                         \
    __asm__ volatile("mrs %0, fpsr" : "=r"(rf_fpsr));                         \
    rf_fpsr |= rf_ex;                                                         \
    __asm__ volatile("msr fpsr, %0" : : "r"(rf_fpsr));                        \
  }                                                                           \
} while (0)

//
// 128 ビットの整数
//

// 小さな操作はマクロにする（slimcc はインライン展開をしないので、関数にすると呼び出しと構造体の
// 受け渡しが大半の時間を占め、GCC でコンパイルしたものの約 20 倍遅かった）。引数は 1 回だけ評価する
#define u128(h, l) ((U128){(h), (l)})

#define u_is_zero(a) ({ U128 uz_a = (a); !uz_a.hi && !uz_a.lo; })

#define u_cmp(a, b) ({                                                        \
  U128 uc_a = (a), uc_b = (b);                                                \
  uc_a.hi != uc_b.hi ? (uc_a.hi < uc_b.hi ? -1 : 1)                           \
  : uc_a.lo != uc_b.lo ? (uc_a.lo < uc_b.lo ? -1 : 1) : 0;                    \
})

#define u_add(a, b) ({                                                        \
  U128 ua_a = (a), ua_b = (b);                                                \
  U128 ua_r = {ua_a.hi + ua_b.hi, ua_a.lo + ua_b.lo};                         \
  ua_r.hi += ua_r.lo < ua_a.lo;                                               \
  ua_r;                                                                       \
})

#define u_sub(a, b) ({                                                        \
  U128 us_a = (a), us_b = (b);                                                \
  U128 us_r = {us_a.hi - us_b.hi, us_a.lo - us_b.lo};                         \
  us_r.hi -= us_a.lo < us_b.lo;                                               \
  us_r;                                                                       \
})

#define u_shl(a, n) ({                                                        \
  U128 ul_a = (a);                                                            \
  int ul_n = (n);                                                             \
  ul_n == 0 ? ul_a                                                            \
  : ul_n >= 128 ? u128(0, 0)                                                  \
  : ul_n >= 64 ? u128(ul_a.lo << (ul_n - 64), 0)                              \
  : u128((ul_a.hi << ul_n) | (ul_a.lo >> (64 - ul_n)), ul_a.lo << ul_n);      \
})

#define u_shr(a, n) ({                                                        \
  U128 ur_a = (a);                                                            \
  int ur_n = (n);                                                             \
  ur_n == 0 ? ur_a                                                            \
  : ur_n >= 128 ? u128(0, 0)                                                  \
  : ur_n >= 64 ? u128(0, ur_a.hi >> (ur_n - 64))                              \
  : u128(ur_a.hi >> ur_n, (ur_a.lo >> ur_n) | (ur_a.hi << (64 - ur_n)));      \
})

#define clz64(x) ({                                                           \
  uint64_t cz_n, cz_x = (x);                                                  \
  __asm__("clz %0, %1" : "=r"(cz_n) : "r"(cz_x)); /* x が 0 なら 64 */        \
  (int)cz_n;                                                                  \
})

#define u_clz(a) ({ U128 ucz_a = (a); ucz_a.hi ? clz64(ucz_a.hi) : 64 + clz64(ucz_a.lo); })

// 64 × 64 → 128
static U128 mul64(uint64_t a, uint64_t b) {
  uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  return u128(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (uint32_t)p00);
}

//
// 分解と組み立て
//

// 仮数部 sig（暗黙の 1 を含む。正規化数ならビット 112 が 1）と、はみ出し分 extra（上位ほど重い。
// 最上位ビットが 0.5 ulp）を、丸めて binary128 にする。exp は sig のビット 112 の位置に対する
// バイアスつきの指数（0 以下なら非正規化数か、アンダーフロー）
static F128 round_pack_s(int sign, int32_t exp, uint64_t sh, uint64_t sl, uint64_t extra, int *ex) {
  int rm = round_mode();
  bool tiny = false;

  if (exp <= 0) {
    // 非正規化数（アンダーフローは丸める前に判定する）
    int n = 1 - exp;
    tiny = true;
    if (n >= 128 + 64) {
      extra = (sh | sl) != 0 || extra;
      sh = sl = 0;
    } else {
      // sh:sl:extra を n だけ右へ（はみ出したビットは extra の最下位にまとめる）
      while (n > 0) {
        int k = n > 63 ? 63 : n;
        uint64_t out = extra & ((1ULL << k) - 1);
        extra = (extra >> k) | (sl << (64 - k)) | (out != 0);
        sl = (sl >> k) | (sh << (64 - k));
        sh >>= k;
        n -= k;
      }
    }
    exp = 0;
  }

  bool inc;
  switch (rm) {
  case RM_NEAREST:
    inc = extra > 0x8000000000000000ULL || (extra == 0x8000000000000000ULL && (sl & 1));
    break;
  case RM_UP:
    inc = !sign && extra;
    break;
  case RM_DOWN:
    inc = sign && extra;
    break;
  default:
    inc = false;
  }
  if (extra) {
    *ex |= EX_INEXACT;
    if (tiny)
      *ex |= EX_UNDERFLOW;
  }
  if (inc) {
    sl++;
    sh += sl == 0;
    if (sh >= (HIDDEN_HI << 1)) {
      // 繰り上がり（仮数部が 2 になった）
      sl = (sl >> 1) | (sh << 63);
      sh >>= 1;
      exp++;
    }
  }
  if (exp == 0 && (sh & HIDDEN_HI))
    exp = 1; // 非正規化数が丸めで最小の正規化数になった

  F128 r;
  if (exp >= EXP_MAX) {
    *ex |= EX_OVERFLOW | EX_INEXACT;
    bool to_inf = rm == RM_NEAREST || (rm == RM_UP && !sign) || (rm == RM_DOWN && sign);
    if (to_inf) {
      r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)EXP_MAX << 48);
      r.w.lo = 0;
    } else {
      r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)(EXP_MAX - 1) << 48) | FRAC_HI_MASK;
      r.w.lo = ~0ULL;
    }
    return r;
  }
  r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)exp << 48) | (sh & FRAC_HI_MASK);
  r.w.lo = sl;
  return r;
}

static F128 round_pack(int sign, int32_t exp, U128 sig, uint64_t extra, int *ex) {
  return round_pack_s(sign, exp, sig.hi, sig.lo, extra, ex);
}

typedef struct {
  int sign;
  int32_t exp; // バイアスつき。非正規化数は正規化したときの値（1 以下）
  U128 sig;    // 暗黙の 1 を含み、ビット 112 が先頭（0 のときは 0）
  enum { C_ZERO, C_NORMAL, C_INF, C_NAN } cls;
  bool snan;
} Unpacked;

static Unpacked unpack(F128 a) {
  Unpacked u;
  u.sign = a.w.hi >> 63;
  int32_t e = (a.w.hi >> 48) & EXP_MAX;
  U128 frac = u128(a.w.hi & FRAC_HI_MASK, a.w.lo);
  u.snan = false;
  if (e == EXP_MAX) {
    u.cls = u_is_zero(frac) ? C_INF : C_NAN;
    u.snan = u.cls == C_NAN && !(frac.hi & QUIET_HI);
    u.exp = e;
    u.sig = frac;
    return u;
  }
  if (e == 0) {
    if (u_is_zero(frac)) {
      u.cls = C_ZERO;
      u.exp = 0;
      u.sig = frac;
      return u;
    }
    // 非正規化数を正規化する
    int sh = u_clz(frac) - 15;
    u.sig = u_shl(frac, sh);
    u.exp = 1 - sh;
    u.cls = C_NORMAL;
    return u;
  }
  u.cls = C_NORMAL;
  u.exp = e;
  u.sig = u128(frac.hi | HIDDEN_HI, frac.lo);
  return u;
}

// 無効演算の結果の NaN（libgcc の aarch64 と同じく、正の符号で仮数部がすべて 1）
static F128 make_nan_default(void) {
  F128 r;
  r.w.hi = 0x7FFFFFFFFFFFFFFFULL;
  r.w.lo = ~0ULL;
  return r;
}

static F128 make_inf(int sign) {
  F128 r;
  r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)EXP_MAX << 48);
  r.w.lo = 0;
  return r;
}

static F128 make_zero(int sign) {
  F128 r;
  r.w.hi = (uint64_t)sign << 63;
  r.w.lo = 0;
  return r;
}

// NaN を含む二項演算の結果（libgcc の aarch64 と同じ選び方: a が quiet で b が signaling なら b、
// そうでなければ a の NaN を使い、quiet にする。signaling NaN があれば無効演算）
static F128 propagate_nan(F128 a, F128 b, int *ex) {
  Unpacked ua = unpack(a), ub = unpack(b);
  if (ua.snan || ub.snan)
    *ex |= EX_INVALID;
  F128 r;
  if (ua.cls == C_NAN && ub.cls == C_NAN)
    r = (!ua.snan && ub.snan) ? b : a;
  else
    r = ua.cls == C_NAN ? a : b;
  r.w.hi |= QUIET_HI;
  return r;
}

//
// 四則演算
//

//
// 速い道: 両方が正規化数のとき。128 ビットの値を 2 つの 64 ビットの局所変数（hi、lo）で持つ
// （slimcc は構造体をいつもメモリに置くが、アドレスを取らないスカラーの局所変数はレジスタに置ける）。
// 手順は add_sub・mul_slow と同じで、結果も同じ
//

#define IS_NORMAL(hi) ((((hi) >> 48) & EXP_MAX) - 1 < EXP_MAX - 1)

static F128 add_fast(uint64_t ahi, uint64_t alo, uint64_t bhi, uint64_t blo) {
  int as = ahi >> 63, bs = bhi >> 63;
  int32_t ae = (ahi >> 48) & EXP_MAX, be = (bhi >> 48) & EXP_MAX;
  uint64_t ah = (ahi & FRAC_HI_MASK) | HIDDEN_HI, al = alo;
  uint64_t bh = (bhi & FRAC_HI_MASK) | HIDDEN_HI, bl = blo;

  // |a| >= |b| にする
  if (ae < be || (ae == be && (ah < bh || (ah == bh && al < bl)))) {
    int ts = as;
    as = bs;
    bs = ts;
    int32_t te = ae;
    ae = be;
    be = te;
    uint64_t t = ah;
    ah = bh;
    bh = t;
    t = al;
    al = bl;
    bl = t;
  }

  // 仮数部を 3 ビット左にずらして余裕を取り、b を指数の差だけ右へ（はみ出しは sticky）
  int d = ae - be;
  ah = (ah << 3) | (al >> 61);
  al <<= 3;
  bh = (bh << 3) | (bl >> 61);
  bl <<= 3;
  bool sticky = false;
  if (d >= 120) {
    sticky = (bh | bl) != 0;
    bh = bl = 0;
  } else if (d >= 64) {
    sticky = bl != 0 || (bh & ((1ULL << (d - 64)) - 1)) != 0;
    bl = bh >> (d - 64);
    bh = 0;
  } else if (d > 0) {
    sticky = (bl & ((1ULL << d) - 1)) != 0;
    bl = (bl >> d) | (bh << (64 - d));
    bh >>= d;
  }
  if (sticky)
    bl |= 1;

  uint64_t sh, sl;
  int32_t exp = ae;
  if (as == bs) {
    sl = al + bl;
    sh = ah + bh + (sl < al);
  } else {
    sl = al - bl;
    sh = ah - bh - (al < bl);
    if (!(sh | sl))
      return make_zero(round_mode() == RM_DOWN);
  }

  // 先頭を（3 ビット左の）ビット 115 にそろえる
  int lead = 127 - (sh ? clz64(sh) : 64 + clz64(sl));
  if (lead > 115) {
    int k = lead - 115;
    bool lost = (sl & ((1ULL << k) - 1)) != 0;
    sl = (sl >> k) | (sh << (64 - k));
    sh >>= k;
    if (lost)
      sl |= 1;
    exp += k;
  } else if (lead < 115) {
    int k = 115 - lead;
    if (k >= 64) {
      sh = sl << (k - 64);
      sl = 0;
    } else {
      sh = (sh << k) | (sl >> (64 - k));
      sl <<= k;
    }
    exp -= k;
  }

  // 下位 3 ビットを extra（上位ほど重い）にする
  uint64_t extra = (sl & 7) << 61;
  sl = (sl >> 3) | (sh << 61);
  sh >>= 3;
  int ex = 0;
  F128 r = round_pack_s(as, exp, sh, sl, extra, &ex);
  raise_flags(ex);
  return r;
}

// 64 × 64 → 128（hi、lo に入れる）
#define MUL64(hi, lo, a, b) do {                                              \
  uint64_t m_a = (a), m_b = (b);                                              \
  uint64_t m_a0 = (uint32_t)m_a, m_a1 = m_a >> 32;                            \
  uint64_t m_b0 = (uint32_t)m_b, m_b1 = m_b >> 32;                            \
  uint64_t m_p00 = m_a0 * m_b0, m_p01 = m_a0 * m_b1;                          \
  uint64_t m_p10 = m_a1 * m_b0, m_p11 = m_a1 * m_b1;                          \
  uint64_t m_mid = (m_p00 >> 32) + (uint32_t)m_p01 + (uint32_t)m_p10;         \
  (hi) = m_p11 + (m_p01 >> 32) + (m_p10 >> 32) + (m_mid >> 32);               \
  (lo) = (m_mid << 32) | (uint32_t)m_p00;                                     \
} while (0)

static F128 mul_fast(uint64_t ahi, uint64_t alo, uint64_t bhi, uint64_t blo) {
  int sign = (ahi ^ bhi) >> 63;
  int32_t exp = (int32_t)((ahi >> 48) & EXP_MAX) + (int32_t)((bhi >> 48) & EXP_MAX) - BIAS;
  uint64_t ah = (ahi & FRAC_HI_MASK) | HIDDEN_HI, bh = (bhi & FRAC_HI_MASK) | HIDDEN_HI;

  // 113 ビット × 113 ビット → 226 ビット（w3:w2:w1:w0。先頭はビット 224 か 225）
  uint64_t llh, lll, lhh, lhl, hlh, hll, hhh, hhl;
  MUL64(llh, lll, alo, blo);
  MUL64(lhh, lhl, alo, bh);
  MUL64(hlh, hll, ah, blo);
  MUL64(hhh, hhl, ah, bh);
  uint64_t w0 = lll;
  uint64_t m = llh + lhl;
  uint64_t mc = m < llh;
  m += hll;
  mc += m < hll;
  uint64_t w1 = m;
  uint64_t t = hhl + lhh;
  uint64_t tc = t < hhl;
  t += hlh;
  tc += t < hlh;
  t += mc;
  tc += t < mc;
  uint64_t w2 = t, w3 = hhh + tc;

  // 右に 64 + b ビット（b は 48 か 49）ずらす。sig は下位 128 ビット、extra はその下の 64 ビット
  int b = 48;
  if (w3 >> (225 - 192) & 1) {
    b = 49;
    exp++;
  }
  uint64_t sl = (w1 >> b) | (w2 << (64 - b));
  uint64_t sh = (w2 >> b) | (w3 << (64 - b));
  uint64_t extra = (w0 >> b) | (w1 << (64 - b));
  if (w0 & ((1ULL << b) - 1))
    extra |= 1;
  int ex = 0;
  F128 r = round_pack_s(sign, exp, sh, sl, extra, &ex);
  raise_flags(ex);
  return r;
}

static F128 add_sub(F128 fa, F128 fb, bool negate_b) {
  int ex = 0;
  Unpacked a = unpack(fa), b = unpack(fb);
  if (negate_b)
    b.sign ^= 1;
  F128 r;

  if (a.cls == C_NAN || b.cls == C_NAN) {
    r = propagate_nan(fa, fb, &ex);
    raise_flags(ex);
    return r;
  }
  if (a.cls == C_INF || b.cls == C_INF) {
    if (a.cls == C_INF && b.cls == C_INF && a.sign != b.sign) {
      raise_flags(EX_INVALID);
      return make_nan_default();
    }
    return make_inf(a.cls == C_INF ? a.sign : b.sign);
  }
  if (a.cls == C_ZERO && b.cls == C_ZERO) {
    if (a.sign == b.sign)
      return make_zero(a.sign);
    return make_zero(round_mode() == RM_DOWN);
  }
  if (a.cls == C_ZERO) {
    r = fb;
    if (negate_b)
      r.w.hi ^= 1ULL << 63;
    return r;
  }
  if (b.cls == C_ZERO)
    return fa;

  // |a| >= |b| にする
  if (a.exp < b.exp || (a.exp == b.exp && u_cmp(a.sig, b.sig) < 0)) {
    Unpacked t = a;
    a = b;
    b = t;
  }

  // 仮数部を 3 ビット左にずらして余裕を取り、b を指数の差だけ右へ（はみ出しは sticky）
  int d = a.exp - b.exp;
  U128 sa = u_shl(a.sig, 3);
  U128 sb = u_shl(b.sig, 3);
  bool sticky = false;
  if (d >= 120) {
    sticky = !u_is_zero(sb);
    sb = u128(0, 0);
  } else if (d > 0) {
    U128 lost = u_sub(sb, u_shl(u_shr(sb, d), d));
    sticky = !u_is_zero(lost);
    sb = u_shr(sb, d);
  }
  if (sticky)
    sb.lo |= 1;

  U128 s;
  int32_t exp = a.exp;
  if (a.sign == b.sign) {
    s = u_add(sa, sb);
  } else {
    s = u_sub(sa, sb);
    if (u_is_zero(s))
      return make_zero(round_mode() == RM_DOWN);
  }

  // 先頭を（3 ビット左の）ビット 115 にそろえる。指数は 0 以下になってもよい（round_pack が
  // 非正規化数にする）
  int lead = 127 - u_clz(s);
  if (lead > 115) {
    int sh = lead - 115;
    bool lost = s.lo & ((1ULL << sh) - 1);
    s = u_shr(s, sh);
    if (lost)
      s.lo |= 1;
    exp += sh;
  } else if (lead < 115) {
    s = u_shl(s, 115 - lead);
    exp -= 115 - lead;
  }

  // 下位 3 ビットを extra（上位ほど重い）にする
  uint64_t extra = (s.lo & 7) << 61;
  r = round_pack(a.sign, exp, u_shr(s, 3), extra, &ex);
  raise_flags(ex);
  return r;
}

long double RT(__addtf3)(long double a, long double b) {
  F128 fa = {.f = a}, fb = {.f = b};
  if (IS_NORMAL(fa.w.hi) && IS_NORMAL(fb.w.hi))
    return add_fast(fa.w.hi, fa.w.lo, fb.w.hi, fb.w.lo).f;
  return add_sub(fa, fb, false).f;
}

long double RT(__subtf3)(long double a, long double b) {
  F128 fa = {.f = a}, fb = {.f = b};
  if (IS_NORMAL(fa.w.hi) && IS_NORMAL(fb.w.hi))
    return add_fast(fa.w.hi, fa.w.lo, fb.w.hi ^ (1ULL << 63), fb.w.lo).f;
  return add_sub(fa, fb, true).f;
}

// 256 ビットの値（w[3] が最上位）を右に sh（0〜127）ずらし、sig と extra（次の 64 ビット。
// それより下が 0 でなければ extra の最下位ビットを立てる）にする
static void shr256(const uint64_t w[4], int sh, U128 *sig, uint64_t *extra) {
  uint64_t t[5] = {0, w[0], w[1], w[2], w[3]}; // t[0] は余り（下にはみ出したもの）
  // 64 ビット単位でずらす
  int words = sh / 64, bits = sh % 64;
  uint64_t sticky = 0;
  for (int k = 0; k < words; k++) {
    sticky |= t[0];
    for (int j = 0; j < 4; j++)
      t[j] = t[j + 1];
    t[4] = 0;
  }
  if (bits) {
    sticky |= t[0] << (64 - bits);
    for (int j = 0; j < 4; j++)
      t[j] = (t[j] >> bits) | (t[j + 1] << (64 - bits));
    t[4] >>= bits;
  }
  // t[1..4] がずらした後の値（t[0] はその下の 64 ビット）。sig は下位 128 ビット、extra はその下
  *sig = u128(t[2], t[1]);
  *extra = t[0] | (sticky != 0);
}

static long double mul_slow(long double fa_, long double fb_) {
  F128 fa = {.f = fa_}, fb = {.f = fb_};
  int ex = 0;
  Unpacked a = unpack(fa), b = unpack(fb);
  int sign = a.sign ^ b.sign;
  F128 r;
  if (a.cls == C_NAN || b.cls == C_NAN) {
    r = propagate_nan(fa, fb, &ex);
    raise_flags(ex);
    return r.f;
  }
  if (a.cls == C_INF || b.cls == C_INF) {
    if (a.cls == C_ZERO || b.cls == C_ZERO) {
      raise_flags(EX_INVALID);
      return make_nan_default().f;
    }
    return make_inf(sign).f;
  }
  if (a.cls == C_ZERO || b.cls == C_ZERO)
    return make_zero(sign).f;

  // 113 ビット × 113 ビット → 226 ビット（先頭はビット 224 か 225）
  U128 ll = mul64(a.sig.lo, b.sig.lo), lh = mul64(a.sig.lo, b.sig.hi);
  U128 hl = mul64(a.sig.hi, b.sig.lo), hh = mul64(a.sig.hi, b.sig.hi);
  uint64_t w[4] = {ll.lo, 0, 0, 0};
  U128 mid = u_add(u128(0, ll.hi), u128(0, lh.lo));
  mid = u_add(mid, u128(0, hl.lo));
  w[1] = mid.lo;
  U128 top = u_add(u128(hh.hi, hh.lo), u128(0, lh.hi));
  top = u_add(top, u128(0, hl.hi));
  top = u_add(top, u128(0, mid.hi));
  w[2] = top.lo;
  w[3] = top.hi;

  int32_t exp = a.exp + b.exp - BIAS;
  int sh = 112;
  if (w[3] >> (225 - 192) & 1) {
    sh = 113;
    exp++;
  }
  U128 sig;
  uint64_t extra;
  shr256(w, sh, &sig, &extra);
  r = round_pack(sign, exp, sig, extra, &ex);
  raise_flags(ex);
  return r.f;
}

long double RT(__multf3)(long double a, long double b) {
  F128 fa = {.f = a}, fb = {.f = b};
  if (IS_NORMAL(fa.w.hi) && IS_NORMAL(fb.w.hi))
    return mul_fast(fa.w.hi, fa.w.lo, fb.w.hi, fb.w.lo).f;
  return mul_slow(a, b);
}

// 多倍長の割り算 u ÷ v（数字は 32 ビット、下位から。m 桁 ÷ n 桁、v の最上位の数字は 0 でないこと）。
// 商（m - n + 1 桁）を q に入れ、余りが 0 でなければ true。u は壊す。
// 出典: Knuth, TAOCP vol. 2, 4.3.1 Algorithm D、Hacker's Delight 9-2 節（divmnu）
static bool div_knuth(uint32_t *u, int m, const uint32_t *v0, int n, uint32_t *q) {
  // 正規化: v の最上位の数字の最上位ビットが 1 になるようにずらす
  int sh = clz64(v0[n - 1]) - 32;
  uint32_t v[4], un[11];
  for (int i = n - 1; i > 0; i--)
    v[i] = (v0[i] << sh) | (sh ? (uint32_t)((uint64_t)v0[i - 1] >> (32 - sh)) : 0);
  v[0] = v0[0] << sh;
  un[m] = sh ? (uint32_t)((uint64_t)u[m - 1] >> (32 - sh)) : 0;
  for (int i = m - 1; i > 0; i--)
    un[i] = (u[i] << sh) | (sh ? (uint32_t)((uint64_t)u[i - 1] >> (32 - sh)) : 0);
  un[0] = u[0] << sh;

  for (int j = m - n; j >= 0; j--) {
    // 商の数字の見積もり（上位 2 桁 ÷ v の最上位の数字）。大きすぎれば 1〜2 回減らす
    uint64_t num = (uint64_t)un[j + n] << 32 | un[j + n - 1];
    uint64_t qhat = num / v[n - 1];
    uint64_t rhat = num - qhat * v[n - 1];
    while (qhat >= (1ULL << 32) || qhat * v[n - 2] > (rhat << 32 | un[j + n - 2])) {
      qhat--;
      rhat += v[n - 1];
      if (rhat >= (1ULL << 32))
        break;
    }
    // un[j..j+n] -= qhat × v
    int64_t t;
    uint64_t k = 0;
    for (int i = 0; i < n; i++) {
      uint64_t p = qhat * v[i];
      t = (int64_t)un[i + j] - (int64_t)k - (int64_t)(p & 0xFFFFFFFF);
      un[i + j] = (uint32_t)t;
      k = (p >> 32) - (uint64_t)(t >> 32);
    }
    t = (int64_t)un[j + n] - (int64_t)k;
    un[j + n] = (uint32_t)t;
    q[j] = (uint32_t)qhat;
    if (t < 0) {
      // 引きすぎたので 1 回足し戻す
      q[j]--;
      uint64_t c = 0;
      for (int i = 0; i < n; i++) {
        c += (uint64_t)un[i + j] + v[i];
        un[i + j] = (uint32_t)c;
        c >>= 32;
      }
      un[j + n] += (uint32_t)c;
    }
  }
  for (int i = 0; i < n; i++)
    if (un[i])
      return true;
  return false;
}

long double RT(__divtf3)(long double fa_, long double fb_) {
  F128 fa = {.f = fa_}, fb = {.f = fb_};
  int ex = 0;
  Unpacked a = unpack(fa), b = unpack(fb);
  int sign = a.sign ^ b.sign;
  F128 r;
  if (a.cls == C_NAN || b.cls == C_NAN) {
    r = propagate_nan(fa, fb, &ex);
    raise_flags(ex);
    return r.f;
  }
  if (a.cls == C_INF) {
    if (b.cls == C_INF) {
      raise_flags(EX_INVALID);
      return make_nan_default().f;
    }
    return make_inf(sign).f;
  }
  if (b.cls == C_INF)
    return make_zero(sign).f;
  if (b.cls == C_ZERO) {
    if (a.cls == C_ZERO) {
      raise_flags(EX_INVALID);
      return make_nan_default().f;
    }
    raise_flags(EX_DIVZERO);
    return make_inf(sign).f;
  }
  if (a.cls == C_ZERO)
    return make_zero(sign).f;

  U128 q;
  // 商 Q = floor(rem0 × 2^176 / b.sig)（177 ビット）を求める。rem0 は [b.sig, 2 × b.sig) にそろえる。
  // Q の上位 113 ビットが仮数部、下位 64 ビットが extra、余りが 0 でなければ sticky
  int32_t exp = a.exp - b.exp + BIAS;
  U128 rem = a.sig;
  if (u_cmp(rem, b.sig) < 0) {
    rem = u_shl(rem, 1);
    exp--;
  }
  // 32 ビットずつの数字（下位から）にして、Knuth の Algorithm D で割る
  uint32_t u[10] = {0}, v[4], qd[7];
  u[5] = (uint32_t)rem.lo << 16;                            // rem0 << 176（176 = 5 × 32 + 16）
  u[6] = (uint32_t)(rem.lo >> 16);
  u[7] = (uint32_t)(rem.lo >> 48) | (uint32_t)(rem.hi << 16);
  u[8] = (uint32_t)(rem.hi >> 16);
  u[9] = (uint32_t)(rem.hi >> 48);
  v[0] = (uint32_t)b.sig.lo;
  v[1] = (uint32_t)(b.sig.lo >> 32);
  v[2] = (uint32_t)b.sig.hi;
  v[3] = (uint32_t)(b.sig.hi >> 32);
  bool nonzero_rem = div_knuth(u, 10, v, 4, qd);
  // Q は 177 ビットなので qd[6] は 0。qd[5]〜qd[2] が仮数部（Q >> 64）、qd[1]・qd[0] が extra
  q = u128((uint64_t)qd[5] << 32 | qd[4], (uint64_t)qd[3] << 32 | qd[2]);
  uint64_t extra = (uint64_t)qd[1] << 32 | qd[0];
  if (nonzero_rem)
    extra |= 1;
  r = round_pack(sign, exp, q, extra, &ex);
  raise_flags(ex);
  return r.f;
}

//
// 比較（libgcc と同じ戻り値: 等しい 0、小さい -1、大きい 1、順序なしは関数ごとの値）
//

static bool is_nan(F128 a) {
  return ((a.w.hi >> 48) & EXP_MAX) == EXP_MAX && ((a.w.hi & FRAC_HI_MASK) || a.w.lo);
}

static bool is_snan(F128 a) {
  return is_nan(a) && !(a.w.hi & QUIET_HI);
}

static int compare(F128 a, F128 b) {
  bool za = !(a.w.hi << 1) && !a.w.lo, zb = !(b.w.hi << 1) && !b.w.lo;
  if (za && zb)
    return 0;
  int sa = a.w.hi >> 63, sb = b.w.hi >> 63;
  if (sa != sb)
    return sa ? -1 : 1;
  // 符号が同じなら、符号を除いたビット列の大小（負なら逆）
  U128 ma = u128(a.w.hi & ~(1ULL << 63), a.w.lo), mb = u128(b.w.hi & ~(1ULL << 63), b.w.lo);
  int c = u_cmp(ma, mb);
  return sa ? -c : c;
}

// 等しいか（quiet な比較: signaling NaN だけ無効演算）
static int cmp_eq(long double x, long double y) {
  F128 a = {.f = x}, b = {.f = y};
  if (is_nan(a) || is_nan(b)) {
    if (is_snan(a) || is_snan(b))
      raise_flags(EX_INVALID);
    return 1;
  }
  return compare(a, b) != 0;
}

// 大小（signaling な比較: NaN があれば無効演算）
static int cmp_order(long double x, long double y, int unordered) {
  F128 a = {.f = x}, b = {.f = y};
  if (is_nan(a) || is_nan(b)) {
    raise_flags(EX_INVALID);
    return unordered;
  }
  return compare(a, b);
}

int RT(__eqtf2)(long double a, long double b) { return cmp_eq(a, b); }
int RT(__netf2)(long double a, long double b) { return cmp_eq(a, b); }
int RT(__lttf2)(long double a, long double b) { return cmp_order(a, b, 2); }
int RT(__letf2)(long double a, long double b) { return cmp_order(a, b, 2); }
int RT(__gttf2)(long double a, long double b) { return cmp_order(a, b, -2); }
int RT(__getf2)(long double a, long double b) { return cmp_order(a, b, -2); }

//
// 整数との変換
//

// 0 方向に切り捨てて整数にする。bits はビット数、is_signed は符号つきか
static uint64_t to_int(long double x, int bits, bool is_signed) {
  F128 f = {.f = x};
  Unpacked u = unpack(f);
  uint64_t max = is_signed ? (1ULL << (bits - 1)) - 1 : (bits == 64 ? ~0ULL : (1ULL << bits) - 1);
  uint64_t min_mag = is_signed ? 1ULL << (bits - 1) : 0; // 負の最小値の絶対値
  if (u.cls == C_ZERO)
    return 0;
  if (u.cls == C_NAN || u.cls == C_INF) {
    raise_flags(EX_INVALID);
    if (!is_signed)
      return u.sign ? 0 : max;
    return u.sign ? (uint64_t)-(int64_t)min_mag : max;
  }
  int32_t e = u.exp - BIAS; // 値 = 1.xxx × 2^e
  if (e < 0) {
    raise_flags(EX_INEXACT);
    return 0;
  }
  if (e >= bits) {
    raise_flags(EX_INVALID);
    if (!is_signed)
      return u.sign ? 0 : max;
    return u.sign ? (uint64_t)-(int64_t)min_mag : max;
  }
  // 整数部（e + 1 ビット）と、捨てる小数部
  U128 ip = u_shr(u.sig, 112 - e);
  U128 frac = u_sub(u.sig, u_shl(ip, 112 - e));
  uint64_t mag = ip.lo;
  if (u.sign) {
    if (!is_signed) {
      raise_flags(EX_INVALID);
      return 0;
    }
    if (mag > min_mag) {
      raise_flags(EX_INVALID);
      return (uint64_t)-(int64_t)min_mag;
    }
    if (!u_is_zero(frac))
      raise_flags(EX_INEXACT);
    return (uint64_t)-(int64_t)mag;
  }
  if (mag > max) {
    raise_flags(EX_INVALID);
    return max;
  }
  if (!u_is_zero(frac))
    raise_flags(EX_INEXACT);
  return mag;
}

int RT(__fixtfsi)(long double x) { return (int)to_int(x, 32, true); }
long RT(__fixtfdi)(long double x) { return (long)to_int(x, 64, true); }
unsigned RT(__fixunstfsi)(long double x) { return (unsigned)to_int(x, 32, false); }
unsigned long RT(__fixunstfdi)(long double x) { return to_int(x, 64, false); }

static long double from_int(uint64_t mag, int sign) {
  if (!mag)
    return make_zero(0).f;
  int lead = 63 - clz64(mag);
  U128 sig = u_shl(u128(0, mag), 112 - lead);
  F128 r;
  r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)(BIAS + lead) << 48) | (sig.hi & FRAC_HI_MASK);
  r.w.lo = sig.lo;
  return r.f;
}

long double RT(__floatsitf)(int i) {
  return from_int(i < 0 ? -(uint64_t)(int64_t)i : (uint64_t)i, i < 0);
}
long double RT(__floatditf)(long i) {
  return from_int(i < 0 ? -(uint64_t)i : (uint64_t)i, i < 0);
}
long double RT(__floatunsitf)(unsigned i) { return from_int(i, 0); }
long double RT(__floatunditf)(unsigned long i) { return from_int(i, 0); }

//
// float・double との変換
//

// 小さい形式（仮数部 mant ビット、指数の最大 emax、バイアス bias）から binary128 へ（正確）
static long double extend(uint64_t bits, int mant, int ebits) {
  int sign = bits >> (mant + ebits);
  int32_t emax = (1 << ebits) - 1, bias = emax >> 1;
  int32_t e = (bits >> mant) & emax;
  uint64_t frac = bits & ((1ULL << mant) - 1);
  F128 r;
  if (e == emax) {
    if (!frac)
      return make_inf(sign).f;
    // NaN: 仮数部を上に詰め、quiet にする（signaling なら無効演算）
    if (!(frac >> (mant - 1)))
      raise_flags(EX_INVALID);
    U128 f = u_shl(u128(0, frac), 112 - mant);
    r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)EXP_MAX << 48) | (f.hi & FRAC_HI_MASK) | QUIET_HI;
    r.w.lo = f.lo;
    return r.f;
  }
  if (e == 0) {
    if (!frac)
      return make_zero(sign).f;
    // 非正規化数を正規化する
    int lead = 63 - clz64(frac);
    e = 1 - (mant - lead);
    frac = (frac << (mant - lead)) & ((1ULL << mant) - 1);
  }
  U128 f = u_shl(u128(0, frac), 112 - mant);
  r.w.hi = ((uint64_t)sign << 63) | ((uint64_t)(e - bias + BIAS) << 48) | (f.hi & FRAC_HI_MASK);
  r.w.lo = f.lo;
  return r.f;
}

long double RT(__extendsftf2)(float x) {
  union { float f; uint32_t u; } v = {.f = x};
  return extend(v.u, 23, 8);
}

long double RT(__extenddftf2)(double x) {
  union { double d; uint64_t u; } v = {.d = x};
  return extend(v.u, 52, 11);
}

// binary128 から小さい形式へ（丸める）
static uint64_t truncate(long double x, int mant, int ebits) {
  F128 f = {.f = x};
  Unpacked u = unpack(f);
  int32_t emax = (1 << ebits) - 1, bias = emax >> 1;
  uint64_t sign = (uint64_t)u.sign << (mant + ebits);
  int ex = 0;
  if (u.cls == C_NAN) {
    if (u.snan)
      raise_flags(EX_INVALID);
    uint64_t frac = u_shr(u.sig, 112 - mant).lo;
    return sign | ((uint64_t)emax << mant) | frac | (1ULL << (mant - 1));
  }
  if (u.cls == C_INF)
    return sign | ((uint64_t)emax << mant);
  if (u.cls == C_ZERO)
    return sign;

  int32_t e = u.exp - BIAS + bias; // 小さい形式のバイアスつき指数
  // 仮数部（mant + 1 ビット）と、はみ出し分
  int sh = 112 - mant;
  uint64_t sig = u_shr(u.sig, sh).lo;
  U128 rest = u_sub(u.sig, u_shl(u_shr(u.sig, sh), sh));
  uint64_t extra = u_shl(rest, 128 - sh).hi; // はみ出し分の上位 64 ビット
  if (!u_is_zero(u_shl(rest, 128 - sh + 64)))
    extra |= 1;
  if (sh > 64 && u_shl(rest, 128 - sh).lo)
    extra |= 1;

  bool tiny = false;
  if (e <= 0) {
    tiny = true;
    int d = 1 - e;
    if (d > 63 + mant) {
      extra = (sig || extra) ? 1 : 0;
      sig = 0;
    } else {
      while (d > 0) {
        int n = d > 63 ? 63 : d;
        uint64_t out = extra & ((1ULL << n) - 1);
        extra = (extra >> n) | (sig << (64 - n)) | (out != 0);
        sig >>= n;
        d -= n;
      }
    }
    e = 0;
  }

  int rm = round_mode();
  bool inc;
  switch (rm) {
  case RM_NEAREST:
    inc = extra > 0x8000000000000000ULL || (extra == 0x8000000000000000ULL && (sig & 1));
    break;
  case RM_UP: inc = !u.sign && extra; break;
  case RM_DOWN: inc = u.sign && extra; break;
  default: inc = false;
  }
  if (extra) {
    ex |= EX_INEXACT;
    if (tiny)
      ex |= EX_UNDERFLOW;
  }
  if (inc) {
    sig++;
    if (sig >> (mant + 1)) {
      sig >>= 1;
      e++;
    }
  }
  if (e == 0 && (sig >> mant))
    e = 1;
  if (e >= emax) {
    ex |= EX_OVERFLOW | EX_INEXACT;
    raise_flags(ex);
    bool to_inf = rm == RM_NEAREST || (rm == RM_UP && !u.sign) || (rm == RM_DOWN && u.sign);
    if (to_inf)
      return sign | ((uint64_t)emax << mant);
    return sign | ((uint64_t)(emax - 1) << mant) | ((1ULL << mant) - 1);
  }
  raise_flags(ex);
  return sign | ((uint64_t)e << mant) | (sig & ((1ULL << mant) - 1));
}

float RT(__trunctfsf2)(long double x) {
  union { uint32_t u; float f; } v = {.u = (uint32_t)truncate(x, 23, 8)};
  return v.f;
}

double RT(__trunctfdf2)(long double x) {
  union { uint64_t u; double d; } v = {.u = truncate(x, 52, 11)};
  return v.d;
}
