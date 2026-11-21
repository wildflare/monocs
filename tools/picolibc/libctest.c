// picolibc の動作確認（GCC でビルドした picolibc と slimcc でビルドした picolibc で、出力が同じか比べる）
// Monocs 上で run して出力を比べる。結果は 1 行ずつ出し、最後に全体のハッシュを出す
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t hash = 1469598103934665603ULL;
static int lines;

static void out(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (const char *p = buf; *p; p++)
        hash = (hash ^ (unsigned char)*p) * 1099511628211ULL;
    lines++;
#ifdef VERBOSE
    printf("%s\n", buf); // すべての行を出す（glibc の出力と行ごとに比べるとき）
#endif
    if (lines % 200 == 0)
        printf("%d lines, hash %016llx\n", lines, (unsigned long long)hash);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static uint32_t rng = 12345;
static uint32_t next(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 1;
}

static double rnd_double(void)
{
    // さまざまな大きさ・符号の値
    double m = (double)next() / 0x7fffffff;
    int e = (int)(next() % 80) - 40;
    double v = ldexp(m, e);
    return next() & 1 ? -v : v;
}

int main(void)
{
    // printf の書式
    static const double specials[] = {0.0, -0.0, 1.0, -1.0, 0.1, 1e-300, 1e300, 123456789.125,
                                      2.2250738585072014e-308, 4.9e-324, 1.7976931348623157e308};
    for (size_t i = 0; i < sizeof(specials) / sizeof(*specials); i++) {
        double d = specials[i];
        out("%g %e %f %.17g %a %10.3f %-12.5e|", d, d, d, d, d, d, d);
    }
    out("%f %f %f", (double)INFINITY, -(double)INFINITY, (double)NAN);
    for (int i = 0; i < 400; i++) {
        double d = rnd_double();
        out("%.17g %.3e %.10f %g %a", d, d, d, d, d);
    }
    out("%d %i %u %x %X %o %ld %lld %llu %hhd %hd %zu %p %c %s %%", -42, 42, 4000000000u, 0xbeef,
        0xBEEF, 0777, -1234567890123L, -9000000000000000000LL, 18000000000000000000ULL, (char)200,
        (short)-30000, (size_t)99, (void *)0x1234, 'x', "str");
    out("[%5d] [%-5d] [%05d] [%+d] [% d] [%.3s] [%10.4s] [%*d] [%-*d]", 42, 42, 42, 42, 42,
        "abcdef", "abcdef", 6, 7, 6, 7);

    // strtod・strtol・atof・sscanf
    static const char *const nums[] = {"0", "1", "-1.5", "3.14159265358979323846", "1e10", "1e-10",
                                       "0x1.8p3", "inf", "-INFINITY", "nan", "  42abc",
                                       "123456789012345678901234567890", "4.9e-324", "2.5e-324",
                                       "1.7976931348623159e308", ".5", "5.", "-0", "0x10", "077"};
    for (size_t i = 0; i < sizeof(nums) / sizeof(*nums); i++) {
        char *end;
        double d = strtod(nums[i], &end);
        float f = strtof(nums[i], NULL);
        long l = strtol(nums[i], NULL, 0);
        unsigned long long u = strtoull(nums[i], NULL, 10);
        out("%s -> %.17g %.9g %ld %llu rest=%td", nums[i], d, (double)f, l, u, end - nums[i]);
    }
    for (int i = 0; i < 300; i++) {
        char buf[64];
        double d = rnd_double();
        snprintf(buf, sizeof(buf), "%.17g", d);
        double back = strtod(buf, NULL);
        out("%s %d", buf, back == d);
    }
    int a, b;
    char word[16];
    double x;
    int n = sscanf("12 -34 hello 5.5", "%d %d %15s %lf", &a, &b, word, &x);
    out("%d %d %d %s %g", n, a, b, word, x);

    // 数学関数
    for (int i = 0; i < 300; i++) {
        double v = rnd_double();
        double p = fabs(v);
        out("%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g", sin(v), cos(v), tan(v), exp(v / 64),
            log(p), sqrt(p), atan2(v, 1.5), cbrt(v));
        out("%.17g %.17g %.17g %.17g %.17g %.17g", pow(p, 0.37), fmod(v, 3.25), floor(v), ceil(v),
            round(v), trunc(v));
        out("%.9g %.9g %.9g %.9g", (double)sinf((float)v), (double)expf((float)v / 64),
            (double)logf((float)p), (double)sqrtf((float)p));
        out("%.9g %.9g %d %d", (double)hypot(v, 2.5), (double)asinh(v), ilogb(p), signbit(v) != 0);
    }
    out("%d %d %d %d", isnan(NAN), isinf(INFINITY), isfinite(1.0), fpclassify(0.0) == FP_ZERO);

    // long double（binary128）
    long double ld = 1.0L;
    for (int i = 0; i < 40; i++) {
        ld = ld * 3.0L / 7.0L + 0.125L;
        out("%.30Lg %.30Lg %.20Lg", ld, sqrtl(ld), (long double)(double)ld);
    }

    // 文字列
    char s1[64], s2[64];
    strcpy(s1, "Hello, world");
    strcat(s1, "!");
    out("%s %zu %d %d %s %s", s1, strlen(s1), strcmp(s1, "Hello"), strncmp(s1, "Hello", 5),
        strchr(s1, 'w'), strrchr(s1, 'o'));
    out("%s %zu %zu", strstr(s1, "world"), strspn(s1, "Hel"), strcspn(s1, ","));
    memset(s2, 'x', 10);
    s2[10] = '\0';
    memmove(s2 + 2, s2, 5);
    memcpy(s2, "ab", 2);
    out("%s %d %p", s2, memcmp(s1, s2, 3) > 0, memchr(s1, 'z', strlen(s1)));
    for (int i = 0; i < 128; i++)
        out("%d %d %d %d %d %d", i, isalpha(i) != 0, isdigit(i) != 0, isspace(i) != 0, toupper(i),
            tolower(i));
    char tok[] = "a,b;;c,d";
    for (char *t = strtok(tok, ",;"); t; t = strtok(NULL, ",;"))
        out("tok %s", t);

    // qsort・bsearch
    int arr[500];
    for (int i = 0; i < 500; i++)
        arr[i] = (int)(next() % 1000) - 500;
    qsort(arr, 500, sizeof(int), cmp_int);
    for (int i = 0; i < 500; i += 25)
        out("%d", arr[i]);
    int key = arr[123];
    int *found = bsearch(&key, arr, 500, sizeof(int), cmp_int);
    out("%d", found && *found == key);

    // malloc・realloc
    char *pp[50];
    for (int i = 0; i < 50; i++) {
        pp[i] = malloc(16 + i * 37);
        memset(pp[i], i, 16 + i * 37);
    }
    for (int i = 0; i < 50; i += 2)
        pp[i] = realloc(pp[i], 1000 + i);
    int ok = 1;
    for (int i = 1; i < 50; i += 2)
        for (int j = 0; j < 16 + i * 37; j++)
            ok &= pp[i][j] == i;
    for (int i = 0; i < 50; i++)
        free(pp[i]);
    out("malloc %d", ok);

    printf("total %d lines, hash %016llx\n", lines, (unsigned long long)hash);
    return 0;
}
