// ファイルのワイド文字の入出力: fgetwc getwc getwchar fgetws ungetwc fputwc putwc putwchar fputws（2026-11-14）。
// picolibc は wchar_t の 4 バイトをそのまま読み書きするが、Linux（glibc の UTF-8 のロケール）はファイルの中を
// UTF-8 にする。Linux と同じになるように、UTF-8 で読み書きする（変換は os/libc/mbchar.c）。picolibc のものより
// 先にリンクされる（wcwidth.c と同じ）。
// glibc との違い: ストリームの向き（バイト用・ワイド用。fwide）は見ない（printf と fputws を同じストリームに
// 混ぜても、どちらも書ける。glibc では後のほうが失敗する）。ungetwc で戻せるのは 1 文字（picolibc の unget の枠）。
// ワイド文字の文字列のストリーム（swprintf・swscanf の中で picolibc が作るもの。初めから __SWIDE の印がある）は、
// picolibc と同じく wchar_t のまま読み書きする（picolibc の vfwprintf は putwc で書く）。ふつうのファイルには、この
// 印は付かない（picolibc の putwc・getwc だけが付け、ここの関数は付けない）。
// getwc_unlocked・ungetwc_unlocked（アプリには出さない）は picolibc の swscanf（vfwscanf）が使う（picolibc の
// 同じ部品を引き込むと、上の関数とぶつかるので、ここに書く）

#include <errno.h>
#include <stdio.h>
#include <wchar.h>

wint_t getwc_unlocked(FILE *f);

wint_t fgetwc(FILE *f)
{
    if (f->flags & __SWIDE)
        return getwc_unlocked(f);
    if (f->unget) {     // ungetwc で戻した文字
        wint_t wc = (wint_t)(f->unget - 1);
        f->unget = 0;
        return wc;
    }
    mbstate_t st = {0};
    for (;;) {
        int c = getc(f);
        if (c == EOF) {
            if (!mbsinit(&st))
                errno = EILSEQ;     // 文字の途中で終わった
            return WEOF;
        }
        char ch = (char)c;
        wchar_t w;
        size_t r = mbrtowc(&w, &ch, 1, &st);
        if (r == (size_t)-2)
            continue;
        if (r == (size_t)-1) {
            errno = EILSEQ;
            return WEOF;
        }
        return (wint_t)w;
    }
}

wint_t getwc(FILE *f)
{
    return fgetwc(f);
}

wint_t getwchar(void)
{
    return fgetwc(stdin);
}

wint_t ungetwc(wint_t wc, FILE *f)
{
    if (wc == WEOF || !f || !(f->flags & __SRD) || f->unget)
        return WEOF;
    f->unget = (__ungetc_t)wc + 1;
    f->flags &= ~__SEOF;    // POSIX: ungetwc は EOF の印を消す
    return wc;
}

// picolibc と同じ（wchar_t のまま読む。swscanf の文字列のストリーム用）
wint_t ungetwc_unlocked(wint_t wc, FILE *f);

wint_t getwc_unlocked(FILE *f)
{
    f->flags |= __SWIDE;
    if (!(f->flags & __SRD))
        return WEOF;
    if (f->unget) {
        wint_t wc = (wint_t)(f->unget - 1);
        f->unget = 0;
        return wc;
    }
    union {
        wchar_t wc;
        char c[sizeof(wchar_t)];
    } u;
    for (unsigned i = 0; i < sizeof(wchar_t); i++) {
        int c = f->get(f);
        if (c < 0)
            return WEOF;
        u.c[i] = (char)c;
    }
    return (wint_t)u.wc;
}

wint_t ungetwc_unlocked(wint_t wc, FILE *f)
{
    if (!(f->flags & __SRD) || wc == WEOF || f->unget)
        return WEOF;
    f->unget = (__ungetc_t)wc + 1;
    f->flags &= ~__SEOF;
    return wc;
}

wchar_t *fgetws(wchar_t *restrict s, int n, FILE *restrict f)
{
    if (n <= 0)
        return nullptr;
    int i = 0;
    while (i < n - 1) {
        wint_t wc = fgetwc(f);
        if (wc == WEOF) {
            if (i == 0)
                return nullptr;
            break;
        }
        s[i++] = (wchar_t)wc;
        if (wc == L'\n')
            break;
    }
    s[i] = L'\0';
    return s;
}

wint_t fputwc(wchar_t wc, FILE *f)
{
    if (f->flags & __SWIDE) {   // ワイド文字の文字列のストリーム: wchar_t のまま（picolibc と同じ）
        if (!(f->flags & __SWR))
            return WEOF;
        union {
            wchar_t wc;
            char c[sizeof(wchar_t)];
        } u = {.wc = wc};
        for (unsigned i = 0; i < sizeof(wchar_t); i++)
            if (f->put(u.c[i], f) < 0)
                return WEOF;
        return (wint_t)wc;
    }
    char b[4];
    size_t n = wcrtomb(b, wc, nullptr);
    if (n == (size_t)-1)
        return WEOF;    // errno は EILSEQ（wcrtomb）
    return fwrite(b, 1, n, f) == n ? (wint_t)wc : WEOF;
}

wint_t putwc(wchar_t wc, FILE *f)
{
    return fputwc(wc, f);
}

wint_t putwchar(wchar_t wc)
{
    return fputwc(wc, stdout);
}

int fputws(const wchar_t *restrict s, FILE *restrict f)
{
    for (; *s; s++)
        if (fputwc(*s, f) == WEOF)
            return -1;
    return 0;
}
