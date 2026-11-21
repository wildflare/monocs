// os/libc/glob.c を Linux の glibc の glob と比べる（sh tools/libc/test_glob.sh）。
// Monocs の glob は monocs_glob（-Dglob=monocs_glob でコンパイル）。glob_t は glibc と同じ形なので、そのまま渡す
#define _GNU_SOURCE
#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int monocs_glob(const char *, int, int (*)(const char *, int), glob_t *);
void monocs_globfree(glob_t *);

// GLOB_MAGCHAR は、合ったものがあったときだけ比べる（合わないとき・GLOB_NOCHECK でパターンを返したときの
// glibc の立て方は、どこで諦めたかで変わる。使うプログラムは、合ったときに見る）
static int show(char *buf, size_t size, int r, glob_t *g, int offs, int fl)
{
    int mag = r == 0 && !(fl & GLOB_NOCHECK) ? !!(g->gl_flags & GLOB_MAGCHAR) : -1;
    int n = snprintf(buf, size, "r=%d n=%zu magchar=%d:", r, r == 0 ? g->gl_pathc : 0, mag);
    if (r == 0)
        for (size_t i = 0; i < (size_t)offs + g->gl_pathc; i++)
            n += snprintf(buf + n, size - (size_t)n, " [%s]", g->gl_pathv[i] ? g->gl_pathv[i] : "(null)");
    return n;
}

int main(void)
{
    static const char *pats[] = {
        "*", "*.c", ".*", "*/", "sub/*", "*/*.c", "s*/*", "sub/deep/*.txt", "/*", "a.c", "nothing", "no*",
        "[ab].c", "[!a]*", "?.c", "sub\\/a", "\\*", "*.{c,h}", "{sub,other}/*", "{a,b,zz}.c", "~", "~/x*",
        "sub/../a.c", "./*.c", "dir.d/*", "*.d", "sub//deep/*", "sub/*/b.txt", "[", "a[", ".h*",
    };
    static const int flagsets[] = {
        0, GLOB_MARK, GLOB_NOCHECK, GLOB_NOSORT | GLOB_MARK, GLOB_NOESCAPE, GLOB_PERIOD, GLOB_BRACE,
        GLOB_NOMAGIC, GLOB_TILDE, GLOB_TILDE_CHECK, GLOB_ONLYDIR, GLOB_BRACE | GLOB_NOCHECK, GLOB_DOOFFS,
    };
    int fail = 0, total = 0;
    for (size_t f = 0; f < sizeof flagsets / sizeof flagsets[0]; f++)
        for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) {
            int fl = flagsets[f];
            glob_t a = {.gl_offs = 2}, b = {.gl_offs = 2};
#ifdef ONLY_MONOCS
            if (monocs_glob(pats[i], fl, NULL, &b) == 0)
                monocs_globfree(&b);
            total++;
            continue;
#endif
            int ra = glob(pats[i], fl, NULL, &a), rb = monocs_glob(pats[i], fl, NULL, &b);
            char sa[8192], sb[8192];
            int offs = (fl & GLOB_DOOFFS) ? 2 : 0;
            show(sa, sizeof sa, ra, &a, offs, fl);
            show(sb, sizeof sb, rb, &b, offs, fl);
            if (fl & GLOB_NOSORT) {     // 並べない: 順は比べない（中身だけ）
                if (ra == 0) qsort(a.gl_pathv, a.gl_pathc, sizeof(char *), (int (*)(const void *, const void *))strcmp);
            }
            total++;
            if (strcmp(sa, sb) && !(fl & GLOB_NOSORT)) {
                printf("DIFF flags=%#x %s\n  glibc: %s\n  monocs:  %s\n", fl, pats[i], sa, sb);
                fail++;
            }
            if (ra == 0) globfree(&a);
            if (rb == 0) monocs_globfree(&b);
        }
#ifdef ONLY_MONOCS
    printf("monocs glob under AddressSanitizer: %d patterns ok\n", total);
    return 0;
#endif
    // GLOB_APPEND
    glob_t a = {0}, b = {0};
    glob("*.c", 0, NULL, &a); glob("sub/*", GLOB_APPEND, NULL, &a);
    monocs_glob("*.c", 0, NULL, &b); monocs_glob("sub/*", GLOB_APPEND, NULL, &b);
    char sa[8192], sb[8192];
    show(sa, sizeof sa, 0, &a, 0, 0); show(sb, sizeof sb, 0, &b, 0, 0);
    total++;
    if (strcmp(sa, sb)) { printf("DIFF append\n  glibc: %s\n  monocs:  %s\n", sa, sb); fail++; }
    globfree(&a); monocs_globfree(&b);
    printf("%d/%d same\n", total - fail, total);
    return fail != 0;
}
