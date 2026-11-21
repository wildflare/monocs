// grep [-ivnclrFE] PATTERN [FILE...]: パターンに合う行を出す。ファイルがなければ stdin（パイプ・<）。
// Monocs に付いてくるアプリ（/bin）（grep で動く）。画面に出すときは、合ったところを色付けする（GNU の --color=auto）。
//
//   -i 大文字・小文字を区別しない  -v 合わない行  -n 行番号  -c 数だけ  -l ファイル名だけ
//   -r ディレクトリの中も（ファイルがなければ .）  -F パターンをそのままの文字列として  -E 拡張の書き方
//
// パターン（小さな正規表現。GNU grep の一部）: 文字、. （どれでも 1 文字）、[abc]・[a-z]・[^...]、^（行頭）、
// $（行末）、* （直前を 0 回以上）、\+（1 回以上）、\?（0 か 1 回）、\. などの \ は次の文字そのもの。
// -E では + と ? を \ なしで書く。グループ ( ) と選択 | は扱わない。
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define COLOR_MATCH "\x1b[01;31m"
#define COLOR_FILE  "\x1b[35m"
#define COLOR_LINE  "\x1b[32m"
#define COLOR_SEP   "\x1b[36m"
#define COLOR_END   "\x1b[m"

enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL };
enum { Q_ONE, Q_STAR, Q_PLUS, Q_OPT };

typedef struct {
    int type, quant;
    unsigned char c;
    unsigned char cls[32];  // N_CLASS: 合う文字のビット表
} Node;

static Node re[256];
static int nre;
static bool opt_i, opt_v, opt_n, opt_c, opt_l, opt_r, opt_F, opt_E, color, show_name;
static const char *pattern;

static bool cls_has(const Node *n, unsigned char c)
{
    return n->cls[c >> 3] & (1 << (c & 7));
}

static void cls_add(Node *n, unsigned char c)
{
    n->cls[c >> 3] |= (unsigned char)(1 << (c & 7));
    if (opt_i && isalpha(c)) {
        unsigned char o = (unsigned char)(islower(c) ? toupper(c) : tolower(c));
        n->cls[o >> 3] |= (unsigned char)(1 << (o & 7));
    }
}

static bool compile(const char *p)
{
    nre = 0;
    if (opt_F) {
        for (; *p && nre < 256; p++)
            re[nre++] = (Node){N_CHAR, Q_ONE, (unsigned char)*p, {0}};
        return true;
    }
    for (; *p; p++) {
        if (nre == 256)
            return false;
        Node *n = &re[nre];
        // 量の記号（直前の要素に付ける）
        int q = -1;
        if (*p == '*')
            q = Q_STAR;
        else if (opt_E && *p == '+')
            q = Q_PLUS;
        else if (opt_E && *p == '?')
            q = Q_OPT;
        else if (!opt_E && *p == '\\' && p[1] == '+')
            q = Q_PLUS, p++;
        else if (!opt_E && *p == '\\' && p[1] == '?')
            q = Q_OPT, p++;
        if (q >= 0 && nre > 0 && re[nre - 1].quant == Q_ONE && re[nre - 1].type != N_BOL) {
            re[nre - 1].quant = q;
            continue;
        }
        *n = (Node){N_CHAR, Q_ONE, 0, {0}};
        if (*p == '^' && nre == 0) {
            n->type = N_BOL;
        } else if (*p == '$' && p[1] == '\0') {
            n->type = N_EOL;
        } else if (*p == '.') {
            n->type = N_ANY;
        } else if (*p == '[') {
            n->type = N_CLASS;
            bool neg = *++p == '^';
            if (neg)
                p++;
            const char *start = p;
            for (; *p && (*p != ']' || p == start); p++) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    for (int c = (unsigned char)p[0]; c <= (unsigned char)p[2]; c++)
                        cls_add(n, (unsigned char)c);
                    p += 2;
                } else {
                    cls_add(n, (unsigned char)*p);
                }
            }
            if (*p != ']')
                return false;   // ] がない
            if (neg)
                for (int i = 0; i < 32; i++)
                    n->cls[i] = (unsigned char)~n->cls[i];
        } else if (*p == '\\' && p[1]) {
            n->c = (unsigned char)*++p;
        } else {
            n->c = (unsigned char)*p;
        }
        nre++;
    }
    return true;
}

static bool node_match(const Node *n, unsigned char c)
{
    switch (n->type) {
    case N_ANY:
        return c != '\0';
    case N_CLASS:
        return c != '\0' && cls_has(n, c);
    case N_CHAR:
        return c != '\0' && (opt_i ? tolower(c) == tolower(n->c) : c == n->c);
    }
    return false;
}

// text の位置 t から、re[i..] が合うか。合えば *end に終わりの位置
static bool match_here(int i, const char *line, const char *t, const char **end)
{
    if (i == nre) {
        *end = t;
        return true;
    }
    const Node *n = &re[i];
    if (n->type == N_BOL)
        return t == line && match_here(i + 1, line, t, end);
    if (n->type == N_EOL)
        return *t == '\0' && match_here(i + 1, line, t, end);
    if (n->quant == Q_ONE)
        return node_match(n, (unsigned char)*t) && match_here(i + 1, line, t + 1, end);
    // 繰り返し: できるだけ多く合わせてから、少しずつ戻す
    int min = n->quant == Q_PLUS ? 1 : 0;
    int max = n->quant == Q_OPT ? 1 : 1 << 30;
    int k = 0;
    while (k < max && node_match(n, (unsigned char)t[k]))
        k++;
    for (; k >= min; k--)
        if (match_here(i + 1, line, t + k, end))
            return true;
    return false;
}

// line の from から後ろで最初に合うところ。なければ false
static bool search(const char *line, const char *from, const char **ms, const char **me)
{
    for (const char *t = from;; t++) {
        if (match_here(0, line, t, me)) {
            *ms = t;
            return true;
        }
        if (!*t || (nre > 0 && re[0].type == N_BOL))
            return false;   // 行の終わり、または行頭に限るパターン
    }
}

static void print_prefix(const char *name, long lineno)
{
    if (show_name)
        printf(color ? COLOR_FILE "%s" COLOR_END COLOR_SEP ":" COLOR_END : "%s:", name);
    if (opt_n)
        printf(color ? COLOR_LINE "%ld" COLOR_END COLOR_SEP ":" COLOR_END : "%ld:", lineno);
}

static void print_line(const char *line)
{
    if (!color || opt_v) {
        printf("%s\n", line);
        return;
    }
    const char *p = line, *ms, *me;
    while (*p && search(line, p, &ms, &me) && me > ms) {
        fwrite(p, 1, (size_t)(ms - p), stdout);
        printf(COLOR_MATCH);
        fwrite(ms, 1, (size_t)(me - ms), stdout);
        printf(COLOR_END);
        p = me;
    }
    printf("%s\n", p);
}

// 1 つのファイル（f）を調べる。合う行があれば true
static bool grep_file(FILE *f, const char *name)
{
    char *line = nullptr;
    size_t cap = 0;
    ssize_t len;
    long lineno = 0, count = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';
        if (memchr(line, '\0', (size_t)len)) {
            // NUL を含む: バイナリのファイル（GNU と同じく、合ったことだけを知らせる）
            const char *ms, *me;
            bool hit = search(line, line, &ms, &me) != opt_v;
            if (hit && !opt_c && !opt_l) {
                printf("Binary file %s matches\n", name);
                free(line);
                return true;
            }
            count += hit;
            continue;
        }
        const char *ms, *me;
        if (search(line, line, &ms, &me) == opt_v)
            continue;
        count++;
        if (opt_l)
            break;
        if (!opt_c) {
            print_prefix(name, lineno);
            print_line(line);
        }
    }
    free(line);
    if (opt_c) {
        if (show_name)
            printf(color ? COLOR_FILE "%s" COLOR_END COLOR_SEP ":" COLOR_END : "%s:", name);
        printf("%ld\n", count);
    }
    if (opt_l && count)
        printf(color ? COLOR_FILE "%s" COLOR_END "\n" : "%s\n", name);
    return count > 0;
}

static int grep_path(const char *path, bool *any)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "grep: %s: No such file or directory\n", path);
        return 2;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!opt_r) {
            fprintf(stderr, "grep: %s: Is a directory\n", path);
            return 0;
        }
        DIR *d = opendir(path);
        if (!d)
            return 2;
        int status = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char sub[1024];
            snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
            int r = grep_path(sub, any);
            if (r > status)
                status = r;
        }
        closedir(d);
        return status;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "grep: %s: cannot open\n", path);
        return 2;
    }
    if (grep_file(f, path))
        *any = true;
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--"); i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'i': opt_i = true; break;
            case 'v': opt_v = true; break;
            case 'n': opt_n = true; break;
            case 'c': opt_c = true; break;
            case 'l': opt_l = true; break;
            case 'r': case 'R': opt_r = true; break;
            case 'F': opt_F = true; break;
            case 'E': opt_E = true; break;
            default:
                fprintf(stderr, "grep: invalid option -- '%c'\nusage: grep [-ivnclrFE] PATTERN [FILE...]\n", *o);
                return 2;
            }
        }
    }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;    // オプションの終わり（GNU と同じ。続くものは、- で始まっても名前）
    if (i == argc) {
        fprintf(stderr, "usage: grep [-ivnclrFE] PATTERN [FILE...]\n");
        return 2;
    }
    pattern = argv[i++];
    if (!compile(pattern)) {
        fprintf(stderr, "grep: invalid pattern '%s'\n", pattern);
        return 2;
    }
    color = isatty(1);
    bool any = false;
    int status = 0;
    if (i == argc && !opt_r) {
        if (grep_file(stdin, "(standard input)"))
            any = true;
    } else {
        show_name = opt_r || argc - i > 1;
        if (i == argc)
            status = grep_path(".", &any);
        for (; i < argc; i++) {
            int r = grep_path(argv[i], &any);
            if (r > status)
                status = r;
        }
    }
    return status ? status : any ? 0 : 1;
}
