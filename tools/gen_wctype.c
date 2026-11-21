// os/libc/wctype_table.h を作る（Linux で: gcc -O2 -o gen_wctype tools/gen_wctype.c && ./gen_wctype > os/libc/wctype_table.h）。
// glibc の C.UTF-8 のロケールで、U+0000〜U+10FFFF のすべての文字の分類（iswalpha など）と大文字・小文字（towupper・
// towlower）を調べて、範囲の表にする。Monocs の isw*・tow*（os/libc/wctype.c）は、Linux と同じ結果になる。
// digit・xdigit は ASCII だけ、alnum は alpha か digit、graph は print で space でないもの（glibc の性質。
// 違えば止まる）なので、表を持たない
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#define MAXC 0x110000

static void ranges(const char *name, int (*f)(wint_t))
{
    printf("static const struct wctype_range wctype_%s[] = {\n", name);
    int n = 0;
    for (unsigned c = 0; c < MAXC;) {
        if (!f(c)) {
            c++;
            continue;
        }
        unsigned s = c;
        while (c < MAXC && f(c))
            c++;
        printf("    {0x%X, 0x%X},\n", s, c - 1);
        n++;
    }
    printf("};  // %d\n\n", n);
}

// 大文字・小文字: c から c + delta へ。step（1 か 2）おきに同じ delta が続くものを 1 つの行にまとめる
static void mapping(const char *name, wint_t (*f)(wint_t))
{
    printf("static const struct wctype_map wctype_%s[] = {\n", name);
    int n = 0;
    for (unsigned c = 0; c < MAXC;) {
        int d = (int)f(c) - (int)c;
        if (!d) {
            c++;
            continue;
        }
        // step 1 と step 2 の、長く続くほう
        unsigned e1 = c, e2 = c;
        while (e1 + 1 < MAXC && (int)f(e1 + 1) - (int)(e1 + 1) == d)
            e1++;
        while (e2 + 2 < MAXC && (int)f(e2 + 2) - (int)(e2 + 2) == d && (int)f(e2 + 1) == (int)(e2 + 1))
            e2 += 2;
        unsigned step = e2 > e1 ? 2 : 1, e = e2 > e1 ? e2 : e1;
        printf("    {0x%X, 0x%X, %d, %u},\n", c, e, d, step);
        n++;
        c = step == 1 ? e + 1 : c + 1;
        if (step == 2) {
            // 間の文字（変わらないもの）は飛ばして、e の次から
            c = e + 1;
        }
    }
    printf("};  // %d\n\n", n);
}

static int f_alpha(wint_t c) { return iswalpha(c); }
static int f_upper(wint_t c) { return iswupper(c); }
static int f_lower(wint_t c) { return iswlower(c); }
static int f_punct(wint_t c) { return iswpunct(c); }
static int f_print(wint_t c) { return iswprint(c); }
static int f_space(wint_t c) { return iswspace(c); }
static int f_blank(wint_t c) { return iswblank(c); }
static int f_cntrl(wint_t c) { return iswcntrl(c); }

int main(void)
{
    if (!setlocale(LC_ALL, "C.UTF-8")) {
        fprintf(stderr, "C.UTF-8 がない\n");
        return 1;
    }
    for (unsigned c = 0; c < MAXC; c++) {
        bool digit = c >= '0' && c <= '9', xdigit = digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!!iswdigit(c) != digit || !!iswxdigit(c) != xdigit || !!iswalnum(c) != (iswalpha(c) || digit) ||
            !!iswgraph(c) != (iswprint(c) && !iswspace(c))) {
            fprintf(stderr, "U+%04X: digit・xdigit・alnum・graph の性質が違う\n", c);
            return 1;
        }
    }
    printf("// glibc（C.UTF-8）の文字の分類と大文字・小文字の表。tools/gen_wctype.c が作る（手で直さない）\n\n");
    ranges("alpha", f_alpha);
    ranges("upper", f_upper);
    ranges("lower", f_lower);
    ranges("punct", f_punct);
    ranges("print", f_print);
    ranges("space", f_space);
    ranges("blank", f_blank);
    ranges("cntrl", f_cntrl);
    mapping("toupper", towupper);
    mapping("tolower", towlower);
    return 0;
}
