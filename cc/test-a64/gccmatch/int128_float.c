// __int128（slimcc では _BitInt(128)）と浮動小数点の変換が GCC と同じか（2026-11-20。QuickJS の intl・temporal）
#include <stdio.h>
#include <stdint.h>

static unsigned long long rng = 88172645463325252ULL;
static unsigned long long next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void show_i(__int128 v)
{
    printf("%016llx%016llx", (unsigned long long)((unsigned __int128)v >> 64), (unsigned long long)v);
}

int main(void)
{
    static const double ds[] = {0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.9999, -1.9999, 3e9, -3e9, 9007199254740991.0,
                                9007199254740993.0, 1.8446744073709552e19, -9.223372036854776e18, 1e30, -1e30,
                                1.7014118346046923e38, -1.7014118346046923e38, 123456789.987654321, 4503599627370495.5};
    for (size_t i = 0; i < sizeof ds / sizeof ds[0]; i++) {
        __int128 s = (__int128)ds[i];
        printf("d2i %.17g ", ds[i]);
        show_i(s);
        if (ds[i] >= 0) {
            unsigned __int128 u = (unsigned __int128)ds[i];
            printf(" u ");
            show_i((__int128)u);
        }
        float f = (float)ds[i];
        printf(" f2i ");
        show_i((__int128)f);
        printf("\n");
    }
    static const __int128 is[] = {0, 1, -1, 2, (__int128)1 << 53, ((__int128)1 << 53) + 1, ((__int128)1 << 53) + 3,
                                  ((__int128)1 << 64) - 1, (__int128)1 << 100, -((__int128)1 << 100) - 1,
                                  ((__int128)1 << 126) + ((__int128)1 << 72) + 1, ((__int128)1 << 24) + 1};
    for (size_t i = 0; i < sizeof is / sizeof is[0]; i++) {
        printf("i2d ");
        show_i(is[i]);
        printf(" %.17g %.9g\n", (double)is[i], (double)(float)is[i]);
    }
    // 乱数: 丸めの境目を含むいろいろな大きさ
    for (int i = 0; i < 2000; i++) {
        int shift = (int)(next() % 127);
        __int128 v = (__int128)(((unsigned __int128)next() << 64 | next()) >> shift);
        if (next() & 1)
            v = -v;
        unsigned __int128 u = ((unsigned __int128)next() << 64 | next()) >> (next() % 128);
        double d = (double)v, du = (double)u;
        float f = (float)v, fu = (float)u;
        double back = (double)(next() % 1000000) * 1e-3 * (double)((__int128)1 << (next() % 100));
        printf("r %a %a %a %a ", d, du, (double)f, (double)fu);
        show_i((__int128)back);
        printf("\n");
    }
    return 0;
}
