// ls [-a] [-l] [-h] [-1] [PATH...]: ファイルの一覧（GNU の ls に近いもの）。Monocs に付いてくるアプリ（/bin）。
// 名前の順に、端末の幅に合わせて列に並べる（上から下へ）。ディレクトリは青（GNU の ls --color と同じ）。
// 出力がファイル・パイプなら、色を付けず 1 行に 1 つ（GNU と同じ）。
//   -a: 「.」で始まるものも、-l: 種類・大きさ・日時、-h: 大きさを 1.2K などで、-1: 1 行に 1 つ
// wcwidth（XSI）の宣言のため
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <monocs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

// UTF-8 の文字列の表示幅（全角は 2）
static int str_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int l = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
        unsigned cp = l == 1 ? *p : (unsigned)(*p & (0x3F >> (l - 1)));
        for (int k = 1; k < l && p[k]; k++)
            cp = cp << 6 | (p[k] & 0x3F);
        int cw = wcwidth((wchar_t)cp);
        w += cw < 0 ? 1 : cw;
        while (l-- > 0 && *p)
            p++;
    }
    return w;
}

#define LS_COLOR_DIR    "\x1b[1;34m"
#define LS_COLOR_END    "\x1b[0m"

typedef struct {
    char *name;
    bool dir;
    long size;
    time_t mtime;
} LsEntry;

static int ls_cmp(const void *a, const void *b)
{
    const LsEntry *x = a, *y = b;
    int r = strcasecmp(x->name, y->name);
    return r ? r : strcmp(x->name, y->name);
}

static void ls_size(char *buf, size_t n, long size, bool human)
{
    if (!human || size < 1024) {
        snprintf(buf, n, "%ld", size);
        return;
    }
    static const char units[] = "KMGT";
    double v = (double)size;
    int u = -1;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        u++;
    }
    if (v < 10)
        snprintf(buf, n, "%.1f%c", (double)((long)(v * 10 + 0.999)) / 10, units[u]);   // 切り上げ（GNU と同じ）
    else
        snprintf(buf, n, "%ld%c", (long)(v + 0.999), units[u]);
}

static void ls_print_name(const LsEntry *e)
{
    if (e->dir && isatty(1))   // ファイルに書くときは色を付けない（GNU と同じ）
        printf(LS_COLOR_DIR "%s" LS_COLOR_END, e->name);
    else
        printf("%s", e->name);
}

static void ls_print(LsEntry *ents, int n, bool longf, bool human, bool one, bool is_dir)
{
    qsort(ents, (size_t)n, sizeof(LsEntry), ls_cmp);
    if (longf) {
        long total = 0;
        int sw = 1;
        char buf[32];
        for (int i = 0; i < n; i++) {
            total += (ents[i].size + 1023) / 1024;
            ls_size(buf, sizeof(buf), ents[i].size, human);
            if ((int)strlen(buf) > sw)
                sw = (int)strlen(buf);
        }
        if (is_dir)
            printf("total %ld\n", total);  // ディレクトリの中を出すときだけ（GNU と同じ）
        time_t now = time(nullptr);
        for (int i = 0; i < n; i++) {
            ls_size(buf, sizeof(buf), ents[i].size, human);
            // 日時: 半年以内なら「月 日 時:分」、それより前・先なら「月 日  年」（GNU と同じ）
            char when[32] = "            ";
            if (ents[i].mtime) {
                struct tm tm;
                localtime_r(&ents[i].mtime, &tm);
                bool recent = ents[i].mtime > now - 182 * 86400L && ents[i].mtime <= now + 3600;
                strftime(when, sizeof(when), recent ? "%b %e %H:%M" : "%b %e  %Y", &tm);
            }
            printf("%s %*s %s ", ents[i].dir ? "drwxr-xr-x" : "-rw-r--r--", sw, buf, when);
            ls_print_name(&ents[i]);
            printf("\n");
        }
        return;
    }
    if (one || !isatty(1)) {     // ファイルに書くときは 1 行に 1 つ（GNU と同じ）
        for (int i = 0; i < n; i++) {
            ls_print_name(&ents[i]);
            printf("\n");
        }
        return;
    }
    // 列に並べる（上から下へ。GNU の ls -C と同じく、列の幅はいちばん長い名前 + 2）
    int width = monocs_term_cols();
    int w = 1;
    for (int i = 0; i < n; i++) {
        int l = str_width(ents[i].name) + 2;
        if (l > w)
            w = l;
    }
    int cols = width / w > 0 ? width / w : 1;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r;
            if (i >= n)
                break;
            ls_print_name(&ents[i]);
            if ((c + 1) * rows + r < n)
                for (int k = str_width(ents[i].name); k < w; k++)
                    putchar(' ');
        }
        printf("\n");
    }
}

static void ls_free(LsEntry *ents, int n)
{
    for (int i = 0; i < n; i++)
        free(ents[i].name);
    free(ents);
}

static bool ls_add(LsEntry **ents, int *n, int *cap, const char *name, bool dir, long size, time_t mtime)
{
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 64;
        LsEntry *p = realloc(*ents, sizeof(LsEntry) * (size_t)c);
        if (!p)
            return false;
        *ents = p;
        *cap = c;
    }
    char *s = strdup(name);
    if (!s)
        return false;
    (*ents)[(*n)++] = (LsEntry){s, dir, size, mtime};
    return true;
}

static int ls_dir(const char *dir, bool all, bool longf, bool human, bool one)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "ls: cannot open directory '%s': %s\n", dir, strerror(errno));
        return 1;
    }
    LsEntry *ents = nullptr;
    int n = 0, cap = 0;
    if (all) {
        ls_add(&ents, &n, &cap, ".", true, 0, 0);
        ls_add(&ents, &n, &cap, "..", true, 0, 0);
    }
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;   // 上で足した（-l でも大きさ・日時を出さない）
        if ((e->d_name[0] == '.' || monocs_dirent_hidden(d)) && !all)
            continue;   // 「.」で始まるものと、FAT の隠しファイル（System Volume Information など）
        long size = 0;
        time_t mtime = 0;
        bool isdir = e->d_type == DT_DIR;
        if (longf) {
            char path[1024 + 64];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (stat(path, &st) == 0) {
                size = isdir ? 0 : (long)st.st_size;
                mtime = st.st_mtime;
            }
        }
        ls_add(&ents, &n, &cap, e->d_name, isdir, size, mtime);
    }
    closedir(d);
    ls_print(ents, n, longf, human, one, true);
    ls_free(ents, n);
    return 0;
}

int main(int argc, char **argv)
{
    bool all = false, longf = false, human = false, one = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *o = argv[first] + 1; *o; o++) {
            switch (*o) {
            case 'a': all = true; break;
            case 'l': longf = true; break;
            case 'h': human = true; break;
            case '1': one = true; break;
            default:
                fprintf(stderr, "ls: invalid option -- '%c'\nusage: ls [-a] [-l] [-h] [-1] [PATH...]\n", *o);
                return 1;
            }
        }
    }
    if (first == argc)
        return ls_dir(".", all, longf, human, one);
    // ファイルを先にまとめて、続けてディレクトリごとに（GNU と同じ）
    LsEntry *files = nullptr;
    int nf = 0, cap = 0, ndirs = 0, status = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "ls: cannot access '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        } else if (S_ISDIR(st.st_mode)) {
            ndirs++;
        } else {
            ls_add(&files, &nf, &cap, argv[i], false, (long)st.st_size, st.st_mtime);
        }
    }
    if (nf)
        ls_print(files, nf, longf, human, one, false);
    ls_free(files, nf);
    bool header = argc - first > 1;
    bool printed = nf > 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (header)
            printf("%s%s:\n", printed ? "\n" : "", argv[i]);
        printed = true;
        status |= ls_dir(argv[i], all, longf, human, one);
    }
    return status;
}

