// glob・globfree（POSIX.1-2008 と GNU の拡張の一部。2026-11-14）。picolibc には本体がないので書いた。
// 動きは glibc に合わせる（ヘッダーは os/api/include/glob.h。構造体・定数は glibc と同じ）:
//   - パターンを / で区切り、* ? [ を含む部分はディレクトリを読んで fnmatch で比べ、含まない部分はそのまま
//     つなぐ（最後の部分はあるかを確かめる）。結果は strcmp の順（C のロケールの strcoll と同じ）
//   - 先頭の . は、パターンの側も . で始まるときだけ合う（GLOB_PERIOD で外す）。readdir は . と .. も返すので、
//     ".*" は . と .. にも合う（glibc・bash と同じ）
//   - GLOB_ERR・errfunc（読めないディレクトリ。ないもの（ENOENT・ENOTDIR）は呼ばない）、GLOB_MARK、GLOB_NOSORT、
//     GLOB_DOOFFS、GLOB_NOCHECK、GLOB_APPEND、GLOB_NOESCAPE、GLOB_PERIOD、GLOB_MAGCHAR、GLOB_BRACE、
//     GLOB_NOMAGIC、GLOB_TILDE（~ と ~/... を HOME に。~名前 はユーザーがないので、そのまま）、GLOB_TILDE_CHECK、
//     GLOB_ONLYDIR。GLOB_ALTDIRFUNC は使わない（いつもふつうのファイルの関数）
// 出典: POSIX.1-2008 glob()、glibc の手引き 10.2（Calling glob、Flags for Globbing、More Flags for Globbing）

#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../api/include/glob.h"

typedef struct {
    char **v;
    size_t n, cap;
} List;

typedef struct {
    int flags;
    int (*errfunc)(const char *, int);
    List *out;
    bool aborted, nomem;
} Ctx;

static bool list_add(Ctx *c, char *s)
{
    if (!s) {
        c->nomem = true;
        return false;
    }
    List *l = c->out;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        char **v = realloc(l->v, sizeof(char *) * cap);
        if (!v) {
            free(s);
            c->nomem = true;
            return false;
        }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = s;
    return true;
}

// p[0..n) に * ? [ が（\ で消されずに）あるか
static bool has_magic(const char *p, size_t n, bool noescape)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\\' && !noescape && i + 1 < n) {
            i++;
            continue;
        }
        if (p[i] == '*' || p[i] == '?' || p[i] == '[')
            return true;
    }
    return false;
}

// \ を取ってつなぐ
static void put_unescaped(char *dst, const char *p, size_t n, bool noescape)
{
    size_t k = strlen(dst);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\\' && !noescape && i + 1 < n)
            i++;
        dst[k++] = p[i];
    }
    dst[k] = '\0';
}

static bool exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// 見つかったパス。GLOB_MARK ならディレクトリの後ろに / を足す（glibc と同じく、/ で終わっていても足す）。
// only_dir（GLOB_ONLYDIR で、* などを含む最後の部分）ならディレクトリだけ
static void found(Ctx *c, const char *path, bool only_dir)
{
    bool mark = c->flags & GLOB_MARK;
    bool dir = (mark || only_dir) && is_dir(path);
    if (only_dir && !dir)
        return;
    size_t n = strlen(path);
    char *s = malloc(n + 2);
    if (!s) {
        c->nomem = true;
        return;
    }
    memcpy(s, path, n + 1);
    if (mark && dir) {
        s[n] = '/';
        s[n + 1] = '\0';
    }
    list_add(c, s);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// path（ここまでの部分。"" は今のフォルダ）に、パターンの残り rest（部分の始まり）を合わせる。
// 部分の間の / はパターンのとおりに残す（sub//x/* は sub//x/a。glibc と同じ）。パターンが / で終わるとき
// （*/）は、最後の部分もディレクトリだけで、結果も / で終わる
static void expand(Ctx *c, char *path, size_t cap, const char *rest)
{
    if (c->aborted || c->nomem)
        return;
    const char *end = strchr(rest, '/');
    size_t n = end ? (size_t)(end - rest) : strlen(rest);
    size_t seplen = end ? strspn(end, "/") : 0;
    const char *after = end ? end + seplen : rest + n;
    bool last = !*after, need_dir = !last || seplen > 0;
    bool noescape = c->flags & GLOB_NOESCAPE;
    size_t plen = strlen(path);

    if (!has_magic(rest, n, noescape)) {
        if (plen + n + seplen + 2 > cap)
            return;
        put_unescaped(path, rest, n, noescape);
        strncat(path, end ? end : "", seplen);
        if (!last)
            expand(c, path, cap, after);
        else if (need_dir ? is_dir(path) : exists(path))
            found(c, path, false);
        path[plen] = '\0';
        return;
    }

    char pat[1024];
    if (n >= sizeof pat)
        return;
    memcpy(pat, rest, n);
    pat[n] = '\0';
    const char *dir = plen ? path : ".";
    DIR *d = opendir(dir);
    if (!d) {
        if (errno != ENOENT && errno != ENOTDIR) {
            if ((c->errfunc && c->errfunc(dir, errno)) || (c->flags & GLOB_ERR))
                c->aborted = true;
        }
        return;
    }
    int fl = (noescape ? FNM_NOESCAPE : 0) | ((c->flags & GLOB_PERIOD) ? 0 : FNM_PERIOD);
    // ディレクトリを読む間にほかのフォルダを開かないよう、先に名前を集める（同時に開けるファイルの数が少ない）
    List names = {0};
    Ctx nc = *c;
    nc.out = &names;
    struct dirent *e;
    while ((e = readdir(d))) {
        // ディレクトリの部分では、. と .. に合わせない（GLOB_PERIOD でも。glibc と同じ）
        if (need_dir && pat[0] != '.' && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")))
            continue;
        if (fnmatch(pat, e->d_name, fl) != 0)
            continue;
        if (need_dir && e->d_type != DT_DIR && e->d_type != DT_UNKNOWN)
            continue;
        if (!list_add(&nc, strdup(e->d_name)))
            break;
    }
    closedir(d);
    c->nomem = nc.nomem;
    for (size_t i = 0; i < names.n; i++) {
        size_t m = strlen(names.v[i]);
        if (!c->aborted && !c->nomem && plen + m + seplen + 2 <= cap) {
            memcpy(path + plen, names.v[i], m + 1);
            strncat(path, end ? end : "", seplen);
            if (!last)
                expand(c, path, cap, after);
            else if (!need_dir || is_dir(path))
                found(c, path, c->flags & GLOB_ONLYDIR);
            path[plen] = '\0';
        }
        free(names.v[i]);
    }
    free(names.v);
}

// GLOB_BRACE: 最初の {…,…} を 1 つずつ展開して、それぞれを glob する。展開するものがなければ false
static bool find_brace(const char *p, bool noescape, const char **open, const char **close)
{
    for (const char *s = p; *s; s++) {
        if (*s == '\\' && !noescape && s[1]) {
            s++;
            continue;
        }
        if (*s != '{')
            continue;
        int depth = 0;
        for (const char *t = s; *t; t++) {
            if (*t == '\\' && !noescape && t[1]) {
                t++;
                continue;
            }
            if (*t == '{')
                depth++;
            else if (*t == '}' && --depth == 0) {
                *open = s;
                *close = t;
                return true;
            }
        }
        return false;
    }
    return false;
}

static int glob_one(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g);

static int glob_brace(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g, bool *done)
{
    const char *open, *close;
    *done = false;
    if (!(flags & GLOB_BRACE) || !find_brace(pattern, flags & GLOB_NOESCAPE, &open, &close))
        return 0;
    *done = true;
    size_t first = g->gl_pathc;
    size_t pre = (size_t)(open - pattern), len = strlen(pattern);
    char *alt = malloc(len + 1);
    if (!alt)
        return GLOB_NOSPACE;
    const char *s = open + 1;
    int depth = 0, mag = 0;     // GLOB_MAGCHAR: 展開したもののどれかにあれば
    for (const char *t = s;; t++) {
        if (*t == '\\' && !(flags & GLOB_NOESCAPE) && t[1]) {
            t++;
            continue;
        }
        if (*t == '{')
            depth++;
        else if (*t == '}' && depth > 0)
            depth--;
        else if ((*t == ',' && depth == 0) || t == close) {
            // 前 + この選びの中身 + 後
            memcpy(alt, pattern, pre);
            memcpy(alt + pre, s, (size_t)(t - s));
            strcpy(alt + pre + (t - s), close + 1);
            int r = glob(alt, flags | GLOB_APPEND, errfunc, g);
            if (r == 0)
                mag |= g->gl_flags & GLOB_MAGCHAR;
            if (r != 0 && r != GLOB_NOMATCH) {
                free(alt);
                return r;
            }
            s = t + 1;
            if (t == close)
                break;
        }
    }
    free(alt);
    g->gl_flags = flags | mag;
    return g->gl_pathc != first ? 0 : GLOB_NOMATCH;
}

int glob(const char *restrict pattern, int flags, int (*errfunc)(const char *, int), glob_t *restrict g)
{
    if (!pattern || !g) {
        errno = EINVAL;
        return GLOB_NOSPACE;
    }
    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = 0;
        g->gl_pathv = nullptr;
        if (!(flags & GLOB_DOOFFS))
            g->gl_offs = 0;
    }
    bool done;
    int r = glob_brace(pattern, flags, errfunc, g, &done);
    if (done)
        return r;   // gl_flags は glob_brace が決める
    return glob_one(pattern, flags, errfunc, g);
}

static int glob_one(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g)
{
    // ~ と ~/...（GLOB_TILDE）
    char pat_buf[4096];
    const char *pat = pattern;
    if ((flags & (GLOB_TILDE | GLOB_TILDE_CHECK)) && pattern[0] == '~') {
        if (pattern[1] == '/' || !pattern[1]) {
            const char *home = getenv("HOME");
            if (home) {
                snprintf(pat_buf, sizeof pat_buf, "%s%s", home, pattern + 1);
                pat = pat_buf;
            } else if (flags & GLOB_TILDE_CHECK) {
                return GLOB_NOMATCH;
            }
        } else if (flags & GLOB_TILDE_CHECK) {
            return GLOB_NOMATCH;    // ~名前: Monocs にユーザーの一覧はない
        }
    }
    // \/ はただの /（glibc と同じ）
    bool noescape = flags & GLOB_NOESCAPE;
    if (!noescape && strstr(pat, "\\/")) {
        char tmp[4096];
        size_t k = 0;
        for (const char *p = pat; *p && k + 1 < sizeof tmp; p++) {
            if (p[0] == '\\' && p[1] == '/')
                continue;
            if (p[0] == '\\' && p[1]) {   // ほかの \x はそのまま（fnmatch が読む）
                tmp[k++] = *p++;
            }
            tmp[k++] = *p;
        }
        tmp[k] = '\0';
        memcpy(pat_buf, tmp, k + 1);
        pat = pat_buf;
    }
    // GLOB_MAGCHAR・GLOB_NOMAGIC の「特別な文字」は、* ? [ と（GLOB_NOESCAPE がなければ）\（glibc と同じ）
    bool magic = has_magic(pat, strlen(pat), noescape) || (!noescape && strchr(pat, '\\'));
    const char *lastpart = strrchr(pat, '/');
    lastpart = lastpart ? lastpart + 1 : pat;
    bool magchar = has_magic(lastpart, strlen(lastpart), noescape) || (!noescape && strchr(lastpart, '\\'));

    List out = {0};
    Ctx c = {flags, errfunc, &out, false, false};
    char path[4096] = "";
    size_t root = strspn(pat, "/");     // 先頭の / はそのまま
    memcpy(path, pat, root);
    path[root] = '\0';
    expand(&c, path, sizeof path, pat + root);
    if (c.nomem || c.aborted) {
        for (size_t i = 0; i < out.n; i++)
            free(out.v[i]);
        free(out.v);
        g->gl_flags = flags;
        return c.nomem ? GLOB_NOSPACE : GLOB_ABORTED;
    }
    if (out.n == 0) {
        if (!(flags & GLOB_NOCHECK) && !((flags & GLOB_NOMAGIC) && !magic)) {
            free(out.v);
            g->gl_flags = flags;
            return GLOB_NOMATCH;
        }
        if (!list_add(&c, strdup(pattern))) {
            free(out.v);
            return GLOB_NOSPACE;
        }
    } else if (!(flags & GLOB_NOSORT)) {
        qsort(out.v, out.n, sizeof(char *), cmp_str);
    }

    // gl_pathv: 先頭に gl_offs 個の NULL（GLOB_DOOFFS）、前の結果（GLOB_APPEND）、今回のもの、NULL
    size_t offs = (flags & GLOB_DOOFFS) ? g->gl_offs : 0;
    size_t old = (flags & GLOB_APPEND) && g->gl_pathv ? g->gl_pathc : 0;
    char **v = realloc((flags & GLOB_APPEND) ? g->gl_pathv : nullptr, sizeof(char *) * (offs + old + out.n + 1));
    if (!v) {
        for (size_t i = 0; i < out.n; i++)
            free(out.v[i]);
        free(out.v);
        return GLOB_NOSPACE;
    }
    if (!old)
        for (size_t i = 0; i < offs; i++)
            v[i] = nullptr;
    memcpy(v + offs + old, out.v, sizeof(char *) * out.n);
    v[offs + old + out.n] = nullptr;
    free(out.v);
    g->gl_pathv = v;
    g->gl_pathc = old + out.n;
    g->gl_flags = flags | (magchar ? GLOB_MAGCHAR : 0);
    return 0;
}

void globfree(glob_t *g)
{
    if (!g || !g->gl_pathv)
        return;
    size_t offs = (g->gl_flags & GLOB_DOOFFS) ? g->gl_offs : 0;
    for (size_t i = 0; i < g->gl_pathc; i++)
        free(g->gl_pathv[offs + i]);
    free(g->gl_pathv);
    g->gl_pathv = nullptr;
    g->gl_pathc = 0;
}
