// os/libc/termcap.c（u_ の名前でコンパイル）が ncurses（xterm-256color）と同じ答えを返すか（tools/libc/test_termcap.sh）
#include <stdio.h>
#include <string.h>
#include <term.h>
#include <termcap.h>

int u_tgetent(char *, const char *);
int u_tgetflag(const char *);
int u_tgetnum(const char *);
char *u_tgetstr(const char *, char **);
char *u_tgoto(const char *, int, int);
char *u_tparm(const char *, ...);

int main(void)
{
    char buf[4096];
    tgetent(buf, "xterm-256color");
    u_tgetent(buf, "xterm-256color");
    int bad = 0, checked = 0;
    for (int i = 0; boolcodes[i]; i++, checked++)
        if ((tgetflag(boolcodes[i]) > 0) != (u_tgetflag(boolcodes[i]) > 0)) {
            printf("flag %s\n", boolcodes[i]);
            bad++;
        }
    for (int i = 0; numcodes[i]; i++, checked++)
        if (tgetnum(numcodes[i]) != u_tgetnum(numcodes[i])) {
            printf("num %s\n", numcodes[i]);
            bad++;
        }
    static const int args[][2] = {{0, 0}, {1, 2}, {5, 79}, {23, 0}, {7, 8}, {15, 16}, {100, 255}, {200, 3}};
    for (int i = 0; strcodes[i]; i++) {
        char a1[4096], a2[4096], *p1 = a1, *p2 = a2;
        char *s1 = tgetstr(strcodes[i], &p1), *s2 = u_tgetstr(strcodes[i], &p2);
        if (s1 == (char *)-1)
            s1 = NULL;
        checked++;
        if (!s1 != !s2 || (s1 && strcmp(s1, s2))) {
            printf("str %s\n", strcodes[i]);
            bad++;
            continue;
        }
        // u6 は %p のない古い termcap の形（端末の答えを読むためのもので、出力には使わない）。ncurses は %p が
        // ないとき引数の順を独自に決めるので、比べない
        if (!s1 || !strcmp(strcodes[i], "u6"))
            continue;
        for (size_t k = 0; k < sizeof args / sizeof args[0]; k++) {
            char g1[1024], g2[1024], t1[1024], t2[1024];
            snprintf(g1, sizeof g1, "%s", tgoto(s1, args[k][0], args[k][1]));
            snprintf(g2, sizeof g2, "%s", u_tgoto(s2, args[k][0], args[k][1]));
            char *r1 = tparm(s1, (long)args[k][0], (long)args[k][1], 3L, 4L, 5L, 6L, 7L, 8L, 9L);
            snprintf(t1, sizeof t1, "%s", r1 ? r1 : "(null)");
            char *r2 = u_tparm(s2, (long)args[k][0], (long)args[k][1], 3L, 4L, 5L, 6L, 7L, 8L, 9L);
            snprintf(t2, sizeof t2, "%s", r2 ? r2 : "(null)");
            checked += 2;
            if (strcmp(g1, g2)) {
                printf("tgoto %s %d %d\n", strcodes[i], args[k][0], args[k][1]);
                bad++;
            }
            if (strcmp(t1, t2)) {
                printf("tparm %s %d %d\n", strcodes[i], args[k][0], args[k][1]);
                bad++;
            }
        }
    }
    printf("%d checks, %d differences\n", checked, bad);
    return bad != 0;
}
