// wcwidth: 文字の表示幅（全角 2、結合文字 0、制御文字 -1）。picolibc のものより先にリンクされる（ldtoa.c と同じ）。
// （2026-11-14 までの）picolibc はマルチバイト対応なし（mb-capable=false）でビルドしていて、picolibc の wcwidth は
// 表示できる文字をすべて 1 とする。curses（os/curses/curses.c）とアプリ（端末のエディタなど）が
// 全角を正しく数えられるように、Unicode の表（os/libc/wcwidth_table.h。tools/gen_wcwidth.py で作る）で引く。
// 分け方は glibc の wcwidth と同じ（表の生成のコメント）。

#include <wchar.h>
#include "wcwidth_table.h"

int wcwidth(wchar_t c);

static bool in_table(unsigned c, const struct wcwidth_range *t, int n)
{
    int lo = 0, hi = n - 1;
    if (c < t[0].first || c > t[n - 1].last)
        return false;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c > t[mid].last)
            lo = mid + 1;
        else if (c < t[mid].first)
            hi = mid - 1;
        else
            return true;
    }
    return false;
}

int wcwidth(wchar_t wc)
{
    unsigned c = (unsigned)wc;
    if (c == 0)
        return 0;
    if (c < 0x20 || (c >= 0x7F && c < 0xA0))
        return -1;      // 制御文字
    if (c < 0x7F)
        return 1;
    if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF)
        return -1;      // サロゲート・範囲外
    if (in_table(c, wcwidth_zero, sizeof(wcwidth_zero) / sizeof(wcwidth_zero[0])))
        return 0;
    if (in_table(c, wcwidth_wide, sizeof(wcwidth_wide) / sizeof(wcwidth_wide[0])))
        return 2;
    return 1;
}

// wcswidth: 文字列の n 文字までの表示幅の合計。表示できない文字があれば -1（POSIX）。picolibc の wcswidth は
// picolibc の wcwidth と同じ部品にあり、使うと 2 つの wcwidth がぶつかるので、ここにも書く
int wcswidth(const wchar_t *s, size_t n);

int wcswidth(const wchar_t *s, size_t n)
{
    int sum = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        int w = wcwidth(s[i]);
        if (w < 0)
            return -1;
        sum += w;
    }
    return sum;
}
