// os/libc/termcap_table.h を作る（Pi 5 の ncurses から。2026-11-20、vim）
//   gcc -o build/gen_termcap tools/gen_termcap.c -ltinfo && build/gen_termcap > os/libc/termcap_table.h
// ncurses の tgetent("xterm-256color") の後、すべての termcap の名前（boolcodes・numcodes・strcodes）について
// tgetflag・tgetnum・tgetstr が返すものを書き出す。文字列は ncurses と同じく terminfo の形（%p1%d など）のまま。
// Monocs の端末（ssh・telnet・USB・UART の先）は xterm と同じに扱う（os/curses/curses.c と同じ考え）
#include <stdio.h>
#include <string.h>
#include <term.h>
#include <termcap.h>

static void put_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20 || c >= 0x7f)
            printf("\\%03o", c);
        else
            putchar(c);
    }
    putchar('"');
}

int main(void)
{
    char buf[4096];
    if (tgetent(buf, "xterm-256color") != 1) {
        fprintf(stderr, "no xterm-256color\n");
        return 1;
    }
    printf("// tools/gen_termcap.c が Pi 5 の ncurses（xterm-256color）から作った表。手で直さない\n");
    printf("// 真偽\nstatic const char *const tc_flags[] = {");
    for (int i = 0; boolcodes[i]; i++)
        if (tgetflag(boolcodes[i]) > 0)
            printf("\"%s\", ", boolcodes[i]);
    printf("nullptr};\n");
    printf("// 数\nstatic const struct { const char *id; int n; } tc_nums[] = {\n");
    for (int i = 0; numcodes[i]; i++) {
        int n = tgetnum(numcodes[i]);
        if (n >= 0)
            printf("    {\"%s\", %d},\n", numcodes[i], n);
    }
    printf("    {nullptr, 0}};\n");
    printf("// 文字列（terminfo の形）\nstatic const struct { const char *id; const char *s; } tc_strs[] = {\n");
    for (int i = 0; strcodes[i]; i++) {
        char area[4096], *ap = area;
        char *s = tgetstr(strcodes[i], &ap);
        if (s && s != (char *)-1) {
            printf("    {\"%s\", ", strcodes[i]);
            put_str(s);
            printf("},\n");
        }
    }
    printf("    {nullptr, nullptr}};\n");
    return 0;
}
