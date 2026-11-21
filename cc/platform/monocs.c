// Monocs（Raspberry Pi Zero 2 W のベアメタル OS）。libslimcc として OS の中で動く。
//
// ヘッダーは SD カードの /usr/lib/slimcc/include（slimcc 付属。GCC の /usr/lib/gcc/…/include にあたる）と /usr/include（picolibc と monocs.h）から読む。
// picolibc のヘッダーは、GCC が定義する整数型のマクロ（__INTPTR_TYPE__ など）を使うので、
// Pi 5 の GCC 14（aarch64-linux-gnu）と同じ値を定義する（gcc -dM -E から取り出した。浮動小数点の
// __FLT_*、__DBL_*、__LDBL_* も含む。long double は binary128）。
//
// また、__GNUC__ があると picolibc の math.h が使う GCC の組み込み関数（__builtin_isnan など）を、
// 関数形式のマクロとして定義する（gcc_builtin_macros）。引数は 1 回だけ評価する。

#include "../slimcc.h"

static const char *const gcc_type_macros[] = {
  "__CHAR16_TYPE__=short unsigned int",
  "__CHAR32_TYPE__=unsigned int",
  "__INT16_MAX__=0x7fff",
  "__INT16_TYPE__=short int",
  "__INT32_MAX__=0x7fffffff",
  "__INT32_TYPE__=int",
  "__INT64_MAX__=0x7fffffffffffffffL",
  "__INT64_TYPE__=long int",
  "__INT8_MAX__=0x7f",
  "__INT8_TYPE__=signed char",
  "__INTMAX_MAX__=0x7fffffffffffffffL",
  "__INTMAX_TYPE__=long int",
  "__INTMAX_WIDTH__=64",
  "__INTPTR_MAX__=0x7fffffffffffffffL",
  "__INTPTR_TYPE__=long int",
  "__INTPTR_WIDTH__=64",
  "__INT_FAST16_MAX__=0x7fffffffffffffffL",
  "__INT_FAST16_TYPE__=long int",
  "__INT_FAST16_WIDTH__=64",
  "__INT_FAST32_MAX__=0x7fffffffffffffffL",
  "__INT_FAST32_TYPE__=long int",
  "__INT_FAST32_WIDTH__=64",
  "__INT_FAST64_MAX__=0x7fffffffffffffffL",
  "__INT_FAST64_TYPE__=long int",
  "__INT_FAST64_WIDTH__=64",
  "__INT_FAST8_MAX__=0x7f",
  "__INT_FAST8_TYPE__=signed char",
  "__INT_FAST8_WIDTH__=8",
  "__INT_LEAST16_MAX__=0x7fff",
  "__INT_LEAST16_TYPE__=short int",
  "__INT_LEAST16_WIDTH__=16",
  "__INT_LEAST32_MAX__=0x7fffffff",
  "__INT_LEAST32_TYPE__=int",
  "__INT_LEAST32_WIDTH__=32",
  "__INT_LEAST64_MAX__=0x7fffffffffffffffL",
  "__INT_LEAST64_TYPE__=long int",
  "__INT_LEAST64_WIDTH__=64",
  "__INT_LEAST8_MAX__=0x7f",
  "__INT_LEAST8_TYPE__=signed char",
  "__INT_LEAST8_WIDTH__=8",
  "__INT_MAX__=0x7fffffff",
  "__INT_WIDTH__=32",
  "__LONG_LONG_MAX__=0x7fffffffffffffffLL",
  "__LONG_LONG_WIDTH__=64",
  "__LONG_MAX__=0x7fffffffffffffffL",
  "__LONG_WIDTH__=64",
  "__PTRDIFF_MAX__=0x7fffffffffffffffL",
  "__PTRDIFF_TYPE__=long int",
  "__PTRDIFF_WIDTH__=64",
  "__SCHAR_MAX__=0x7f",
  "__SCHAR_WIDTH__=8",
  "__SHRT_MAX__=0x7fff",
  "__SHRT_WIDTH__=16",
  "__SIG_ATOMIC_MAX__=0x7fffffff",
  "__SIG_ATOMIC_MIN__=(-__SIG_ATOMIC_MAX__ - 1)",
  "__SIG_ATOMIC_TYPE__=int",
  "__SIG_ATOMIC_WIDTH__=32",
  "__SIZE_MAX__=0xffffffffffffffffUL",
  "__SIZE_TYPE__=long unsigned int",
  "__SIZE_WIDTH__=64",
  "__UINT16_MAX__=0xffff",
  "__UINT16_TYPE__=short unsigned int",
  "__UINT32_MAX__=0xffffffffU",
  "__UINT32_TYPE__=unsigned int",
  "__UINT64_MAX__=0xffffffffffffffffUL",
  "__UINT64_TYPE__=long unsigned int",
  "__UINT8_MAX__=0xff",
  "__UINT8_TYPE__=unsigned char",
  "__UINTMAX_MAX__=0xffffffffffffffffUL",
  "__UINTMAX_TYPE__=long unsigned int",
  "__UINTPTR_MAX__=0xffffffffffffffffUL",
  "__UINTPTR_TYPE__=long unsigned int",
  "__UINT_FAST16_MAX__=0xffffffffffffffffUL",
  "__UINT_FAST16_TYPE__=long unsigned int",
  "__UINT_FAST32_MAX__=0xffffffffffffffffUL",
  "__UINT_FAST32_TYPE__=long unsigned int",
  "__UINT_FAST64_MAX__=0xffffffffffffffffUL",
  "__UINT_FAST64_TYPE__=long unsigned int",
  "__UINT_FAST8_MAX__=0xff",
  "__UINT_FAST8_TYPE__=unsigned char",
  "__UINT_LEAST16_MAX__=0xffff",
  "__UINT_LEAST16_TYPE__=short unsigned int",
  "__UINT_LEAST32_MAX__=0xffffffffU",
  "__UINT_LEAST32_TYPE__=unsigned int",
  "__UINT_LEAST64_MAX__=0xffffffffffffffffUL",
  "__UINT_LEAST64_TYPE__=long unsigned int",
  "__UINT_LEAST8_MAX__=0xff",
  "__UINT_LEAST8_TYPE__=unsigned char",
  "__WCHAR_MAX__=0xffffffffU",
  "__WCHAR_MIN__=0U",
  "__WCHAR_TYPE__=unsigned int",
  "__WCHAR_WIDTH__=32",
  "__WINT_MAX__=0xffffffffU",
  "__WINT_MIN__=0U",
  "__WINT_TYPE__=unsigned int",
  "__WINT_WIDTH__=32",
  "__INT16_C(c)=c",
  "__INT32_C(c)=c",
  "__INT64_C(c)=c ## L",
  "__INT8_C(c)=c",
  "__INTMAX_C(c)=c ## L",
  "__UINT16_C(c)=c",
  "__UINT32_C(c)=c ## U",
  "__UINT64_C(c)=c ## UL",
  "__UINT8_C(c)=c",
  "__UINTMAX_C(c)=c ## UL",
  "__DBL_DECIMAL_DIG__=17",
  "__DBL_DENORM_MIN__=((double)4.94065645841246544176568792868221372e-324L)",
  "__DBL_DIG__=15",
  "__DBL_EPSILON__=((double)2.22044604925031308084726333618164062e-16L)",
  "__DBL_HAS_DENORM__=1",
  "__DBL_HAS_INFINITY__=1",
  "__DBL_HAS_QUIET_NAN__=1",
  "__DBL_IS_IEC_60559__=1",
  "__DBL_MANT_DIG__=53",
  "__DBL_MAX_10_EXP__=308",
  "__DBL_MAX_EXP__=1024",
  "__DBL_MAX__=((double)1.79769313486231570814527423731704357e+308L)",
  "__DBL_MIN_10_EXP__=(-307)",
  "__DBL_MIN_EXP__=(-1021)",
  "__DBL_MIN__=((double)2.22507385850720138309023271733240406e-308L)",
  "__DBL_NORM_MAX__=((double)1.79769313486231570814527423731704357e+308L)",
  "__FLT_DECIMAL_DIG__=9",
  "__FLT_DENORM_MIN__=1.40129846432481707092372958328991613e-45F",
  "__FLT_DIG__=6",
  "__FLT_EPSILON__=1.19209289550781250000000000000000000e-7F",
  "__FLT_EVAL_METHOD_C99__=0",
  "__FLT_EVAL_METHOD_TS_18661_3__=0",
  "__FLT_EVAL_METHOD__=0",
  "__FLT_HAS_DENORM__=1",
  "__FLT_HAS_INFINITY__=1",
  "__FLT_HAS_QUIET_NAN__=1",
  "__FLT_IS_IEC_60559__=1",
  "__FLT_MANT_DIG__=24",
  "__FLT_MAX_10_EXP__=38",
  "__FLT_MAX_EXP__=128",
  "__FLT_MAX__=3.40282346638528859811704183484516925e+38F",
  "__FLT_MIN_10_EXP__=(-37)",
  "__FLT_MIN_EXP__=(-125)",
  "__FLT_MIN__=1.17549435082228750796873653722224568e-38F",
  "__FLT_NORM_MAX__=3.40282346638528859811704183484516925e+38F",
  "__FLT_RADIX__=2",
  "__LDBL_DECIMAL_DIG__=36",
  "__LDBL_DENORM_MIN__=6.47517511943802511092443895822764655e-4966L",
  "__LDBL_DIG__=33",
  "__LDBL_EPSILON__=1.92592994438723585305597794258492732e-34L",
  "__LDBL_HAS_DENORM__=1",
  "__LDBL_HAS_INFINITY__=1",
  "__LDBL_HAS_QUIET_NAN__=1",
  "__LDBL_IS_IEC_60559__=1",
  "__LDBL_MANT_DIG__=113",
  "__LDBL_MAX_10_EXP__=4932",
  "__LDBL_MAX_EXP__=16384",
  "__LDBL_MAX__=1.18973149535723176508575932662800702e+4932L",
  "__LDBL_MIN_10_EXP__=(-4931)",
  "__LDBL_MIN_EXP__=(-16381)",
  "__LDBL_MIN__=3.36210314311209350626267781732175260e-4932L",
  "__LDBL_NORM_MAX__=1.18973149535723176508575932662800702e+4932L",
};

// ヘッダーを探すパスの前に付ける接頭辞。Monocs では ""。
// Linux 上で確かめるとき（slimcc-jit-monocs）は、SD カードと同じ形のディレクトリを指す
TLS const char *slimcc_sysroot = "";

// GCC の組み込み関数のうち、picolibc（math.h など）が使うもの。
// 浮動小数点の例外フラグ（無効演算など）の扱いは GCC と同じとは限らない
static const char *const gcc_builtin_macros[] = {
  // 定数（NaN のペイロードは扱わない。引数の文字列は "" だけを想定する）
  "__builtin_huge_val()=((double)__builtin_math_constant_inff())",
  "__builtin_huge_valf()=(__builtin_math_constant_inff())",
  "__builtin_huge_vall()=((long double)__builtin_math_constant_inff())",
  "__builtin_inf()=((double)__builtin_math_constant_inff())",
  "__builtin_inff()=(__builtin_math_constant_inff())",
  "__builtin_infl()=((long double)__builtin_math_constant_inff())",
  "__builtin_nan(s)=((double)__builtin_math_constant_nanf())",
  "__builtin_nanf(s)=(__builtin_math_constant_nanf())",
  "__builtin_nanl(s)=((long double)__builtin_math_constant_nanf())",
  "__builtin_nans(s)=(__builtin_math_constant_nans())",
  "__builtin_nansf(s)=(__builtin_math_constant_nansf())",
  "__builtin_nansl(s)=(__builtin_math_constant_nansl())",
  // 分類（型によらない）
  "__builtin_isnan(x)=({ __typeof__(x) __v = (x); __v != __v; })",
  "__builtin_isinf_sign(x)=({ __typeof__(x) __v = (x); "
      "__v == (__typeof__(__v))__builtin_math_constant_inff() ? 1 : "
      "__v == -(__typeof__(__v))__builtin_math_constant_inff() ? -1 : 0; })",
  "__builtin_isinf(x)=(__builtin_isinf_sign(x) != 0)",
  "__builtin_isfinite(x)=({ __typeof__(x) __v = (x); "
      "__v == __v && __v != (__typeof__(__v))__builtin_math_constant_inff() && "
      "__v != -(__typeof__(__v))__builtin_math_constant_inff(); })",
  "__builtin_iszero(x)=({ __typeof__(x) __v = (x); __v == 0; })",
  "__builtin_isnormal(x)=({ __typeof__(x) __v = (x); __typeof__(x) __a = __v < 0 ? -__v : __v; "
      "__builtin_isfinite(__v) && __a >= (sizeof(__v) == sizeof(float) ? __FLT_MIN__ : "
      "sizeof(__v) == sizeof(double) ? __DBL_MIN__ : __LDBL_MIN__); })",
  "__builtin_issubnormal(x)=({ __typeof__(x) __v = (x); __typeof__(x) __a = __v < 0 ? -__v : __v; "
      "__a != 0 && __a < (sizeof(__v) == sizeof(float) ? __FLT_MIN__ : "
      "sizeof(__v) == sizeof(double) ? __DBL_MIN__ : __LDBL_MIN__); })",
  "__builtin_fpclassify(nan, inf, norm, sub, zero, x)=({ __typeof__(x) __w = (x); "
      "__builtin_isnan(__w) ? (nan) : __builtin_isinf_sign(__w) ? (inf) : "
      "__builtin_iszero(__w) ? (zero) : __builtin_issubnormal(__w) ? (sub) : (norm); })",
  // 符号ビット（-0.0 と NaN の符号も見る）
  "__builtin_signbitf(x)=({ union { float __f; unsigned int __u; } __t = { (x) }; (int)(__t.__u >> 31); })",
  "__builtin_signbit(x)=({ union { double __d; unsigned long __u; } __t = { (x) }; (int)(__t.__u >> 63); })",
  "__builtin_signbitl(x)=({ union { long double __d; unsigned long __u[2]; } __t = { (x) }; "
      "(int)(__t.__u[1] >> 63); })",
  // ビット数え（clz・ctz は 0 のとき 32 か 64。GCC では未定義）
  "__builtin_clz(x)=({ unsigned int __r; __asm__(\"clz %w0, %w1\" : \"=r\"(__r) : \"r\"((unsigned int)(x))); (int)__r; })",
  "__builtin_clzl(x)=({ unsigned long __r; __asm__(\"clz %0, %1\" : \"=r\"(__r) : \"r\"((unsigned long)(x))); (int)__r; })",
  "__builtin_clzll(x)=__builtin_clzl(x)",
  "__builtin_ctz(x)=({ unsigned int __r; __asm__(\"rbit %w0, %w1\\n\\tclz %w0, %w0\" : \"=r\"(__r) : \"r\"((unsigned int)(x))); (int)__r; })",
  "__builtin_ctzl(x)=({ unsigned long __r; __asm__(\"rbit %0, %1\\n\\tclz %0, %0\" : \"=r\"(__r) : \"r\"((unsigned long)(x))); (int)__r; })",
  "__builtin_ctzll(x)=__builtin_ctzl(x)",
  "__builtin_popcountl(x)=({ unsigned long __v = (x); __v = __v - ((__v >> 1) & 0x5555555555555555UL); "
      "__v = (__v & 0x3333333333333333UL) + ((__v >> 2) & 0x3333333333333333UL); "
      "__v = (__v + (__v >> 4)) & 0x0f0f0f0f0f0f0f0fUL; (int)((__v * 0x0101010101010101UL) >> 56); })",
  "__builtin_popcount(x)=__builtin_popcountl((unsigned int)(x))",
  "__builtin_popcountll(x)=__builtin_popcountl(x)",
  // 比較（順序のない組では偽）
  "__builtin_isunordered(x, y)=({ __typeof__(x) __a = (x); __typeof__(y) __b = (y); "
      "__a != __a || __b != __b; })",
  "__builtin_isgreater(x, y)=({ __typeof__(x) __a = (x); __typeof__(y) __b = (y); "
      "__a == __a && __b == __b && __a > __b; })",
  "__builtin_isgreaterequal(x, y)=({ __typeof__(x) __a = (x); __typeof__(y) __b = (y); "
      "__a == __a && __b == __b && __a >= __b; })",
  "__builtin_isless(x, y)=({ __typeof__(x) __a = (x); __typeof__(y) __b = (y); "
      "__a == __a && __b == __b && __a < __b; })",
  "__builtin_islessequal(x, y)=({ __typeof__(x) __a = (x); __typeof__(y) __b = (y); "
      "__a == __a && __b == __b && __a <= __b; })",
  "__builtin_islessgreater(x, y)=({ __typeof__(x) __a = (x); __typeof__(y) __b = (y); "
      "__a == __a && __b == __b && __a != __b; })",
  // 数学関数の組み込み（GCC は同じ名前の関数を呼ぶ。math.h を読んでいなくても使えるよう、その場で宣言する。
  // SQLite の shell.c。2026-11-20）
  "__builtin_ceil(x)=({ extern double ceil(double); ceil(x); })",
  "__builtin_floor(x)=({ extern double floor(double); floor(x); })",
  "__builtin_trunc(x)=({ extern double trunc(double); trunc(x); })",
  "__builtin_round(x)=({ extern double round(double); round(x); })",
  "__builtin_fabs(x)=({ extern double fabs(double); fabs(x); })",
  "__builtin_sqrt(x)=({ extern double sqrt(double); sqrt(x); })",
  "__builtin_ceilf(x)=({ extern float ceilf(float); ceilf(x); })",
  "__builtin_floorf(x)=({ extern float floorf(float); floorf(x); })",
  "__builtin_truncf(x)=({ extern float truncf(float); truncf(x); })",
  "__builtin_roundf(x)=({ extern float roundf(float); roundf(x); })",
  "__builtin_fabsf(x)=({ extern float fabsf(float); fabsf(x); })",
  "__builtin_sqrtf(x)=({ extern float sqrtf(float); sqrtf(x); })",
  // GCC の __atomic_*・__sync_*（2026-11-20。SQLite は __has_extension(c_atomic) を見て __atomic_load_n などを使う）。
  // slimcc の C11 のアトミック（stdatomic.h と同じ組み込み）で作る。メモリの順序の引数は見ず、いつも seq_cst
  // （どの順序を頼まれても、より強い順序で正しい）。nand はない。__GCC_HAVE_SYNC_COMPARE_AND_SWAP_* は
  // 定義しない（picolibc の stdio がそれを見て作り方を変えるため）
  "__ATOMIC_RELAXED=0", "__ATOMIC_CONSUME=1", "__ATOMIC_ACQUIRE=2", "__ATOMIC_RELEASE=3",
  "__ATOMIC_ACQ_REL=4", "__ATOMIC_SEQ_CST=5",
  "__atomic_load_n(p, m)=(*__builtin_atomic_chk(p))",
  "__atomic_store_n(p, v, m)=((void)(*__builtin_atomic_chk(p) = (v)))",
  "__atomic_exchange_n(p, v, m)=__builtin_atomic_exchange((p), (v))",
  "__atomic_compare_exchange_n(p, e, d, w, s, f)=__builtin_compare_and_swap((p), (e), (d))",
  "__atomic_load(p, r, m)=((void)(*(r) = *__builtin_atomic_chk(p)))",
  "__atomic_store(p, v, m)=((void)(*__builtin_atomic_chk(p) = *(v)))",
  "__atomic_exchange(p, v, r, m)=((void)(*(r) = __builtin_atomic_exchange((p), *(v))))",
  "__atomic_compare_exchange(p, e, d, w, s, f)=__builtin_compare_and_swap((p), (e), *(d))",
  "__atomic_fetch_add(p, v, m)=__builtin_atomic_fetch_add((p), (v))",
  "__atomic_fetch_sub(p, v, m)=__builtin_atomic_fetch_sub((p), (v))",
  "__atomic_fetch_and(p, v, m)=__builtin_atomic_fetch_and((p), (v))",
  "__atomic_fetch_or(p, v, m)=__builtin_atomic_fetch_or((p), (v))",
  "__atomic_fetch_xor(p, v, m)=__builtin_atomic_fetch_xor((p), (v))",
  "__atomic_add_fetch(p, v, m)=__builtin_atomic_arith_add((p), (v))",
  "__atomic_sub_fetch(p, v, m)=__builtin_atomic_arith_sub((p), (v))",
  "__atomic_and_fetch(p, v, m)=__builtin_atomic_arith_and((p), (v))",
  "__atomic_or_fetch(p, v, m)=__builtin_atomic_arith_or((p), (v))",
  "__atomic_xor_fetch(p, v, m)=__builtin_atomic_arith_xor((p), (v))",
  "__atomic_test_and_set(p, m)=((_Bool)__builtin_atomic_exchange((unsigned char *)(p), 1))",
  "__atomic_clear(p, m)=((void)(*__builtin_atomic_chk((unsigned char *)(p)) = 0))",
  "__atomic_thread_fence(m)=__builtin_atomic_thread_fence()",
  "__atomic_signal_fence(m)=__asm__ volatile(\"\" ::: \"memory\")",
  "__atomic_always_lock_free(n, p)=((n) <= 8)",
  "__atomic_is_lock_free(n, p)=((n) <= 8)",
  "__sync_synchronize()=__builtin_atomic_thread_fence()",
  "__sync_fetch_and_add(p, v, ...)=__builtin_atomic_fetch_add((p), (v))",
  "__sync_fetch_and_sub(p, v, ...)=__builtin_atomic_fetch_sub((p), (v))",
  "__sync_fetch_and_and(p, v, ...)=__builtin_atomic_fetch_and((p), (v))",
  "__sync_fetch_and_or(p, v, ...)=__builtin_atomic_fetch_or((p), (v))",
  "__sync_fetch_and_xor(p, v, ...)=__builtin_atomic_fetch_xor((p), (v))",
  "__sync_add_and_fetch(p, v, ...)=__builtin_atomic_arith_add((p), (v))",
  "__sync_sub_and_fetch(p, v, ...)=__builtin_atomic_arith_sub((p), (v))",
  "__sync_and_and_fetch(p, v, ...)=__builtin_atomic_arith_and((p), (v))",
  "__sync_or_and_fetch(p, v, ...)=__builtin_atomic_arith_or((p), (v))",
  "__sync_xor_and_fetch(p, v, ...)=__builtin_atomic_arith_xor((p), (v))",
  "__sync_bool_compare_and_swap(p, o, n, ...)=({ __typeof__(*(p)) __o = (o); "
      "__builtin_compare_and_swap((p), &__o, (n)); })",
  "__sync_val_compare_and_swap(p, o, n, ...)=({ __typeof__(*(p)) __o = (o); "
      "__builtin_compare_and_swap((p), &__o, (n)); __o; })",
  "__sync_lock_test_and_set(p, v, ...)=__builtin_atomic_exchange((p), (v))",
  "__sync_lock_release(p, ...)=((void)(*__builtin_atomic_chk(p) = 0))",
};

void platform_init_driver(void) {
  dumpmachine_str = "aarch64-none-elf";
}

void platform_init_cc1(void) {
  define_macro("__ELF__", "1");
  define_macro("__MONOCS__", "1");
  // picolibc の sys/cdefs.h は __GNUC__ がないと __ASMNAME などを定義せず、__func__ を NULL にする。
  // 版は clang と同じ 4.2.1 を名乗る（新しい GCC だけの機能を picolibc に使わせないため。ユーザーと合意、2026-11-04）
  define_macro("__GNUC__", "4");
  define_macro("__GNUC_MINOR__", "2");
  define_macro("__GNUC_PATCHLEVEL__", "1");
  // __GNUC__ があると使われる __extension__ は、意味を持たないので消す（platform/freebsd.c と同じ）
  define_macro("__extension__", "");
  init_ty_lp64();
  for (size_t i = 0; i < sizeof(gcc_type_macros) / sizeof(*gcc_type_macros); i++)
    define_macro_cli(gcc_type_macros[i]);
  for (size_t i = 0; i < sizeof(gcc_builtin_macros) / sizeof(*gcc_builtin_macros); i++)
    define_macro_cli(gcc_builtin_macros[i]);
}

void platform_stdinc_paths(StringArray *paths) {
  add_include_path(paths, format("%s/usr/lib/slimcc/include", slimcc_sysroot));
  add_include_path(paths, format("%s/usr/include", slimcc_sysroot));
}

void platform_search_dirs(StringArray *paths) {
  (void)paths;
}

// 内蔵アセンブラで .o を書く（main.c の run_assembler_gnustyle。外部の as は呼ばない）
void run_assembler(StringArray *as_args, const char *input, const char *output) {
  run_assembler_gnustyle(as_args, input, output);
}

void run_linker(StringArray *paths, StringArray *args, const char *output) {
  (void)paths;
  (void)args;
  (void)output;
  error("Monocs: an external linker is not available");
}
