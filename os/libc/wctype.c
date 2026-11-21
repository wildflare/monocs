// ワイド文字の分類（iswalpha など）と大文字・小文字（towupper・towlower）。picolibc のものより先にリンクされる
// （wcwidth.c と同じ）。picolibc をマルチバイト対応なしでビルドしていたときは ASCII（と Latin-1 の一部）しか
// 分類しなかった（今はマルチバイトあり。それでも glibc と同じ表にするため、置き換えたまま）。Linux（glibc の C.UTF-8）と同じ結果になるように、glibc から作った表（os/libc/wctype_table.h。
// tools/gen_wctype.c で作る。glibc 2.41、Unicode 16.0）で引く。ロケールは見ない（Monocs の文字はいつも UTF-8）。
// 確かめる: sh tools/libc/test_wctype.sh（すべての文字で glibc と比べる）

#include <stdint.h>
#include <string.h>
#include <wctype.h>

struct wctype_range {
    unsigned first, last;
};
struct wctype_map {
    unsigned first, last;
    int delta;
    unsigned step;  // 1: 範囲のすべて、2: first から 1 つおき
};
#include "wctype_table.h"

#define COUNT(t) (sizeof(t) / sizeof((t)[0]))

static bool in(unsigned c, const struct wctype_range *t, size_t n)
{
    if (n == 0 || c < t[0].first || c > t[n - 1].last)
        return false;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c > t[mid].last)
            lo = mid + 1;
        else if (c < t[mid].first)
            hi = mid;
        else
            return true;
    }
    return false;
}

static wint_t map(wint_t wc, const struct wctype_map *t, size_t n)
{
    unsigned c = (unsigned)wc;
    size_t lo = 0, hi = n;
    while (lo < hi) {   // first が c 以下の最後の行
        size_t mid = (lo + hi) / 2;
        if (t[mid].first <= c)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return wc;
    const struct wctype_map *m = &t[lo - 1];
    if (c > m->last || (c - m->first) % m->step)
        return wc;
    return (wint_t)((int)c + m->delta);
}

int iswalpha(wint_t c) { return in(c, wctype_alpha, COUNT(wctype_alpha)); }
int iswupper(wint_t c) { return in(c, wctype_upper, COUNT(wctype_upper)); }
int iswlower(wint_t c) { return in(c, wctype_lower, COUNT(wctype_lower)); }
int iswpunct(wint_t c) { return in(c, wctype_punct, COUNT(wctype_punct)); }
int iswprint(wint_t c) { return in(c, wctype_print, COUNT(wctype_print)); }
int iswspace(wint_t c) { return in(c, wctype_space, COUNT(wctype_space)); }
int iswblank(wint_t c) { return in(c, wctype_blank, COUNT(wctype_blank)); }
int iswcntrl(wint_t c) { return in(c, wctype_cntrl, COUNT(wctype_cntrl)); }
int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswxdigit(wint_t c) { return iswdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iswalnum(wint_t c) { return iswalpha(c) || iswdigit(c); }
int iswgraph(wint_t c) { return iswprint(c) && !iswspace(c); }
wint_t towupper(wint_t c) { return map(c, wctype_toupper, COUNT(wctype_toupper)); }
wint_t towlower(wint_t c) { return map(c, wctype_tolower, COUNT(wctype_tolower)); }

// wctype・iswctype（名前で選ぶ。0 は「ない名前」）
static const char *const class_names[] = {
    "", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit",
};
static int (*const class_fns[])(wint_t) = {
    nullptr, iswalnum, iswalpha, iswblank, iswcntrl, iswdigit, iswgraph, iswlower, iswprint, iswpunct, iswspace,
    iswupper, iswxdigit,
};

wctype_t wctype(const char *name)
{
    for (size_t i = 1; i < COUNT(class_names); i++)
        if (!strcmp(name, class_names[i]))
            return (wctype_t)i;
    return 0;
}

int iswctype(wint_t c, wctype_t desc)
{
    size_t i = (size_t)desc;
    return i > 0 && i < COUNT(class_fns) ? class_fns[i](c) : 0;
}

// wctrans・towctrans（1: toupper、2: tolower。picolibc の wctrans_t は int。Linux の試験（glibc はポインタ）でも
// 通るように、整数を経由して書く）
wctrans_t wctrans(const char *name)
{
    return (wctrans_t)(uintptr_t)(!strcmp(name, "toupper") ? 1 : !strcmp(name, "tolower") ? 2 : 0);
}

wint_t towctrans(wint_t c, wctrans_t t)
{
    uintptr_t k = (uintptr_t)t;
    return k == 1 ? towupper(c) : k == 2 ? towlower(c) : c;
}
