#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
size_t u_mbrtowc(wchar_t *, const char *, size_t, mbstate_t *);
size_t u_wcrtomb(char *, wchar_t, mbstate_t *);
static long bad, beyond; static int shown;
static void check(const unsigned char *b, int len)
{
    mbstate_t s1 = {0}, s2 = {0};
    wchar_t w1 = 0, w2 = 0;
    size_t r1 = mbrtowc(&w1, (const char *)b, len, &s1), r2 = u_mbrtowc(&w2, (const char *)b, len, &s2);
    if (r1 != (size_t)-1 && r1 != (size_t)-2 && r1 > 0 && (unsigned)w1 > 0x10FFFF) { beyond++; if (r2 == (size_t)-1) return; }
    if (r1 != r2 || (r1 < 5 && r1 > 0 && w1 != w2)) {
        if (bad++ < 2000000 && ++shown < 25) {
            printf("diff len %d:", len);
            for (int i = 0; i < len; i++) printf(" %02x", b[i]);
            printf("  glibc %zd U+%X  monocs %zd U+%X\n", (ssize_t)r1, (unsigned)w1, (ssize_t)r2, (unsigned)w2);
        }
    }
}
int main(void)
{
    setlocale(LC_ALL, "C.UTF-8");
    unsigned char b[4];
    long n = 0;
    for (int a = 0; a < 256; a++) { b[0] = a; check(b, 1); n++;
        for (int c = 0; c < 256; c++) { b[1] = c; check(b, 2); n++;
            for (int d = 0; d < 256; d++) { b[2] = d; check(b, 3); n++; } } }
    srand(1);
    for (long i = 0; i < 20000000; i++) { b[0] = 0xF0 + rand() % 16; b[1] = rand(); b[2] = rand(); b[3] = rand(); check(b, 4); n++; }
    for (unsigned v = 0; v < 0x110000; v++) { char x[4], y[4]; mbstate_t s = {0};
        size_t r1 = wcrtomb(x, v, &s), r2 = u_wcrtomb(y, v, NULL);
        if (r1 != r2 || (r1 != (size_t)-1 && memcmp(x, y, r1))) { if (bad++ < 10) printf("wcrtomb diff U+%X\n", v); } n++; }
    for (long i = 0; i < 2000000; i++) {
        unsigned char x[6]; for (int k = 0; k < 6; k++) x[k] = (k == 0) ? 0xC0 + rand() % 64 : 0x80 + rand() % 64;
        if (rand() % 4 == 0) x[1 + rand() % 5] = rand();
        mbstate_t s1 = {0}, s2 = {0}; size_t r1 = (size_t)-2, r2 = (size_t)-2; wchar_t w1 = 0, w2 = 0;
        for (int k = 0; k < 6 && r1 == (size_t)-2; k++) { r1 = mbrtowc(&w1, (char *)x + k, 1, &s1); r2 = u_mbrtowc(&w2, (char *)x + k, 1, &s2);
            if ((r1 == 1 && (unsigned)w1 > 0x10FFFF && r2 == (size_t)-1)) break;
            if (r1 != r2 || (r1 == 1 && w1 != w2)) { if (bad++ < 10) printf("bytewise diff at %d: %02x %02x %02x\n", k, x[0], x[1], x[2]); break; } }
        n++;
    }
    printf("compared %ld, differences %ld (glibc accepts beyond U+10FFFF, monocs rejects: %ld)\n", n, bad, beyond);
    return bad != 0;
}
