// termcap の関数（tgetent・tgetflag・tgetnum・tgetstr・tgoto・tputs）と tparm（2026-11-20、vim）
//
// Monocs には端末の型のデータベース（terminfo）がない。端末（ssh・telnet・USB・UART の先）はどれも xterm と同じに
// 扱い（os/curses/curses.c と同じ考え）、tgetent はどの名前でも xterm-256color の表（termcap_table.h。Pi 5 の
// ncurses から tools/gen_termcap.c で作る）を選ぶ。返す文字列は ncurses と同じく terminfo の形（%p1%d など）で、
// tgoto・tparm がそれを解く（ncurses の tgoto は terminfo の文字列を tparm(cap, row, col) で解く）。
// 確かめる: sh tools/libc/test_termcap.sh（ncurses と同じ答えか）
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../api/include/termcap.h"

#include "termcap_table.h"

// termcap の変数（Linux の ncurses と同じ名前・型）
char PC;
char *UP, *BC;
short ospeed;

int tgetent(char *bp, const char *name)
{
    (void)bp;
    (void)name;     // どの端末も xterm-256color
    return 1;
}

int tgetflag(const char *id)
{
    for (int i = 0; tc_flags[i]; i++)
        if (!strncmp(tc_flags[i], id, 2))
            return 1;
    return 0;
}

int tgetnum(const char *id)
{
    for (int i = 0; tc_nums[i].id; i++)
        if (!strncmp(tc_nums[i].id, id, 2))
            return tc_nums[i].n;
    return -1;
}

char *tgetstr(const char *id, char **area)
{
    for (int i = 0; tc_strs[i].id; i++) {
        if (strncmp(tc_strs[i].id, id, 2))
            continue;
        const char *s = tc_strs[i].s;
        if (area && *area) {    // ncurses と同じく、area があれば写して進める
            char *p = *area;
            strcpy(p, s);
            *area += strlen(s) + 1;
            return p;
        }
        return (char *)s;
    }
    return nullptr;
}

// ---- tparm（terminfo の引数つきの文字列。terminfo(5) の「Parameterized Strings」）----

#define TP_OUT 1024

typedef struct {
    long st[32];
    int sp;
} TpStack;

static void tp_push(TpStack *s, long v)
{
    if (s->sp < 32)
        s->st[s->sp++] = v;
}

static long tp_pop(TpStack *s)
{
    return s->sp > 0 ? s->st[--s->sp] : 0;
}

// p を、%? の条件の「%e」「%;」の位置まで飛ばす（入れ子の %? … %; は数える）。else_too なら %e でも止まる
static const char *tp_skip(const char *p, bool else_too)
{
    int level = 0;
    for (; *p; p++) {
        if (*p != '%' || !p[1])
            continue;
        p++;
        if (*p == '?')
            level++;
        else if (*p == ';') {
            if (level == 0)
                return p + 1;
            level--;
        } else if (*p == 'e' && level == 0 && else_too)
            return p + 1;
    }
    return p;
}

static char *tparm_v(const char *str, const long *params)
{
    static char out[TP_OUT];
    static long dyn[26], stat[26];  // %Pa〜%Pz（動的）、%PA〜%PZ（静的）
    long par[9];
    memcpy(par, params, sizeof par);
    TpStack s = {.sp = 0};
    size_t n = 0;
#define OUT(c) do { if (n + 1 < TP_OUT) out[n++] = (char)(c); } while (0)
    if (!str)
        return nullptr;
    for (const char *p = str; *p;) {
        if (*p != '%') {
            OUT(*p++);
            continue;
        }
        p++;
        // %[[:]flags][width[.precision]][doxXs]。- と + は「:」の後だけ書式の印（%- %+ は引き算・足し算）
        char fmt[16] = "%";
        size_t fl = 1;
        const char *flags = "# ";
        if (*p == ':') {
            flags = "-+# ";
            p++;
        }
        while (*p && strchr(flags, *p) && fl < sizeof fmt - 4)
            fmt[fl++] = *p++;
        while (*p && strchr("0123456789.", *p) && fl < sizeof fmt - 4)
            fmt[fl++] = *p++;
        char c = *p ? *p++ : '\0';
        switch (c) {
        case '%': OUT('%'); break;
        case 'c': { char ch = (char)tp_pop(&s); OUT(ch ? ch : '\200'); break; }  // NUL は \200（ncurses と同じ。文字列が切れないように）
        case 'd': case 'o': case 'x': case 'X': case 's': {
            char tmp[64];
            if (c == 's') {
                fmt[fl++] = 's';
            } else {
                fmt[fl++] = 'l';
                fmt[fl++] = c;
            }
            fmt[fl] = '\0';
            long v = tp_pop(&s);
            if (c == 's')
                snprintf(tmp, sizeof tmp, fmt, (const char *)v ? (const char *)v : "");
            else
                snprintf(tmp, sizeof tmp, fmt, v);
            for (char *t = tmp; *t; t++)
                OUT(*t);
            break;
        }
        case 'p': if (*p >= '1' && *p <= '9') tp_push(&s, par[*p - '1']); if (*p) p++; break;
        case 'P': {
            long v = tp_pop(&s);
            if (*p >= 'a' && *p <= 'z') dyn[*p - 'a'] = v;
            else if (*p >= 'A' && *p <= 'Z') stat[*p - 'A'] = v;
            if (*p) p++;
            break;
        }
        case 'g':
            if (*p >= 'a' && *p <= 'z') tp_push(&s, dyn[*p - 'a']);
            else if (*p >= 'A' && *p <= 'Z') tp_push(&s, stat[*p - 'A']);
            if (*p) p++;
            break;
        case '\'': tp_push(&s, (unsigned char)*p); if (*p) p++; if (*p == '\'') p++; break;
        case '{': {
            long v = 0;
            bool neg = *p == '-';
            if (neg) p++;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            if (*p == '}') p++;
            tp_push(&s, neg ? -v : v);
            break;
        }
        case 'l': { const char *t = (const char *)tp_pop(&s); tp_push(&s, t ? (long)strlen(t) : 0); break; }
        case '+': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a + b); break; }
        case '-': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a - b); break; }
        case '*': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a * b); break; }
        case '/': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, b ? a / b : 0); break; }
        case 'm': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, b ? a % b : 0); break; }
        case '&': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a & b); break; }
        case '|': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a | b); break; }
        case '^': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a ^ b); break; }
        case '=': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a == b); break; }
        case '<': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a < b); break; }
        case '>': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a > b); break; }
        case 'A': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a && b); break; }
        case 'O': { long b = tp_pop(&s), a = tp_pop(&s); tp_push(&s, a || b); break; }
        case '!': tp_push(&s, !tp_pop(&s)); break;
        case '~': tp_push(&s, ~tp_pop(&s)); break;
        case 'i': par[0]++; par[1]++; break;
        case '?': break;
        case 't': if (!tp_pop(&s)) p = tp_skip(p, true); break;
        case 'e': p = tp_skip(p, false); break;
        case ';': break;
        default: break;
        }
    }
    out[n] = '\0';
#undef OUT
    return out;
}

char *tparm(const char *str, ...)
{
    long par[9];
    va_list ap;
    va_start(ap, str);
    for (int i = 0; i < 9; i++)
        par[i] = va_arg(ap, long);
    va_end(ap);
    return tparm_v(str, par);
}

char *tiparm(const char *str, ...)
{
    // 引数の数は文字列の中の %pN の最大から決まる（ncurses の tiparm）。int で受けて long にする
    int count = 0;
    for (const char *p = str; p && *p; p++)
        if (p[0] == '%' && p[1] == 'p' && p[2] >= '1' && p[2] <= '9' && p[2] - '0' > count)
            count = p[2] - '0';
    long par[9] = {0};
    va_list ap;
    va_start(ap, str);
    for (int i = 0; i < count; i++)
        par[i] = va_arg(ap, int);
    va_end(ap);
    return tparm_v(str, par);
}

// ncurses と同じく、terminfo の文字列は tparm(cap, row, col)。引数を 3 つ以上使う文字列は NULL（ncurses と同じ）
char *tgoto(const char *cap, int col, int row)
{
    for (const char *p = cap; p && *p; p++)
        if (p[0] == '%' && p[1] == 'p' && p[2] >= '3' && p[2] <= '9')
            return nullptr;
    long par[9] = {row, col};
    return tparm_v(cap, par);
}

// $<…> の待ち（パディング）は出さずに飛ばす（端末は待たなくてよい速さ）
int tputs(const char *str, int affcnt, int (*putc_fn)(int))
{
    (void)affcnt;
    if (!str)
        return -1;
    for (const char *p = str; *p; p++) {
        if (p[0] == '$' && p[1] == '<') {
            const char *e = strchr(p, '>');
            if (e) {
                p = e;
                continue;
            }
        }
        putc_fn((unsigned char)*p);
    }
    return 0;
}
