// マルチバイト文字（UTF-8）と wchar_t（Unicode）の変換: mbrtowc・mbtowc・mblen・mbrlen・wcrtomb・wctomb・
// mbstowcs・wcstombs・mbsinit と MB_CUR_MAX（__locale_mb_cur_max）。
// picolibc をマルチバイト対応なしでビルドしていたとき（〜2026-11-14）、これらは 1 バイトを 1 文字として返した。今は
// マルチバイトありだが、picolibc のものはロケール（setlocale）が C なら ASCII だけ。Monocs はロケールを見ずに、いつも
// UTF-8 にする（文字はいつも UTF-8 の方針。Linux の UTF-8 のロケールと同じ動き）。
// Monocs の文字はいつも UTF-8（端末・ファイル名・curses）なので、Linux の UTF-8 のロケールと同じ動きにする
// （wcwidth.c と同じく、カーネルで定義して picolibc より先にリンクする。picolibc の各関数は 1 つずつ別のメンバー）。
// 出典: RFC 3629（UTF-8。長すぎる表し方、サロゲート U+D800〜DFFF、U+10FFFF より大きいものは誤り）、
//       C17 7.29.6（mbrtowc などの戻り値: (size_t)-2 は途中まで、(size_t)-1 は誤り（errno は EILSEQ））

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// MB_CUR_MAX（stdlib.h のマクロがこれを呼ぶ）。UTF-8 の 1 文字は 4 バイトまで
size_t __locale_mb_cur_max(void)
{
    return 4;
}

// 先頭のバイトから、続くバイトの数と、値の始まり。誤りなら -1。glibc と同じく、昔の 5・6 バイトの形（F8〜FD）と
// U+10FFFF を超える 4 バイトの形（F5〜F7）も、そろうまでは受け取り（(size_t)-2）、そろったところで誤りにする
static int lead(unsigned char c, uint32_t *v)
{
    static const struct { unsigned char lo, hi, mask; int more; } forms[] = {
        {0x00, 0x7F, 0x7F, 0}, {0xC2, 0xDF, 0x1F, 1}, {0xE0, 0xEF, 0x0F, 2},
        {0xF0, 0xF7, 0x07, 3}, {0xF8, 0xFB, 0x03, 4}, {0xFC, 0xFD, 0x01, 5},
    };
    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; i++)
        if (c >= forms[i].lo && c <= forms[i].hi) {
            *v = c & forms[i].mask;
            return forms[i].more;
        }
    return -1;      // 0x80〜0xC1（続きのバイト、長すぎる 2 バイトの始まり）、0xFE・0xFF
}

// 読み終えた値が正しいか（長すぎる表し方、サロゲート、範囲の外。5・6 バイトの形はいつも誤り）
static bool valid(uint32_t v, int len)
{
    static const uint32_t min[4] = {0, 0x80, 0x800, 0x10000};
    return len <= 4 && v >= min[len - 1] && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF);
}

// mbstate_t に途中の値を持つ: __count の下位 4 ビットが読んだバイトの数、その上が全体のバイトの数、
// __value.__wch が途中までの値
size_t mbrtowc(wchar_t *restrict pwc, const char *restrict s, size_t n, mbstate_t *restrict ps)
{
    static mbstate_t internal;
    if (!ps)
        ps = &internal;
    if (!s) {               // 状態を初めに戻す（mbrtowc(NULL, "", 1, ps) と同じ）
        pwc = nullptr;
        s = "";
        n = 1;
    }
    if (n == 0)
        return (size_t)-2;
    int have = ps->__count & 15, len = ps->__count >> 4;
    uint32_t v = (uint32_t)ps->__value.__wch;
    size_t used = 0;
    if (have == 0) {        // 先頭のバイト
        int more = lead((unsigned char)s[used++], &v);
        if (more < 0)
            goto bad;
        have = 1;
        len = more + 1;
    }
    if (len < 1 || len > 6 || have > len)
        goto bad;
    while (have < len && used < n) {    // 続きのバイト（10xxxxxx）
        unsigned char c = (unsigned char)s[used++];
        if ((c & 0xC0) != 0x80)
            goto bad;
        v = (v << 6) | (c & 0x3F);
        have++;
    }
    if (have < len) {       // 途中まで: 持っておいて、続きを待つ
        ps->__count = have | (len << 4);
        ps->__value.__wch = (wint_t)v;
        return (size_t)-2;
    }
    if (!valid(v, len))
        goto bad;
    ps->__count = 0;
    if (pwc)
        *pwc = (wchar_t)v;
    return v == 0 ? 0 : used;

bad:
    ps->__count = 0;
    errno = EILSEQ;
    return (size_t)-1;
}

size_t mbrlen(const char *restrict s, size_t n, mbstate_t *restrict ps)
{
    static mbstate_t internal;
    return mbrtowc(nullptr, s, n, ps ? ps : &internal);
}

int mbtowc(wchar_t *restrict pwc, const char *restrict s, size_t n)
{
    if (!s)
        return 0;           // UTF-8 は状態を持たない
    mbstate_t st = {0};
    size_t r = mbrtowc(pwc, s, n, &st);
    if (r == (size_t)-2) {
        errno = EILSEQ;
        return -1;
    }
    return r == (size_t)-1 ? -1 : (int)r;
}

int mblen(const char *s, size_t n)
{
    return mbtowc(nullptr, s, n);
}

int mbsinit(const mbstate_t *ps)
{
    return !ps || ps->__count == 0;
}

size_t wcrtomb(char *restrict s, wchar_t wc, mbstate_t *restrict ps)
{
    if (ps)
        ps->__count = 0;
    if (!s)
        return 1;           // 状態を初めに戻す（L'\0' を書いたのと同じ 1 バイト）
    uint32_t v = (uint32_t)wc;
    if (v < 0x80) {
        s[0] = (char)v;
        return 1;
    }
    if (v < 0x800) {
        s[0] = (char)(0xC0 | (v >> 6));
        s[1] = (char)(0x80 | (v & 0x3F));
        return 2;
    }
    if (v >= 0xD800 && v <= 0xDFFF)
        goto bad;
    if (v < 0x10000) {
        s[0] = (char)(0xE0 | (v >> 12));
        s[1] = (char)(0x80 | ((v >> 6) & 0x3F));
        s[2] = (char)(0x80 | (v & 0x3F));
        return 3;
    }
    if (v <= 0x10FFFF) {
        s[0] = (char)(0xF0 | (v >> 18));
        s[1] = (char)(0x80 | ((v >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((v >> 6) & 0x3F));
        s[3] = (char)(0x80 | (v & 0x3F));
        return 4;
    }
bad:
    errno = EILSEQ;
    return (size_t)-1;
}

int wctomb(char *s, wchar_t wc)
{
    if (!s)
        return 0;
    size_t r = wcrtomb(s, wc, nullptr);
    return r == (size_t)-1 ? -1 : (int)r;
}

// pwcs が NULL なら、変換した数だけを返す
size_t mbstowcs(wchar_t *restrict pwcs, const char *restrict s, size_t n)
{
    mbstate_t st = {0};
    size_t count = 0;
    while (!pwcs || count < n) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, s, 4, &st);
        if (r == (size_t)-1 || r == (size_t)-2) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (pwcs)
            pwcs[count] = wc;
        if (r == 0)
            return count;
        s += r;
        count++;
    }
    return count;
}

// s が NULL なら、書くバイト数だけを返す。n バイトに入りきらない文字は書かない
size_t wcstombs(char *restrict s, const wchar_t *restrict pwcs, size_t n)
{
    size_t count = 0;
    for (;; pwcs++) {
        char tmp[4];
        size_t r = wcrtomb(tmp, *pwcs, nullptr);
        if (r == (size_t)-1)
            return (size_t)-1;
        if (*pwcs == 0) {
            if (s && count < n)
                s[count] = 0;
            return count;
        }
        if (s) {
            if (count + r > n)
                return count;
            memcpy(s + count, tmp, r);
        }
        count += r;
    }
}
