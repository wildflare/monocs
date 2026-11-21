// AArch64 の型のレイアウトを確かめるテスト（layout.c）を生成する。
// Pi 5 の GCC でビルド・実行し、GCC が計算した値を _Static_assert として書き出す。
// 生成した layout.c は、GCC と slimcc-a64 の両方で -fsyntax-only が通ること。
//
//   gcc -std=gnu23 -o gen_layout gen_layout.c && ./gen_layout > layout.c
//   （layout.c の検査も -std=gnu23 で行う。_BitInt を使うため）

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include "layout_types.h"

#define SIZE(t)         printf("_Static_assert(sizeof(%s) == %zu, \"sizeof(%s)\");\n", #t, sizeof(t), #t)
#define ALIGN(t)        printf("_Static_assert(_Alignof(%s) == %zu, \"_Alignof(%s)\");\n", #t, _Alignof(t), #t)
#define LAYOUT(t)       SIZE(t); ALIGN(t)
#define OFFSET(t, m)    printf("_Static_assert(offsetof(%s, %s) == %zu, \"offsetof(%s, %s)\");\n", #t, #m, offsetof(t, m), #t, #m)
#define TRUTH(e)        printf("_Static_assert(%s%s, \"%s\");\n", (e) ? "" : "!", #e, #e)

int main(void)
{
    printf("// gen_layout.c が生成したファイル。直接編集しない\n");
    printf("#include <stdarg.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <wchar.h>\n");
    printf("#include \"layout_types.h\"\n\n");

    // 基本型（AAPCS64 10.1 節）
    LAYOUT(char); LAYOUT(short); LAYOUT(int); LAYOUT(long); LAYOUT(long long);
    LAYOUT(float); LAYOUT(double); LAYOUT(long double); LAYOUT(void *);
    LAYOUT(_Bool); LAYOUT(__int128); LAYOUT(unsigned __int128); LAYOUT(__uint128_t); LAYOUT(size_t); LAYOUT(ptrdiff_t);
    LAYOUT(wchar_t); LAYOUT(wint_t); LAYOUT(max_align_t); LAYOUT(intmax_t);

    // 符号
    TRUTH((char)-1 > 0);
    TRUTH((wchar_t)-1 > 0);
    TRUTH((wint_t)-1 > 0);
    TRUTH(L'\xff' > 0);
    TRUTH(sizeof(L'a') == sizeof(wchar_t));
    TRUTH(sizeof('a') == sizeof(int));

    // va_list（AAPCS64 の「Variable argument lists」）
    LAYOUT(va_list);
    OFFSET(va_list, __stack); OFFSET(va_list, __gr_top); OFFSET(va_list, __vr_top);
    OFFSET(va_list, __gr_offs); OFFSET(va_list, __vr_offs);

    // 構造体・共用体
    LAYOUT(struct s_char_double); OFFSET(struct s_char_double, d);
    LAYOUT(struct s_char_ldouble); OFFSET(struct s_char_ldouble, ld);
    LAYOUT(struct s_hfa3); LAYOUT(struct s_hva); LAYOUT(union u_mixed);
    LAYOUT(struct s_flex); OFFSET(struct s_flex, data);
    LAYOUT(struct s_packed); OFFSET(struct s_packed, i);
    LAYOUT(struct s_aligned); OFFSET(struct s_aligned, c2);

    // ビットフィールド（AAPCS64 10.1.8 節）
    LAYOUT(struct bf_basic); LAYOUT(struct bf_cross); OFFSET(struct bf_cross, c);
    LAYOUT(struct bf_zero_int); OFFSET(struct bf_zero_int, b);
    LAYOUT(struct bf_zero_char);
    LAYOUT(struct bf_unnamed_long); LAYOUT(struct bf_long_long);
    LAYOUT(struct bf_char_type); LAYOUT(struct bf_short_span);
    LAYOUT(union u_bf_unnamed); LAYOUT(struct bf_unnamed_packed);
    LAYOUT(struct bf_unnamed_after); OFFSET(struct bf_unnamed_after, b);
    LAYOUT(struct bf_unnamed_pragma);

    // _BitInt（幅 64 を超えると 16 バイト単位、境界 16）
    LAYOUT(_BitInt(2)); LAYOUT(_BitInt(8)); LAYOUT(_BitInt(9)); LAYOUT(_BitInt(33));
    LAYOUT(_BitInt(64)); LAYOUT(_BitInt(65)); LAYOUT(_BitInt(128)); LAYOUT(_BitInt(129));
    LAYOUT(_BitInt(200)); LAYOUT(unsigned _BitInt(256));
    LAYOUT(struct s_char_int128); OFFSET(struct s_char_int128, i);

    // 列挙型
    LAYOUT(enum e_small); LAYOUT(enum e_big); LAYOUT(enum e_neg);
    return 0;
}
