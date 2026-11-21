// Linux 用のプロジェクトの Makefile を読んで、`run DIR` がコンパイルするもの（.c の一覧、オプション）と、
// できるプログラムの名前を決める（2026-11-14。ユーザーと合意: Linux と同じソース・同じ Makefile のまま動かす）。
//
// GNU make の「読む」部分だけを行う。規則のコマンドは動かさない（Monocs は実行ファイルを作らず、ソースから動かす）。
//   - 行の続き（\）、コメント（#）、代入（= := ::= += ?= !=）、export・override、
//     条件（ifeq・ifneq・ifdef・ifndef・else・endif）、define〜endef（変数として）、規則の行（目標: 前提）
//   - 展開: $(VAR) ${VAR} $X $$、置き換え $(VAR:.c=.o)・$(VAR:%.c=%.o)、関数 subst patsubst strip findstring filter
//     filter-out sort word words firstword lastword wordlist dir notdir suffix basename addprefix addsuffix join
//     wildcard abspath realpath if or and foreach call value origin。shell は空（コマンドを動かさない）、
//     info・warning・error も空。include は読まない（.d などの依存の記録に使われる）
//   - 定義のない変数は環境変数から。CC は "cc"
// 決め方: 既定のゴール（.DEFAULT_GOAL、なければ最初の規則の最初の目標。そこから見つからなければ all）から前提を
// たどり、前提に .c か .o を持つ最初の目標（.a のライブラリは除く）をプログラムとする（all: $(TARGET)、
// $(TARGET): $(OBJS) の形）。プログラムの前提の .a は、その .a の前提の .o を含める（2026-11-16。Lua の Makefile）。その .c（.o は同じ名前の .c）が
// コンパイルするもの。オプションは $(CPPFLAGS) $(CFLAGS) の -std= -D -U -I と、$(LDFLAGS) $(LDLIBS) $(LIBS) の
// -l名前 のうち /lib/名前 があるもの（-lncursesw・-lm などは Monocs が持っているので要らない）
// 出典: GNU make の手引き（GNU Make Manual 4.4）の 3.3（Makefile の名前）、5.1・6（変数と代入の種類、置き換え）、
//       7（条件）、8（関数）、4.8（.DEFAULT_GOAL）、2.3（既定のゴール: . で始まる目標は、/ を含まなければ選ばない）

#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "makefile.h"

#define MAX_DEPTH 32        // 展開の入れ子の深さ（自分を参照する変数で止まらないように）
#define LIB_DIR "/usr/lib"

// ---- 文字列 ----

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (b->n + n + 1 > cap)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;     // 足りなければ切れる（Makefile は小さいので、ふつうは起きない）
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void bputs(Buf *b, const char *s)
{
    bput(b, s, strlen(s));
}

static void bputc(Buf *b, char c)
{
    bput(b, &c, 1);
}

// 単語を空白で区切って足す（最初の単語の前には空白を入れない）
static void bword(Buf *b, const char *s, size_t n, bool *first)
{
    if (!*first)
        bputc(b, ' ');
    *first = false;
    bput(b, s, n);
}

static char *bdone(Buf *b)
{
    if (!b->p)
        b->p = calloc(1, 1);
    return b->p;
}

static char *dupn(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

static char *sdup(const char *s)
{
    return dupn(s, strlen(s));
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 次の単語（空白で区切る）。なければ nullptr
static const char *next_word(const char **s, size_t *len)
{
    const char *p = *s;
    while (*p && is_space(*p))
        p++;
    if (!*p) {
        *s = p;
        return nullptr;
    }
    const char *start = p;
    while (*p && !is_space(*p))
        p++;
    *len = (size_t)(p - start);
    *s = p;
    return start;
}

static char *trim(char *s)
{
    while (is_space(*s))
        s++;
    size_t n = strlen(s);
    while (n && is_space(s[n - 1]))
        s[--n] = '\0';
    return s;
}

// ---- 変数と規則 ----

typedef struct {
    char *name, *value;
    bool recursive;     // = で定義した（使うたびに展開する）
    bool cmdline;       // make の引数の 変数=値（Makefile の中の代入より強い。GNU make と同じ）
} Var;

typedef struct {
    char *targets, *prereqs;    // 展開した後の、空白で区切った並び
    char *recipe;               // コマンドの行（展開する前。\n で区切る。$(MAKE) の呼び直しを読むため）
} Rule;

typedef struct {
    char dir[512];
    char target[256];   // コマンドの行を展開するときの $@
    Var *vars;
    int nvars, capvars;
    Rule *rules;
    int nrules, caprules;
    int depth;
    int include_depth;      // include の入れ子
    // コマンドの行を展開するときの自動変数（$< $^ $*。$@ は target）
    const char *auto_first, *auto_all, *auto_stem;
    bool recipe_flags;      // .o のコマンドの行からオプションを取った
    bool builtin_flags;     // コマンドの行のない .o がある（GNU make の組み込みの規則: $(CPPFLAGS) $(CFLAGS)）
} Make;

static Var *find_var(Make *m, const char *name, size_t n)
{
    for (int i = m->nvars - 1; i >= 0; i--)     // 後から足したもの（foreach・call の一時的なもの）が先
        if (strlen(m->vars[i].name) == n && !memcmp(m->vars[i].name, name, n))
            return &m->vars[i];
    return nullptr;
}

static Var *push_var(Make *m, const char *name, char *value, bool recursive)
{
    if (m->nvars == m->capvars) {
        int cap = m->capvars ? m->capvars * 2 : 32;
        Var *v = realloc(m->vars, sizeof(Var) * (size_t)cap);
        if (!v) {
            free(value);
            return nullptr;
        }
        m->vars = v;
        m->capvars = cap;
    }
    Var *v = &m->vars[m->nvars++];
    *v = (Var){sdup(name), value, recursive, false};
    return v;
}

static void pop_var(Make *m)
{
    Var *v = &m->vars[--m->nvars];
    free(v->name);
    free(v->value);
}

static void set_var(Make *m, const char *name, char *value, bool recursive)
{
    Var *v = find_var(m, name, strlen(name));
    if (v && v->cmdline) {
        free(value);    // make の引数で決めたものは変えない
        return;
    }
    if (v) {
        free(v->value);
        v->value = value;
        v->recursive = recursive;
    } else {
        push_var(m, name, value, recursive);
    }
}

static void expand_into(Make *m, Buf *out, const char *s, size_t n);

static char *expandn(Make *m, const char *s, size_t n)
{
    Buf b = {0};
    expand_into(m, &b, s, n);
    return bdone(&b);
}

static char *expand(Make *m, const char *s)
{
    return expandn(m, s, strlen(s));
}

// 変数の値を out に（= のものは展開する）
static void put_var(Make *m, Buf *out, const char *name, size_t n)
{
    Var *v = find_var(m, name, n);
    if (v) {
        if (v->recursive)
            expand_into(m, out, v->value, strlen(v->value));
        else
            bputs(out, v->value);
        return;
    }
    char key[128];
    if (n >= sizeof key)
        return;
    memcpy(key, name, n);
    key[n] = '\0';
    if (!strcmp(key, "CC")) {
        bputs(out, "cc");
        return;
    }
    if (!strcmp(key, "MAKE")) {
        bputs(out, "make");     // コマンドの中の $(MAKE) の呼び直しを読む（read_level）
        return;
    }
    if (!strcmp(key, "@")) {
        bputs(out, m->target);  // 今読んでいる規則の目標（コマンドの中の $(MAKE) $@）
        return;
    }
    if (!strcmp(key, "<") || !strcmp(key, "^") || !strcmp(key, "+") || !strcmp(key, "*")) {
        const char *a = key[0] == '<' ? m->auto_first : key[0] == '*' ? m->auto_stem : m->auto_all;
        if (a)
            bputs(out, a);
        return;
    }
    const char *env = getenv(key);
    if (env)
        bputs(out, env);
}

// ---- パターン（%） ----

// word が pat（% を 1 つまで含む）に合うか。合えば stem に % の部分
static bool pat_match(const char *pat, size_t pn, const char *w, size_t wn, const char **stem, size_t *sn)
{
    const char *pc = memchr(pat, '%', pn);
    if (!pc) {
        *stem = w;
        *sn = 0;
        return pn == wn && !memcmp(pat, w, wn);
    }
    size_t pre = (size_t)(pc - pat), suf = pn - pre - 1;
    if (wn < pre + suf || memcmp(w, pat, pre) || memcmp(w + wn - suf, pc + 1, suf))
        return false;
    *stem = w + pre;
    *sn = wn - pre - suf;
    return true;
}

// rep の % を stem にして足す
static void pat_put(Buf *out, const char *rep, size_t rn, const char *stem, size_t sn)
{
    const char *rc = memchr(rep, '%', rn);
    if (!rc) {
        bput(out, rep, rn);
        return;
    }
    bput(out, rep, (size_t)(rc - rep));
    bput(out, stem, sn);
    bput(out, rc + 1, rn - (size_t)(rc - rep) - 1);
}

// text の各単語を pat → rep（patsubst と置き換えの参照）
static void patsubst(Buf *out, const char *pat, const char *rep, const char *text)
{
    bool first = true;
    size_t wn;
    const char *w;
    while ((w = next_word(&text, &wn))) {
        const char *stem;
        size_t sn;
        if (!first)
            bputc(out, ' ');
        first = false;
        if (pat_match(pat, strlen(pat), w, wn, &stem, &sn))
            pat_put(out, rep, strlen(rep), stem, sn);
        else
            bput(out, w, wn);
    }
}

// ---- 関数 ----

// s[0..n) をトップレベルの , で最大 max 個に分ける（最後のものは残りすべて。make と同じ）
static int split_args(const char *s, size_t n, const char **arg, size_t *alen, int max)
{
    int k = 0, depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < n && k < max - 1; i++) {
        if (s[i] == '(' || s[i] == '{')
            depth++;
        else if ((s[i] == ')' || s[i] == '}') && depth > 0)
            depth--;
        else if (s[i] == ',' && depth == 0) {
            arg[k] = s + start;
            alen[k++] = i - start;
            start = i + 1;
        }
    }
    arg[k] = s + start;
    alen[k++] = n - start;
    return k;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// wildcard: パターンごとに、合う名前を並べて（make 4.3 から並べる）足す。パスは Makefile のフォルダから
static void wildcard(Make *m, Buf *out, const char *pats, bool *first)
{
    size_t wn;
    const char *w;
    while ((w = next_word(&pats, &wn))) {
        char pat[512];
        if (wn >= sizeof pat)
            continue;
        memcpy(pat, w, wn);
        pat[wn] = '\0';
        char *slash = strrchr(pat, '/');
        char sub[512], dirpath[1100];
        const char *base = slash ? slash + 1 : pat;
        snprintf(sub, sizeof sub, "%.*s", slash ? (int)(slash - pat + 1) : 0, pat);
        if (!strpbrk(base, "*?[")) {        // 文字どおりの名前: あれば
            struct stat st;
            snprintf(dirpath, sizeof dirpath, "%s%s%s", pat[0] == '/' ? "" : m->dir, pat[0] == '/' ? "" : "/", pat);
            if (stat(dirpath, &st) == 0)
                bword(out, pat, wn, first);
            continue;
        }
        if (pat[0] == '/')
            snprintf(dirpath, sizeof dirpath, "%s", sub[0] ? sub : "/");
        else
            snprintf(dirpath, sizeof dirpath, "%s/%s", m->dir, sub);
        DIR *d = opendir(dirpath);
        if (!d)
            continue;
        char **names = nullptr;
        int n = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && base[0] != '.')
                continue;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || fnmatch(base, e->d_name, 0))
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                char **p = realloc(names, sizeof(char *) * (size_t)cap);
                if (!p)
                    break;
                names = p;
            }
            names[n++] = sdup(e->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof(char *), cmp_str);
        for (int i = 0; i < n; i++) {
            if (!*first)
                bputc(out, ' ');
            *first = false;
            bputs(out, sub);
            bputs(out, names[i]);
            free(names[i]);
        }
        free(names);
    }
}

// 単語ごとの変換（dir notdir suffix basename addprefix addsuffix abspath）
enum word_op { W_DIR, W_NOTDIR, W_SUFFIX, W_BASENAME, W_PREFIX, W_ADDSUFFIX, W_ABSPATH };

static void each_word(Make *m, Buf *out, enum word_op op, const char *text, const char *arg)
{
    bool first = true;
    size_t wn;
    const char *w;
    while ((w = next_word(&text, &wn))) {
        const char *slash = nullptr;
        for (size_t i = 0; i < wn; i++)
            if (w[i] == '/')
                slash = w + i;
        const char *dot = nullptr;
        for (const char *p = slash ? slash + 1 : w; p < w + wn; p++)
            if (*p == '.')
                dot = p;
        switch (op) {
        case W_DIR:
            if (slash)
                bword(out, w, (size_t)(slash - w + 1), &first);
            else
                bword(out, "./", 2, &first);
            break;
        case W_NOTDIR:
            if (slash)
                bword(out, slash + 1, (size_t)(w + wn - slash - 1), &first);
            else
                bword(out, w, wn, &first);
            break;
        case W_SUFFIX:
            if (dot)
                bword(out, dot, (size_t)(w + wn - dot), &first);
            break;
        case W_BASENAME:
            bword(out, w, dot ? (size_t)(dot - w) : wn, &first);
            break;
        case W_PREFIX:
            bword(out, arg, strlen(arg), &first);
            bput(out, w, wn);
            break;
        case W_ADDSUFFIX:
            bword(out, w, wn, &first);
            bputs(out, arg);
            break;
        case W_ABSPATH:
            bword(out, "", 0, &first);
            if (w[0] != '/') {
                bputs(out, m->dir);
                bputc(out, '/');
            }
            bput(out, w, wn);
            break;
        }
    }
}

// filter・filter-out
static void filter(Buf *out, const char *pats, const char *text, bool keep)
{
    bool first = true;
    size_t wn;
    const char *w;
    while ((w = next_word(&text, &wn))) {
        bool match = false;
        const char *p = pats;
        size_t pn;
        const char *pw;
        while (!match && (pw = next_word(&p, &pn))) {
            const char *stem;
            size_t sn;
            match = pat_match(pw, pn, w, wn, &stem, &sn);
        }
        if (match == keep)
            bword(out, w, wn, &first);
    }
}

static int count_words(const char *s)
{
    int n = 0;
    size_t wn;
    while (next_word(&s, &wn))
        n++;
    return n;
}

// n 番目（1 から）の単語
static const char *nth_word(const char *s, int n, size_t *wn)
{
    const char *w;
    for (int i = 1; (w = next_word(&s, wn)); i++)
        if (i == n)
            return w;
    return nullptr;
}

// 関数を呼ぶ。name が関数でなければ false
// uname の答え（$(shell uname)・`uname` の中）。Monocs は Linux ではない（Linux と名乗ると dlopen などの要る作りを選ぶ）
static char *read_file(const char *path);

static const char *uname_answer(const char *cmd)
{
    if (!strcmp(cmd, "uname") || !strcmp(cmd, "uname -s"))
        return "Monocs";
    if (!strcmp(cmd, "uname -m") || !strcmp(cmd, "uname -p"))
        return "aarch64";
    return nullptr;
}

static bool call_function(Make *m, Buf *out, const char *name, size_t nn, const char *args, size_t an)
{
    static const struct { const char *name; int nargs; } funcs[] = {
        {"subst", 3}, {"patsubst", 3}, {"strip", 1}, {"findstring", 2}, {"filter", 2}, {"filter-out", 2},
        {"sort", 1}, {"word", 2}, {"words", 1}, {"firstword", 1}, {"lastword", 1}, {"wordlist", 3},
        {"dir", 1}, {"notdir", 1}, {"suffix", 1}, {"basename", 1}, {"addprefix", 2}, {"addsuffix", 2},
        {"join", 2}, {"wildcard", 1}, {"abspath", 1}, {"realpath", 1}, {"if", 3}, {"or", 64}, {"and", 64},
        {"foreach", 3}, {"call", 64}, {"value", 1}, {"origin", 1}, {"shell", 1}, {"info", 1},
        {"warning", 1}, {"error", 1}, {"eval", 1},
    };
    int fi = -1;
    for (int i = 0; i < (int)(sizeof funcs / sizeof funcs[0]); i++)
        if (strlen(funcs[i].name) == nn && !memcmp(funcs[i].name, name, nn))
            fi = i;
    if (fi < 0)
        return false;
    const char *fname = funcs[fi].name;
    const char *arg[64];
    size_t alen[64];
    int na = split_args(args, an, arg, alen, funcs[fi].nargs);

    // 引数を展開せずに使うもの（if・or・and・foreach・call・value・shell など）
    if (!strcmp(fname, "if")) {
        char *raw = expandn(m, arg[0], alen[0]);
        bool yes = trim(raw)[0] != '\0';
        free(raw);
        if (yes ? na > 1 : na > 2)
            expand_into(m, out, arg[yes ? 1 : 2], alen[yes ? 1 : 2]);
        return true;
    }
    if (!strcmp(fname, "or") || !strcmp(fname, "and")) {
        bool is_or = fname[0] == 'o';
        char *last = nullptr;
        for (int i = 0; i < na; i++) {
            free(last);
            Buf b = {0};
            expand_into(m, &b, arg[i], alen[i]);
            last = bdone(&b);
            const char *t = last;
            size_t wn;
            bool empty = next_word(&t, &wn) == nullptr;
            if (is_or && !empty) {
                bputs(out, trim(last));
                free(last);
                return true;
            }
            if (!is_or && empty) {
                free(last);
                return true;
            }
        }
        if (!is_or && last)
            bputs(out, trim(last));
        free(last);
        return true;
    }
    if (!strcmp(fname, "foreach")) {
        if (na < 3)
            return true;
        char *raw = expandn(m, arg[0], alen[0]);
        char *var = trim(raw);
        char *list = expandn(m, arg[1], alen[1]);
        bool first = true;
        const char *p = list;
        size_t wn;
        const char *w;
        while ((w = next_word(&p, &wn))) {
            if (!push_var(m, var, dupn(w, wn), false))
                break;
            char *r = expandn(m, arg[2], alen[2]);
            pop_var(m);
            if (r[0])
                bword(out, r, strlen(r), &first);
            free(r);
        }
        free(list);
        free(raw);
        return true;
    }
    if (!strcmp(fname, "call")) {
        char *raw = expandn(m, arg[0], alen[0]);
        char *fn = trim(raw);
        Var *v = find_var(m, fn, strlen(fn));
        char *body = v ? sdup(v->value) : nullptr;
        int pushed = 0;
        for (int i = 0; i < na && body; i++) {
            char num[12];
            snprintf(num, sizeof num, "%d", i);
            char *val = i == 0 ? sdup(fn) : expandn(m, arg[i], alen[i]);
            if (push_var(m, num, val, false))
                pushed++;
        }
        if (body)
            expand_into(m, out, body, strlen(body));
        while (pushed--)
            pop_var(m);
        free(body);
        free(raw);
        return true;
    }
    if (!strcmp(fname, "value")) {
        char *raw = expandn(m, arg[0], alen[0]);
        char *n = trim(raw);
        Var *v = find_var(m, n, strlen(n));
        if (v)
            bputs(out, v->value);
        free(raw);
        return true;
    }
    if (!strcmp(fname, "shell")) {
        // コマンドは動かさない。uname と cat FILE だけは答える（Makefile が OS を見分ける・版を読むのによく使う。2026-11-16）
        char *c = expandn(m, arg[0], alen[0]);
        const char *cmd = trim(c);
        const char *u = uname_answer(cmd);
        if (u)
            bputs(out, u);
        // cat FILE（1 つのファイル。2026-11-20、QuickJS の CONFIG_VERSION=$(shell cat VERSION)）: 中身を読む。GNU make と
        // 同じく、改行は空白に、終わりの改行は取る
        if (!u && !strncmp(cmd, "cat ", 4) && !strpbrk(cmd + 4, " \t;|&<>$`*?'\"")) {
            char path[1100];
            snprintf(path, sizeof path, "%s%s%s", cmd[4] == '/' ? "" : m->dir, cmd[4] == '/' ? "" : "/", cmd + 4);
            char *text = read_file(path);
            if (text) {
                size_t n = strlen(text);
                while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
                    text[--n] = '\0';
                for (char *q = text; *q; q++)
                    if (*q == '\n' || *q == '\r')
                        *q = ' ';
                bputs(out, text);
                free(text);
            }
        }
        free(c);
        return true;
    }
    if (!strcmp(fname, "info") || !strcmp(fname, "warning") || !strcmp(fname, "error") || !strcmp(fname, "eval"))
        return true;    // 表示しない

    // ほかは、引数を展開してから
    char *a[3] = {};
    for (int i = 0; i < na && i < 3; i++)
        a[i] = expandn(m, arg[i], alen[i]);
    const char *a0 = a[0] ? a[0] : "", *a1 = a[1] ? a[1] : "", *a2 = a[2] ? a[2] : "";
    bool first = true;
    if (!strcmp(fname, "subst")) {
        size_t fl = strlen(a0);
        for (const char *p = a2; *p;) {
            const char *q = fl ? strstr(p, a0) : nullptr;
            if (!q) {
                bputs(out, p);
                break;
            }
            bput(out, p, (size_t)(q - p));
            bputs(out, a1);
            p = q + fl;
        }
    } else if (!strcmp(fname, "patsubst")) {
        patsubst(out, trim(a[0]), trim(a[1]), a2);
    } else if (!strcmp(fname, "strip")) {
        const char *p = a0;
        size_t wn;
        const char *w;
        while ((w = next_word(&p, &wn)))
            bword(out, w, wn, &first);
    } else if (!strcmp(fname, "findstring")) {
        if (strstr(a1, a0))
            bputs(out, a0);
    } else if (!strcmp(fname, "filter") || !strcmp(fname, "filter-out")) {
        filter(out, a0, a1, fname[6] == '\0');
    } else if (!strcmp(fname, "sort")) {
        int n = count_words(a0);
        char **w = calloc((size_t)n + 1, sizeof(char *));
        const char *p = a0;
        size_t wn;
        const char *s;
        for (int i = 0; w && (s = next_word(&p, &wn)); i++)
            w[i] = dupn(s, wn);
        if (w) {
            qsort(w, (size_t)n, sizeof(char *), cmp_str);
            for (int i = 0; i < n; i++) {
                if (i == 0 || strcmp(w[i], w[i - 1]))
                    bword(out, w[i], strlen(w[i]), &first);
            }
            for (int i = 0; i < n; i++)
                free(w[i]);
            free(w);
        }
    } else if (!strcmp(fname, "word")) {
        size_t wn;
        const char *w = nth_word(a1, atoi(a0), &wn);
        if (w)
            bput(out, w, wn);
    } else if (!strcmp(fname, "words")) {
        char num[16];
        snprintf(num, sizeof num, "%d", count_words(a0));
        bputs(out, num);
    } else if (!strcmp(fname, "firstword") || !strcmp(fname, "lastword")) {
        size_t wn;
        const char *w = nth_word(a0, fname[0] == 'f' ? 1 : count_words(a0), &wn);
        if (w)
            bput(out, w, wn);
    } else if (!strcmp(fname, "wordlist")) {
        int s = atoi(a0), e = atoi(a1);
        const char *p = a2;
        size_t wn;
        const char *w;
        for (int i = 1; (w = next_word(&p, &wn)); i++)
            if (i >= s && i <= e)
                bword(out, w, wn, &first);
    } else if (!strcmp(fname, "dir")) {
        each_word(m, out, W_DIR, a0, nullptr);
    } else if (!strcmp(fname, "notdir")) {
        each_word(m, out, W_NOTDIR, a0, nullptr);
    } else if (!strcmp(fname, "suffix")) {
        each_word(m, out, W_SUFFIX, a0, nullptr);
    } else if (!strcmp(fname, "basename")) {
        each_word(m, out, W_BASENAME, a0, nullptr);
    } else if (!strcmp(fname, "addprefix")) {
        each_word(m, out, W_PREFIX, a1, a0);
    } else if (!strcmp(fname, "addsuffix")) {
        each_word(m, out, W_ADDSUFFIX, a1, a0);
    } else if (!strcmp(fname, "abspath") || !strcmp(fname, "realpath")) {
        each_word(m, out, W_ABSPATH, a0, nullptr);
    } else if (!strcmp(fname, "join")) {
        const char *p = a0, *q = a1;
        size_t pn, qn;
        const char *pw, *qw;
        for (;;) {
            pw = next_word(&p, &pn);
            qw = next_word(&q, &qn);
            if (!pw && !qw)
                break;
            bword(out, pw ? pw : "", pw ? pn : 0, &first);
            if (qw)
                bput(out, qw, qn);
        }
    } else if (!strcmp(fname, "wildcard")) {
        wildcard(m, out, a0, &first);
    } else if (!strcmp(fname, "origin")) {
        char *n = trim(a[0] ? a[0] : (char *)"");
        bputs(out, find_var(m, n, strlen(n)) ? "file" : getenv(n) ? "environment" : "undefined");
    }
    for (int i = 0; i < 3; i++)
        free(a[i]);
    return true;
}

// $( の中身（s[0..n)）を展開して out に
static void reference(Make *m, Buf *out, const char *s, size_t n)
{
    // 関数: 名前（英小文字と -）に空白が続く
    size_t k = 0;
    while (k < n && (islower((unsigned char)s[k]) || s[k] == '-'))
        k++;
    if (k > 0 && k < n && (s[k] == ' ' || s[k] == '\t')) {
        size_t a = k;
        while (a < n && (s[a] == ' ' || s[a] == '\t'))
            a++;
        if (call_function(m, out, s, k, s + a, n - a))
            return;
    }
    char *name = expandn(m, s, n);      // $($(X)) のように、名前も展開する
    char *colon = strchr(name, ':');
    char *eq = colon ? strchr(colon, '=') : nullptr;
    if (colon && eq) {
        // 置き換えの参照: $(VAR:a=b) は a で終わる単語を b に、$(VAR:%.c=%.o) は patsubst と同じ
        *colon = *eq = '\0';
        Buf v = {0};
        put_var(m, &v, name, strlen(name));
        char *val = bdone(&v);
        char pat[256], rep[256];
        if (strchr(colon + 1, '%')) {
            snprintf(pat, sizeof pat, "%s", colon + 1);
            snprintf(rep, sizeof rep, "%s", eq + 1);
        } else {
            snprintf(pat, sizeof pat, "%%%s", colon + 1);
            snprintf(rep, sizeof rep, "%%%s", eq + 1);
        }
        patsubst(out, pat, rep, val);
        free(val);
    } else {
        put_var(m, out, name, strlen(name));
    }
    free(name);
}

static void expand_into(Make *m, Buf *out, const char *s, size_t n)
{
    if (m->depth >= MAX_DEPTH)
        return;
    m->depth++;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '$') {
            size_t j = i;
            while (j < n && s[j] != '$')
                j++;
            bput(out, s + i, j - i);
            i = j - 1;
            continue;
        }
        if (i + 1 >= n)
            break;
        char c = s[i + 1];
        if (c == '$') {
            bputc(out, '$');
            i++;
        } else if (c == '(' || c == '{') {
            char close = c == '(' ? ')' : '}';
            int depth = 1;
            size_t j = i + 2;
            for (; j < n; j++) {
                if (s[j] == c)
                    depth++;
                else if (s[j] == close && --depth == 0)
                    break;
            }
            reference(m, out, s + i + 2, j - i - 2);
            i = j;
        } else {
            put_var(m, out, s + i + 1, 1);     // $X（自動変数の $@ $< などは空）
            i++;
        }
    }
    m->depth--;
}

// ---- 読む ----

static void add_rule(Make *m, char *targets, char *prereqs)
{
    if (m->nrules == m->caprules) {
        int cap = m->caprules ? m->caprules * 2 : 16;
        Rule *r = realloc(m->rules, sizeof(Rule) * (size_t)cap);
        if (!r) {
            free(targets);
            free(prereqs);
            return;
        }
        m->rules = r;
        m->caprules = cap;
    }
    m->rules[m->nrules++] = (Rule){targets, prereqs, nullptr};
}

// 条件（ifeq・ifneq の (a,b)・"a" "b"・'a' 'b'）が成り立つか
static bool eval_cond(Make *m, const char *kw, const char *arg)
{
    if (!strcmp(kw, "ifdef") || !strcmp(kw, "ifndef")) {
        char *raw = expand(m, arg);
        char *n = trim(raw);
        Buf b = {0};
        put_var(m, &b, n, strlen(n));
        char *v = bdone(&b);
        bool def = v[0] != '\0';    // make は「値が空でない」かで決める
        free(v);
        free(raw);
        return kw[2] == 'd' ? def : !def;
    }
    char *a = nullptr, *b = nullptr;
    while (is_space(*arg))
        arg++;
    if (*arg == '(') {
        const char *end = arg + strlen(arg);
        while (end > arg && end[-1] != ')')
            end--;
        const char *parts[2];
        size_t plen[2];
        int k = split_args(arg + 1, end > arg + 1 ? (size_t)(end - arg - 2) : 0, parts, plen, 2);
        a = expandn(m, parts[0], plen[0]);
        b = k > 1 ? expandn(m, parts[1], plen[1]) : sdup("");
    } else {
        // "a" "b" の形
        char *q[2] = {};
        const char *p = arg;
        for (int i = 0; i < 2; i++) {
            while (is_space(*p))
                p++;
            char quote = *p;
            if (quote != '"' && quote != '\'')
                break;
            const char *e = strchr(p + 1, quote);
            if (!e)
                break;
            q[i] = expandn(m, p + 1, (size_t)(e - p - 1));
            p = e + 1;
        }
        a = q[0] ? q[0] : sdup("");
        b = q[1] ? q[1] : sdup("");
    }
    // (a,b) の形では、a の後ろと b の前の空白は比べない（make と同じく、両端を落とす）
    bool eq = !strcmp(trim(a), trim(b));
    free(a);
    free(b);
    return kw[2] == 'e' ? eq : !eq;     // ifeq・ifneq
}

// 行の先頭の語（directive）と、その後ろ
static bool starts_word(const char *line, const char *kw, const char **rest)
{
    size_t n = strlen(kw);
    if (strncmp(line, kw, n) || (line[n] && !is_space(line[n]) && line[n] != '('))
        return false;
    *rest = line + n;
    return true;
}

#define MAX_COND 32

typedef struct {
    bool parent;    // 外側が生きているか
    bool taken;     // どれかの枝を選んだ
    bool active;    // 今の枝が生きているか
} Cond;

// 代入か規則の行
static void parse(Make *m, char *text);

static void statement(Make *m, char *line, bool *in_rule)
{
    const char *rest;
    while (starts_word(line, "export", &rest) || starts_word(line, "override", &rest) ||
           starts_word(line, "private", &rest)) {
        line = (char *)rest;
        while (is_space(*line))
            line++;
    }
    if (!*line)
        return;
    // include: そのファイルを読む（フォルダからの位置。ないものは飛ばす: -include と同じ。2026-11-20、vim の
    // auto/config.mk）
    if (starts_word(line, "include", &rest) || starts_word(line, "-include", &rest) ||
        starts_word(line, "sinclude", &rest)) {
        if (m->include_depth >= 8)
            return;
        char *files = expand(m, rest);
        const char *fp = files;
        size_t wn;
        const char *w;
        while ((w = next_word(&fp, &wn))) {
            char path[1100];
            snprintf(path, sizeof path, "%s%s%.*s", w[0] == '/' ? "" : m->dir, w[0] == '/' ? "" : "/", (int)wn, w);
            char *text = read_file(path);
            if (!text)
                continue;
            m->include_depth++;
            parse(m, text);
            m->include_depth--;
            free(text);
        }
        free(files);
        return;
    }
    if (starts_word(line, "vpath", &rest) || starts_word(line, "unexport", &rest))
        return;
    if (starts_word(line, "undefine", &rest)) {
        char *raw = expand(m, rest);
        char *n = trim(raw);
        Var *v = find_var(m, n, strlen(n));
        if (v) {
            free(v->value);
            v->value = sdup("");
            v->recursive = false;
        }
        free(raw);
        return;
    }
    // 代入か規則か: 括弧の外の最初の = か :
    int depth = 0;
    size_t i = 0, n = strlen(line);
    for (; i < n; i++) {
        char c = line[i];
        if (c == '$' && i + 1 < n && (line[i + 1] == '(' || line[i + 1] == '{')) {
            depth++;
            i++;
        } else if ((c == ')' || c == '}') && depth > 0) {
            depth--;
        } else if (depth == 0 && (c == '=' || c == ':')) {
            break;
        }
    }
    if (i == n)
        return;     // どちらでもない（$(eval ...) だけの行など）
    size_t name_end = i, val_start;
    char op;
    if (line[i] == '=') {
        op = '=';
        if (i > 0 && strchr("+?!", line[i - 1])) {
            op = line[i - 1];
            name_end = i - 1;
        }
        val_start = i + 1;
    } else if (line[i + 1] == '=') {
        op = ':';
        val_start = i + 2;
    } else if (line[i + 1] == ':' && line[i + 2] == '=') {
        op = ':';
        val_start = i + 3;
    } else {
        // 規則: 目標: 前提 [; コマンド]（:: も）
        size_t p = i + 1 + (line[i + 1] == ':');
        char *pre = line + p;
        char *semi = strchr(pre, ';');
        if (semi)
            *semi = '\0';
        // 目標ごとの変数（target: VAR = value）は使わない
        for (char *q = pre; *q; q++)
            if (*q == '=')
                return;
        char *targets = expandn(m, line, i);
        char *prereqs = expand(m, pre);
        for (char *q = prereqs; *q; q++)
            if (*q == '|')
                *q = ' ';   // 順序だけの前提も前提として扱う
        add_rule(m, targets, prereqs);
        *in_rule = true;
        return;
    }
    *in_rule = false;
    char *raw_name = expandn(m, line, name_end);
    char *name = trim(raw_name);
    char *value = line + val_start;
    while (is_space(*value))
        value++;
    size_t vl = strlen(value);
    while (vl && is_space(value[vl - 1]))
        value[--vl] = '\0';
    if (!*name) {
        free(raw_name);
        return;
    }
    Var *v = find_var(m, name, strlen(name));
    switch (op) {
    case '=':
        set_var(m, name, sdup(value), true);
        break;
    case ':':
        set_var(m, name, expand(m, value), false);
        break;
    case '?':
        if (!v && !getenv(name))
            set_var(m, name, sdup(value), true);
        break;
    case '!':
        set_var(m, name, sdup(""), false);    // シェルのコマンドの結果: 動かさない
        break;
    case '+':
        if (v && v->cmdline)
            break;
        if (!v) {
            set_var(m, name, sdup(value), true);
        } else {
            Buf b = {0};
            bputs(&b, v->value);
            if (v->value[0] && *value)
                bputc(&b, ' ');
            if (v->recursive) {
                bputs(&b, value);
            } else {
                char *e = expand(m, value);
                bputs(&b, e);
                free(e);
            }
            free(v->value);
            v->value = bdone(&b);
        }
        break;
    }
    free(raw_name);
}

// 行のコメント（# から。\# は # そのもの）を取る
static void strip_comment(char *line)
{
    for (char *p = line; *p; p++) {
        if (*p == '\\' && p[1] == '#') {
            memmove(p, p + 1, strlen(p));
            continue;
        }
        if (*p == '#') {
            *p = '\0';
            return;
        }
    }
}

static void parse(Make *m, char *text)
{
    Cond cond[MAX_COND];
    int ncond = 0;
    bool in_rule = false;
    char *define_name = nullptr;    // define の中（値をためる）
    Buf define_val = {0};
    int define_depth = 0;
    char *p = text;
    while (*p) {
        // 1 行（\ の続きは 1 つの空白にまとめる）
        Buf lb = {0};
        for (;;) {
            char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            size_t l = len;
            if (l && p[l - 1] == '\r')
                l--;
            size_t bs = 0;
            while (bs < l && p[l - 1 - bs] == '\\')
                bs++;
            bool cont = bs % 2 == 1;
            if (cont) {
                size_t k = l - 1;
                while (k > 0 && is_space(p[k - 1]))
                    k--;
                bput(&lb, p, k);
                bputc(&lb, ' ');
            } else {
                bput(&lb, p, l);
            }
            p += len + (e ? 1 : 0);
            if (!cont || !*p)
                break;
            while (*p == ' ' || *p == '\t')
                p++;
        }
        char *line = bdone(&lb);
        bool active = ncond == 0 || cond[ncond - 1].active;

        if (define_name) {
            const char *rest;
            char *t = line;
            while (is_space(*t))
                t++;
            if (starts_word(t, "endef", &rest) && --define_depth == 0) {
                if (active)
                    set_var(m, define_name, bdone(&define_val), true);
                else
                    free(define_val.p);
                define_val = (Buf){0};
                free(define_name);
                define_name = nullptr;
            } else {
                if (starts_word(t, "define", &rest))
                    define_depth++;
                if (define_val.n)
                    bputc(&define_val, '\n');
                bputs(&define_val, line);
            }
            free(line);
            continue;
        }
        if (line[0] == '\t' && in_rule) {   // 規則のコマンド（覚えるだけ。動かさない）
            if (m->nrules > 0) {
                Rule *r = &m->rules[m->nrules - 1];
                Buf b = {0};
                if (r->recipe) {
                    bputs(&b, r->recipe);
                    bputc(&b, '\n');
                    free(r->recipe);
                }
                bputs(&b, line + 1);
                r->recipe = bdone(&b);
            }
            free(line);
            continue;
        }
        strip_comment(line);
        char *t = line;
        while (is_space(*t))
            t++;
        const char *rest;
        if (starts_word(t, "ifeq", &rest) || starts_word(t, "ifneq", &rest) || starts_word(t, "ifdef", &rest) ||
            starts_word(t, "ifndef", &rest)) {
            char kw[8];
            snprintf(kw, sizeof kw, "%.*s", (int)(rest - t), t);
            if (ncond < MAX_COND) {
                bool yes = active && eval_cond(m, kw, rest);
                cond[ncond++] = (Cond){active, yes, yes};
            }
            in_rule = false;
        } else if (starts_word(t, "else", &rest)) {
            if (ncond > 0) {
                Cond *c = &cond[ncond - 1];
                while (is_space(*rest))
                    rest++;
                const char *r2;
                bool yes;
                if (*rest && (starts_word(rest, "ifeq", &r2) || starts_word(rest, "ifneq", &r2) ||
                              starts_word(rest, "ifdef", &r2) || starts_word(rest, "ifndef", &r2))) {
                    char kw[8];
                    snprintf(kw, sizeof kw, "%.*s", (int)(r2 - rest), rest);
                    yes = c->parent && !c->taken && eval_cond(m, kw, r2);
                } else {
                    yes = c->parent && !c->taken;
                }
                c->active = yes;
                c->taken = c->taken || yes;
            }
        } else if (starts_word(t, "endif", &rest)) {
            if (ncond > 0)
                ncond--;
        } else if (starts_word(t, "define", &rest)) {
            char *raw = expand(m, rest);
            char *n = trim(raw);
            size_t nl = strlen(n);      // define NAME = の形も
            while (nl && (n[nl - 1] == '=' || n[nl - 1] == ':' || is_space(n[nl - 1])))
                n[--nl] = '\0';
            define_name = sdup(n);
            define_depth = 1;     // 生きていない枝の define も endef までは読み飛ばす（値は捨てる）
            free(raw);
        } else if (active) {
            statement(m, t, &in_rule);
        }
        free(line);
    }
    free(define_name);
    free(define_val.p);
}

// ---- 結果を決める ----

// target を目標に持つ規則の前提（複数の規則に分かれていれば、すべて）。なければ nullptr
static char *prereqs_of(Make *m, const char *target, size_t tn)
{
    Buf b = {0};
    bool found = false;
    for (int i = 0; i < m->nrules; i++) {
        const char *p = m->rules[i].targets;
        size_t wn;
        const char *w;
        while ((w = next_word(&p, &wn)))
            if (wn == tn && !memcmp(w, target, tn)) {
                found = true;
                if (b.n)
                    bputc(&b, ' ');
                bputs(&b, m->rules[i].prereqs);
            }
    }
    if (!found)
        return nullptr;
    return bdone(&b);
}

static bool ends_with(const char *w, size_t wn, const char *suf)
{
    size_t n = strlen(suf);
    return wn >= n && !memcmp(w + wn - n, suf, n);
}

// target から前提をたどって、.c か .o を前提に持つ最初の目標（プログラム）。見つかれば name と、その前提。
// .a の目標はライブラリなので、プログラムには選ばない（all: liblua.a lua luac のとき、lua を選ぶ。
// プログラムの前提の .a は collect_sources が開く）
// target が .PHONY の前提にあるか（all・test などの、ファイルを作らない目標）
static bool is_phony(Make *m, const char *target, size_t tn)
{
    char *p = prereqs_of(m, ".PHONY", 6);
    bool found = false;
    const char *s = p;
    size_t wn;
    const char *w;
    while (s && !found && (w = next_word(&s, &wn)))
        found = wn == tn && !memcmp(w, target, tn);
    free(p);
    return found;
}

// target を目標に持つ規則にコマンドの行があるか
static bool has_recipe(Make *m, const char *target, size_t tn)
{
    for (int i = 0; i < m->nrules; i++) {
        const char *p = m->rules[i].targets;
        size_t wn;
        const char *w;
        while ((w = next_word(&p, &wn)))
            if (wn == tn && !memcmp(w, target, tn) && m->rules[i].recipe)
                return true;
    }
    return false;
}

static bool find_program(Make *m, const char *target, size_t tn, int depth, char *name, size_t size, char **pre)
{
    if (depth > 8 || ends_with(target, tn, ".a"))
        return false;
    char *p = prereqs_of(m, target, tn);
    if (!p)
        return false;
    // コマンドの行のない all と、.PHONY の目標は、それ自体はプログラムでない（前提の中を探す。2026-11-20、QuickJS の
    // all: $(OBJDIR) $(OBJDIR)/quickjs.check.o … $(PROGS) で、確かめのための .o を持つ all を選んでいた）
    bool phony = (tn == 3 && !memcmp(target, "all", 3) && !has_recipe(m, target, tn)) || is_phony(m, target, tn);
    const char *s = p;
    size_t wn;
    const char *w;
    while (!phony && (w = next_word(&s, &wn)))
        if (ends_with(w, wn, ".c") || ends_with(w, wn, ".o")) {
            snprintf(name, size, "%.*s", (int)tn, target);
            *pre = p;
            return true;
        }
    s = p;
    while ((w = next_word(&s, &wn)))
        if (find_program(m, w, wn, depth + 1, name, size, pre)) {
            free(p);
            return true;
        }
    free(p);
    return false;
}

static void list_add(char ***list, int *n, const char *s)
{
    char **p = realloc(*list, sizeof(char *) * (size_t)(*n + 1));
    if (!p)
        return;
    *list = p;
    p[(*n)++] = sdup(s);
}

static void add_flags_from(Make *m, MakeInfo *mi, const char *text);

// .o を作る規則（2026-11-20、vim の objects/xdiffi.o: xdiff/xdiffi.c）: その .o を目標に持つ規則の前提の最初の .c、
// なければ合うパターンの規則（objects/%.o: %.c）、サフィックスの規則（.c.o:）。src に .c（Makefile のフォルダから）、
// *recipe にコマンドの行（なければ nullptr）、stem に % の部分。.c が見つからなければ false
static bool object_rule(Make *m, const char *o, size_t on, char *src, size_t ssz, const char **recipe, char *stem,
                        size_t stsz)
{
    *recipe = nullptr;
    src[0] = stem[0] = '\0';
    for (int i = 0; i < m->nrules; i++) {
        const char *p = m->rules[i].targets;
        size_t wn;
        const char *w;
        bool hit = false;
        while ((w = next_word(&p, &wn)) && !hit)
            hit = wn == on && !memcmp(w, o, on);
        if (!hit)
            continue;
        if (m->rules[i].recipe && !*recipe)
            *recipe = m->rules[i].recipe;
        const char *q = m->rules[i].prereqs;
        while (!src[0] && (w = next_word(&q, &wn)))
            if (ends_with(w, wn, ".c"))
                snprintf(src, ssz, "%.*s", (int)wn, w);
    }
    if (src[0])
        return true;
    for (int i = 0; i < m->nrules; i++) {
        const char *p = m->rules[i].targets;
        size_t wn;
        const char *w;
        while ((w = next_word(&p, &wn))) {
            const char *st;
            size_t sn;
            if (!memchr(w, '%', wn) || !pat_match(w, wn, o, on, &st, &sn))
                continue;
            const char *q = m->rules[i].prereqs;
            size_t pn;
            const char *pw = next_word(&q, &pn);
            if (!pw || !memchr(pw, '%', pn))
                continue;
            Buf b = {0};
            pat_put(&b, pw, pn, st, sn);
            char *c = bdone(&b);
            char path[1100];
            struct stat sb;
            snprintf(path, sizeof path, "%s%s%s", c[0] == '/' ? "" : m->dir, c[0] == '/' ? "" : "/", c);
            bool ok = strlen(c) > 2 && !strcmp(c + strlen(c) - 2, ".c") && stat(path, &sb) == 0;
            if (ok) {
                snprintf(src, ssz, "%s", c);
                snprintf(stem, stsz, "%.*s", (int)sn, st);
                if (!*recipe)
                    *recipe = m->rules[i].recipe;
            }
            free(c);
            if (ok)
                return true;
        }
    }
    return false;
}

// サフィックスの規則（.c.o:）のコマンドの行
static const char *suffix_recipe(Make *m)
{
    for (int i = 0; i < m->nrules; i++)
        if (!strcmp(trim(m->rules[i].targets), ".c.o") && m->rules[i].recipe)
            return m->rules[i].recipe;
    return nullptr;
}

// .o を作るコマンドの行から、コンパイラのオプションを取る（$@ $< $^ $* を入れて展開する。-c かその .c を含む行）。
// コマンドの行がなければ、GNU make の組み込みの規則（$(CPPFLAGS) $(CFLAGS)。collect_flags）
static void recipe_flags(Make *m, MakeInfo *mi, const char *recipe, const char *o, size_t on, const char *src,
                         const char *stem)
{
    if (!recipe) {
        m->builtin_flags = true;
        return;
    }
    char saved[256];
    snprintf(saved, sizeof saved, "%s", m->target);
    snprintf(m->target, sizeof m->target, "%.*s", (int)on, o);
    char *all = prereqs_of(m, o, on);
    m->auto_first = src;
    m->auto_all = all ? all : src;
    m->auto_stem = stem;
    char *text = sdup(recipe);
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        while (*line == '@' || *line == '-' || *line == '+' || is_space(*line))
            line++;
        char *e = expand(m, line);
        if (strstr(e, " -c") || strstr(e, src)) {
            add_flags_from(m, mi, e);
            m->recipe_flags = true;
        }
        free(e);
    }
    free(text);
    free(all);
    m->auto_first = m->auto_all = m->auto_stem = nullptr;
    snprintf(m->target, sizeof m->target, "%s", saved);
}

// .c・.o の前提から、コンパイルする .c（絶対パス）。.a の前提は、その .a の目標の前提（.o）を開く
// （lua: lua.o liblua.a、liblua.a: lapi.o …）
static void collect_sources_depth(Make *m, const char *pre, MakeInfo *mi, int depth);

static void collect_sources(Make *m, const char *pre, MakeInfo *mi)
{
    collect_sources_depth(m, pre, mi, 0);
}

static void collect_sources_depth(Make *m, const char *pre, MakeInfo *mi, int depth)
{
    size_t wn;
    const char *w;
    while ((w = next_word(&pre, &wn))) {
        char rel[512], path[1100];
        struct stat st;
        if (ends_with(w, wn, ".a")) {
            char *ap = depth < 8 ? prereqs_of(m, w, wn) : nullptr;
            if (ap) {
                collect_sources_depth(m, ap, mi, depth + 1);
                free(ap);
            }
            continue;
        }
        char stem[256];
        const char *recipe;
        if (ends_with(w, wn, ".c")) {
            snprintf(rel, sizeof rel, "%.*s", (int)wn, w);
            m->builtin_flags = true;    // .c を直接つなぐ（コンパイルのコマンドはプログラムの規則の中）
        } else if (ends_with(w, wn, ".o") && object_rule(m, w, wn, rel, sizeof rel, &recipe, stem, sizeof stem)) {
            if (!recipe)
                recipe = suffix_recipe(m);
            recipe_flags(m, mi, recipe, w, wn, rel, stem);
        } else if (ends_with(w, wn, ".o")) {
            // 同じ名前の .c。なければ、前のフォルダを 1 つずつ外したもの（build/sub/c.o → sub/c.c → c.c）の
            // うち、あるもの。どれもなければ元の名前（コンパイルでエラーになる）
            snprintf(rel, sizeof rel, "%.*s.c", (int)wn - 2, w);
            for (const char *r = rel; r;) {
                snprintf(path, sizeof path, "%s%s%s", r[0] == '/' ? "" : m->dir, r[0] == '/' ? "" : "/", r);
                if (stat(path, &st) == 0) {
                    memmove(rel, r, strlen(r) + 1);
                    break;
                }
                r = rel[0] == '/' ? nullptr : strchr(r, '/');
                if (r)
                    r++;
            }
            const char *base = strrchr(rel, '/');
            base = base ? base + 1 : rel;
            snprintf(stem, sizeof stem, "%.*s", (int)(strlen(base) > 2 ? strlen(base) - 2 : 0), base);
            recipe_flags(m, mi, suffix_recipe(m), w, wn, rel, stem);
        } else {
            continue;
        }
        const char *r = rel;
        while (r[0] == '.' && r[1] == '/')
            r += 2;
        snprintf(path, sizeof path, "%s%s%s", r[0] == '/' ? "" : m->dir, r[0] == '/' ? "" : "/", r);
        bool dup = false;
        for (int i = 0; i < mi->nsrcs && !dup; i++)
            dup = !strcmp(mi->srcs[i], path);
        if (!dup)
            list_add(&mi->srcs, &mi->nsrcs, path);
    }
}

// シェルと同じく単語に分ける（'...'、"..."、\。make はコマンドをシェルに渡すので、-DNAME=\"x\" などはこう読む）
static int shell_words(const char *s, char ***out)
{
    int n = 0;
    *out = nullptr;
    while (*s) {
        while (is_space(*s))
            s++;
        if (!*s)
            break;
        Buf b = {0};
        while (*s && !is_space(*s)) {
            if (*s == '\'') {
                const char *e = strchr(s + 1, '\'');
                if (!e)
                    e = s + strlen(s);
                bput(&b, s + 1, (size_t)(e - s - 1));
                s = *e ? e + 1 : e;
            } else if (*s == '"') {
                s++;
                while (*s && *s != '"') {
                    if (*s == '\\' && (s[1] == '"' || s[1] == '\\' || s[1] == '$'))
                        s++;
                    bputc(&b, *s++);
                }
                if (*s)
                    s++;
            } else if (*s == '\\' && s[1]) {
                bputc(&b, s[1]);
                s += 2;
            } else {
                bputc(&b, *s++);
            }
        }
        list_add(out, &n, bdone(&b));
        free(b.p);
    }
    return n;
}

// 同じオプションは 1 回だけ（.o ごとのコマンドの行から取るので、同じものが何度も出る）
static void add_flag(MakeInfo *mi, const char *a)
{
    for (int i = 0; i < mi->nflags; i++)
        if (!strcmp(mi->flags[i], a))
            return;
    list_add(&mi->flags, &mi->nflags, a);
}

// text（シェルの語）の中の、Monocs のコンパイラが使うオプション
static void add_flags_from(Make *m, MakeInfo *mi, const char *text)
{
    char **w;
    int n = shell_words(text, &w);
    for (int i = 0; i < n; i++) {
        char buf[1100];
        const char *a = w[i];
        // -D NAME のように分かれたもの
        // -include FILE（2 つの語のまま、組で 1 回だけ）
        if (!strcmp(a, "-include") && i + 1 < n) {
            bool dup = false;
            for (int k = 0; k + 1 < mi->nflags && !dup; k++)
                dup = !strcmp(mi->flags[k], "-include") && !strcmp(mi->flags[k + 1], w[i + 1]);
            if (!dup) {
                list_add(&mi->flags, &mi->nflags, a);
                list_add(&mi->flags, &mi->nflags, w[i + 1]);
            }
            i++;
            continue;
        }
        if ((!strcmp(a, "-D") || !strcmp(a, "-U") || !strcmp(a, "-I")) && i + 1 < n) {
            snprintf(buf, sizeof buf, "%s%s", a, w[i + 1]);
            i++;
            a = buf;
        }
        if (!strncmp(a, "-I", 2) && a[2] && a[2] != '/') {
            char tmp[1100];
            snprintf(tmp, sizeof tmp, "-I%s/%s", m->dir, a + 2);
            add_flag(mi, tmp);
        } else if (!strncmp(a, "-std=", 5) || (!strncmp(a, "-D", 2) && a[2]) || (!strncmp(a, "-U", 2) && a[2]) ||
                   (!strncmp(a, "-I", 2) && a[2]) || !strcmp(a, "-fopenmp") ||
                   !strcmp(a, "-fno-schedule-insns2") || !strcmp(a, "-fschedule-insns2") ||
                   !strcmp(a, "-fno-unroll-loops") || !strcmp(a, "-funroll-loops") ||
                   !strcmp(a, "-fno-strict-aliasing") || !strcmp(a, "-fstrict-aliasing")) {
            add_flag(mi, a);
        }
        // ほか（-Wall -O2 -g -MMD など）は Monocs のコンパイラには要らない
    }
    for (int i = 0; i < n; i++)
        free(w[i]);
    free(w);
}

static void collect_flags(Make *m, MakeInfo *mi)
{
    // コマンドの行からオプションを取った .o だけなら、$(CPPFLAGS) $(CFLAGS) は足さない（GNU make と同じく、
    // 組み込みの規則を使う .o・.c があるときだけ）
    if (m->builtin_flags || !m->recipe_flags) {
        char *cflags = expand(m, "$(CPPFLAGS) $(CFLAGS)");
        add_flags_from(m, mi, cflags);
        free(cflags);
    }
    char **w;
    int n;

    char *libs = expand(m, "$(LDFLAGS) $(LDLIBS) $(LIBS)");
    n = shell_words(libs, &w);
    for (int i = 0; i < n; i++) {
        if (strncmp(w[i], "-l", 2) || !w[i][2] || strchr(w[i] + 2, '/'))
            continue;
        char dir[300];
        struct stat st;
        snprintf(dir, sizeof dir, "%s/%s", LIB_DIR, w[i] + 2);
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
            list_add(&mi->flags, &mi->nflags, w[i]);
    }
    for (int i = 0; i < n; i++)
        free(w[i]);
    free(w);
    free(libs);
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return nullptr;
    Buf b = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0)
        bput(&b, tmp, n);
    fclose(fp);
    return bdone(&b);
}

static void free_make(Make *m)
{
    while (m->nvars)
        pop_var(m);
    free(m->vars);
    for (int i = 0; i < m->nrules; i++) {
        free(m->rules[i].targets);
        free(m->rules[i].prereqs);
        free(m->rules[i].recipe);
    }
    free(m->rules);
}

// ---- 読む（make の引数と、コマンドの中の $(MAKE) の呼び直し） ----

#define MAKE_VARS_MAX 32
#define MAKE_DEPTH_MAX 4

typedef struct {
    char *name[MAKE_VARS_MAX], *value[MAKE_VARS_MAX];
    int n;
} MakeVars;

static void vars_set(MakeVars *v, const char *name, size_t nl, const char *value)
{
    for (int i = 0; i < v->n; i++)
        if (strlen(v->name[i]) == nl && !memcmp(v->name[i], name, nl)) {
            free(v->value[i]);
            v->value[i] = sdup(value);
            return;
        }
    if (v->n < MAKE_VARS_MAX) {
        v->name[v->n] = dupn(name, nl);
        v->value[v->n++] = sdup(value);
    }
}

static void vars_copy(MakeVars *dst, const MakeVars *src)
{
    *dst = (MakeVars){0};
    for (int i = 0; i < src->n; i++)
        vars_set(dst, src->name[i], strlen(src->name[i]), src->value[i]);
}

static void vars_free(MakeVars *v)
{
    for (int i = 0; i < v->n; i++) {
        free(v->name[i]);
        free(v->value[i]);
    }
    v->n = 0;
}

// 変数=値 の形か（名前は英数字と _ . -）
static bool is_assignment(const char *w, size_t *nl)
{
    const char *eq = strchr(w, '=');
    if (!eq || eq == w)
        return false;
    for (const char *p = w; p < eq; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '-'))
            return false;
    *nl = (size_t)(eq - w);
    return true;
}

static char *read_makefile(const char *dir)
{
    static const char *const names[] = {"GNUmakefile", "makefile", "Makefile"};
    char *text = nullptr;
    for (int i = 0; i < 3 && !text; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        text = read_file(path);
    }
    return text;
}

// コマンドの行の `…`（バッククォート）を、uname なら答えに、ほかは空に置き換える（コマンドは動かさない）
static char *backticks(const char *line)
{
    Buf b = {0};
    for (const char *p = line; *p;) {
        if (*p == '`') {
            const char *e = strchr(p + 1, '`');
            if (!e)
                break;
            char *c = dupn(p + 1, (size_t)(e - p - 1));
            const char *u = uname_answer(trim(c));
            if (u)
                bputs(&b, u);
            free(c);
            p = e + 1;
        } else {
            bputc(&b, *p++);
        }
    }
    return bdone(&b);
}

static bool read_level(const char *dir, const char *goal_arg, const MakeVars *vars, bool may_fall_back, int depth,
                       MakeInfo *mi);

// goal のコマンドの中の make の呼び直し（make 目標 変数=値、cd DIR && make …、make -C DIR …）を、順に読み直す
static bool follow_recursion(Make *m, const char *goal, const MakeVars *vars, bool may_fall_back, int depth,
                             MakeInfo *mi)
{
    for (int r = 0; r < m->nrules; r++) {
        const char *p = m->rules[r].targets;
        size_t wn;
        const char *w;
        bool hit = false;
        while ((w = next_word(&p, &wn)))
            if (wn == strlen(goal) && !memcmp(w, goal, wn))
                hit = true;
        if (!hit || !m->rules[r].recipe)
            continue;
        snprintf(m->target, sizeof m->target, "%s", goal);
        char *recipe = sdup(m->rules[r].recipe);
        for (char *line = recipe, *next; line; line = next) {
            next = strchr(line, '\n');
            if (next)
                *next++ = '\0';
            while (*line == '@' || *line == '-' || *line == '+' || is_space(*line))
                line++;
            char *ex = expand(m, line);
            char *bt = backticks(ex);
            free(ex);
            char **words;
            int n = shell_words(bt, &words);
            free(bt);
            char sub[600];
            snprintf(sub, sizeof sub, "%s", m->dir);
            for (int i = 0; i < n; i++) {
                if (!strcmp(words[i], "cd") && i + 1 < n) {
                    const char *d = words[++i];
                    snprintf(sub, sizeof sub, "%s%s%s", d[0] == '/' ? "" : m->dir, d[0] == '/' ? "" : "/", d);
                    continue;
                }
                const char *base = strrchr(words[i], '/');
                if (strcmp(base ? base + 1 : words[i], "make") != 0)
                    continue;
                // make の引数（&&・;・|| まで）
                MakeVars nv;
                vars_copy(&nv, vars);
                const char *target = nullptr;
                int k = i + 1;
                for (; k < n; k++) {
                    const char *a = words[k];
                    if (!strcmp(a, "&&") || !strcmp(a, ";") || !strcmp(a, "||"))
                        break;
                    size_t nl;
                    if (!strcmp(a, "-C") && k + 1 < n) {
                        const char *d = words[++k];
                        snprintf(sub, sizeof sub, "%s%s%s", d[0] == '/' ? "" : m->dir, d[0] == '/' ? "" : "/", d);
                    } else if (a[0] == '-') {
                        continue;   // ほかのオプション（-j、-s など）は見ない
                    } else if (is_assignment(a, &nl)) {
                        vars_set(&nv, a, nl, a + nl + 1);
                    } else if (!target) {
                        target = a;
                    }
                }
                bool ok = read_level(sub, target, &nv, may_fall_back, depth + 1, mi);
                vars_free(&nv);
                if (ok) {
                    for (int j = 0; j < n; j++)
                        free(words[j]);
                    free(words);
                    free(recipe);
                    return true;
                }
                i = k;
            }
            for (int j = 0; j < n; j++)
                free(words[j]);
            free(words);
        }
        free(recipe);
    }
    return false;
}

// goal（なければ前提の目標を順に）のコマンドの make の呼び直しをたどる
static bool follow_goal(Make *m, const char *goal, const MakeVars *vars, bool may_fall_back, int depth, int hops,
                        MakeInfo *mi)
{
    if (hops > 8)
        return false;
    if (follow_recursion(m, goal, vars, may_fall_back, depth, mi))
        return true;
    char *pre = prereqs_of(m, goal, strlen(goal));
    if (!pre)
        return false;
    bool ok = false;
    const char *p = pre;
    size_t wn;
    const char *w;
    while (!ok && (w = next_word(&p, &wn))) {
        char t[256];
        snprintf(t, sizeof t, "%.*s", (int)wn, w);
        ok = follow_goal(m, t, vars, may_fall_back, depth, hops + 1, mi);
    }
    free(pre);
    return ok;
}

// dir の Makefile を、目標 goal_arg（NULL なら既定のゴール）と make の引数の変数 vars で読む。プログラムが見つかれば
// mi に入れて true。見つからなければ、目標のコマンドの中の make の呼び直しをたどる（MAKE_DEPTH_MAX 段まで）。
// may_fall_back（make に目標を書いていない）なら、それでも見つからないとき all を試す
static bool read_level(const char *dir, const char *goal_arg, const MakeVars *vars, bool may_fall_back, int depth,
                       MakeInfo *mi)
{
    if (depth > MAKE_DEPTH_MAX)
        return false;
    char *text = read_makefile(dir);
    if (!text)
        return false;
    Make m = {0};
    snprintf(m.dir, sizeof m.dir, "%s", dir);
    size_t dl = strlen(m.dir);
    while (dl > 1 && m.dir[dl - 1] == '/')
        m.dir[--dl] = '\0';
    for (int i = 0; i < vars->n; i++) {
        Var *v = push_var(&m, vars->name[i], sdup(vars->value[i]), false);
        if (v)
            v->cmdline = true;
    }
    parse(&m, text);
    free(text);

    // 目標: make の引数、なければ既定のゴール（.DEFAULT_GOAL、なければ最初の規則の最初の目標）
    char goal[256] = "";
    if (goal_arg) {
        snprintf(goal, sizeof goal, "%s", goal_arg);
    } else {
        Buf g = {0};
        put_var(&m, &g, ".DEFAULT_GOAL", 13);
        char *dg = bdone(&g);
        snprintf(goal, sizeof goal, "%s", trim(dg));
        free(dg);
        for (int i = 0; i < m.nrules && !goal[0]; i++) {
            const char *p = m.rules[i].targets;
            size_t wn;
            const char *w;
            while ((w = next_word(&p, &wn)))
                if (!memchr(w, '%', wn) && (w[0] != '.' || memchr(w, '/', wn))) {
                    snprintf(goal, sizeof goal, "%.*s", (int)wn, w);
                    break;
                }
        }
    }
    char *pre = nullptr;
    bool found = goal[0] && find_program(&m, goal, strlen(goal), 0, mi->target, sizeof mi->target, &pre);
    if (found) {
        collect_sources(&m, pre, mi);
        free(pre);
        collect_flags(&m, mi);
        // 名前はフォルダを除いたもの（build/app → app）
        char *slash = strrchr(mi->target, '/');
        if (slash)
            memmove(mi->target, slash + 1, strlen(slash));
        free_make(&m);
        return true;
    }
    // 目標のコマンドの中の make の呼び直し（Lua の linux-noreadline: → $(MAKE) all SYSCFLAGS=-DLUA_USE_LINUX）。
    // 目標にコマンドがなければ、前提の目標のもの（linux: linux-noreadline、all: $(PLAT)）
    if (goal[0] && follow_goal(&m, goal, vars, may_fall_back, depth, 0, mi)) {
        free_make(&m);
        return true;
    }
    // 見つからない（Lua の guess: → make `uname` は Monocs という目標がない）: 目標を選んでいなければ all を試す
    // （GNU の慣習の目標）
    if (may_fall_back && strcmp(goal, "all") != 0 &&
        find_program(&m, "all", 3, 0, mi->target, sizeof mi->target, &pre)) {
        collect_sources(&m, pre, mi);
        free(pre);
        collect_flags(&m, mi);
        free_make(&m);
        return true;
    }
    if (depth == 0)
        collect_flags(&m, mi);  // プログラムの規則がなくても、オプション（CFLAGS）は使う（フォルダの .c をすべて）
    free_make(&m);
    return false;
}

// フォルダの .monocs-make（シェルの make が書く: make の引数。目標 変数=値 …）を読む
static char *read_make_args(const char *dir)
{
    char path[600];
    snprintf(path, sizeof path, "%s/" MAKE_ARGS_FILE, dir);
    char *t = read_file(path);
    if (t)
        t[strcspn(t, "\n")] = '\0';
    return t;
}

bool make_read(const char *dir, MakeInfo *mi)
{
    *mi = (MakeInfo){0};
    char *text = read_makefile(dir);
    if (!text)
        return false;
    free(text);
    // make の引数（.monocs-make）: 目標と 変数=値
    MakeVars vars = {0};
    char *goal = nullptr;
    char *args = read_make_args(dir);
    if (args) {
        char **words;
        int n = shell_words(args, &words);
        for (int i = 0; i < n; i++) {
            size_t nl;
            if (is_assignment(words[i], &nl))
                vars_set(&vars, words[i], nl, words[i] + nl + 1);
            else if (!goal && words[i][0] != '-')
                goal = sdup(words[i]);
            free(words[i]);
        }
        free(words);
        free(args);
    }
    read_level(dir, goal, &vars, goal == nullptr, 0, mi);
    free(goal);
    vars_free(&vars);
    return true;
}

void make_free(MakeInfo *mi)
{
    for (int i = 0; i < mi->nflags; i++)
        free(mi->flags[i]);
    for (int i = 0; i < mi->nsrcs; i++)
        free(mi->srcs[i]);
    free(mi->flags);
    free(mi->srcs);
    *mi = (MakeInfo){0};
}

bool make_program_name(const char *dir, char *name, size_t size)
{
    MakeInfo mi;
    if (!make_read(dir, &mi))
        return false;
    bool ok = mi.target[0] != '\0';
    snprintf(name, size, "%s", mi.target);
    make_free(&mi);
    return ok;
}
