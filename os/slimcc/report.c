// cc --report（2026-11-26。docs/sandbox.md の 6 の 2）: プログラムが使う OS の関数を種類ごとに並べる。
// 人が作ったコードを動かす前に、何をするプログラムかを見るため（Android のアプリの「この機能を使います」に近い）。
// 使う関数は、つなぐとき（run.c の resolve）に OS の関数表から引いた名前でわかる（-l のライブラリの分も入る）。
// あわせて、渡されたソースのファイル（ヘッダーは見ない）から、インラインアセンブリと周辺機器の番地（0x3F000000〜
// 0x40FFFFFF）の直書きを探す。限界: ポインタで OS のメモリを直接いじるなど、一覧に出ない手もある（悪意は防げない）。
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "report.h"

#define MAX_NAMES 2048
static char *names[MAX_NAMES];
static int nnames;
static bool recording;

void report_begin(void)
{
    for (int i = 0; i < nnames; i++)
        free(names[i]);
    nnames = 0;
    recording = true;
}

void report_note(const char *name)
{
    if (!recording)
        return;
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i], name))
            return;
    size_t n = strlen(name) + 1;    // strdup はカーネルの規格（ISO C23）では宣言されない
    if (nnames < MAX_NAMES && (names[nnames] = malloc(n))) {
        memcpy(names[nnames], name, n);
        nnames++;
    }
}

// 種類: 名前が完全に一致するか、終わりが * なら前の部分が一致するもの
struct kind {
    const char *title;
    const char *const *names;
};
static const char *const k_files[] = {
    "fopen", "freopen", "open", "openat", "creat", "opendir", "fdopendir", "stat", "lstat", "access", "realpath",
    "glob", "chdir", "getcwd", "tmpfile", "tmpnam", "mkstemp", "mmap", nullptr };
static const char *const k_change[] = {
    "remove", "unlink", "rename", "mkdir", "rmdir", "truncate", "ftruncate", "utime", "utimes", "chmod",
    "fchmod", "chown", "link", "symlink", "monocs_fat_attr", nullptr };
static const char *const k_net[] = {
    "socket", "connect", "bind", "listen", "accept", "accept4", "send", "sendto", "recv", "recvfrom",
    "getaddrinfo", "gethostbyname", "monocs_tcp_*", "monocs_udp_*", "monocs_resolve", "monocs_tls_*", nullptr };
static const char *const k_proc[] = { "system", "popen", "monocs_cc", "monocs_ld", "monocs_rexec_*", nullptr };
static const char *const k_sys[] = {
    "monocs_disk_*", "monocs_reboot", "settimeofday", "clock_settime", "monocs_set_timezone", "monocs_wifi_*",
    "monocs_console_*", "setenv", "putenv", "unsetenv", "setuid", "setgid", nullptr };
static const char *const k_cores[] = { "pthread_create", "monocs_core_start", "__monocs_omp_parallel", nullptr };
static const struct kind kinds[] = {
    { "files (open, read, list)", k_files },
    { "files (change, delete)", k_change },
    { "network", k_net },
    { "other programs", k_proc },
    { "system (SD card, reboot, clock, Wi-Fi, console, environment)", k_sys },
    { "other cores", k_cores },
};
#define NKINDS ((int)(sizeof kinds / sizeof kinds[0]))

static bool match(const char *pat, const char *name)
{
    size_t n = strlen(pat);
    if (n && pat[n - 1] == '*')
        return !strncmp(pat, name, n - 1);
    return !strcmp(pat, name);
}

static int kind_of(const char *name)
{
    for (int k = 0; k < NKINDS; k++)
        for (const char *const *p = kinds[k].names; *p; p++)
            if (match(*p, name))
                return k;
    return -1;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// ソースのファイルを読み、注釈と文字列を飛ばして、インラインアセンブリと周辺機器の番地を探す
static void scan_source(const char *path, int *found)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int c, prev = '\n', line = 1;
    char word[64];
    while ((c = getc(f)) != EOF) {
        if (c == '\n') {
            line++;
            prev = c;
            continue;
        }
        if (c == '/') {
            int d = getc(f);
            if (d == '/') {
                while ((c = getc(f)) != EOF && c != '\n')
                    ;
                line++;
                prev = '\n';
                continue;
            }
            if (d == '*') {
                int p = 0;
                while ((c = getc(f)) != EOF && !(p == '*' && c == '/')) {
                    if (c == '\n')
                        line++;
                    p = c;
                }
                prev = ' ';
                continue;
            }
            ungetc(d, f);
        } else if (c == '"' || c == '\'') {
            int q = c, d;
            while ((d = getc(f)) != EOF && d != q && d != '\n')
                if (d == '\\')
                    getc(f);
            if (d == '\n')
                line++;
            prev = q;
            continue;
        } else if ((isalnum(c) || c == '_') && !(isalnum(prev) || prev == '_')) {
            int n = 0;
            word[n++] = (char)c;
            while ((c = getc(f)) != EOF && (isalnum(c) || c == '_'))
                if (n < (int)sizeof word - 1)
                    word[n++] = (char)c;
            word[n] = 0;
            if (c != EOF)
                ungetc(c, f);
            if (!strcmp(word, "asm") || !strcmp(word, "__asm__") || !strcmp(word, "__asm")) {
                printf("  ! inline assembly at %s:%d\n", path, line);
                (*found)++;
            } else if ((word[0] == '0') && (word[1] == 'x' || word[1] == 'X')) {
                unsigned long long v = strtoull(word, nullptr, 16);
                if (v >= 0x3F000000ULL && v <= 0x40FFFFFFULL) {
                    printf("  ! hardware address %s at %s:%d\n", word, path, line);
                    (*found)++;
                }
            }
            prev = 'a';
            continue;
        }
        prev = c;
    }
    fclose(f);
}

void report_print(const char *const *files, int nfiles, const char *const *flags)
{
    recording = false;
    if (nfiles <= 0) {      // 作れなかったとき: 覚えたものを捨てるだけ
        for (int i = 0; i < nnames; i++)
            free(names[i]);
        nnames = 0;
        return;
    }
    qsort(names, (size_t)nnames, sizeof names[0], cmp_str);
    printf("%s%s uses:\n", files[0], nfiles > 1 ? " (and the other files)" : "");
    bool any = false;
    for (int k = 0; k < NKINDS; k++) {
        int shown = 0;
        for (int i = 0; i < nnames; i++) {
            if (kind_of(names[i]) != k)
                continue;
            if (shown++)
                printf(", %s", names[i]);
            else
                printf("  %s: %s", kinds[k].title, names[i]);
        }
        if (shown) {
            printf("\n");
            any = true;
        }
    }
    // ライブラリ（-l）: gpio などはハードウェアのレジスタを直接読み書きする
    for (int i = 0; flags && flags[i]; i++) {
        const char *l = flags[i];
        if (strncmp(l, "-l", 2))
            continue;
        static const char *const hw[] = { "gpio", "i2c", "spi", "pwm", "softuart", nullptr };
        bool is_hw = false;
        for (const char *const *h = hw; *h; h++)
            is_hw |= !strcmp(l + 2, *h);
        printf("  library: %s%s\n", l + 2, is_hw ? " (hardware: reads and writes the chip's registers)" : "");
        any = true;
    }
    int others = 0;
    for (int i = 0; i < nnames; i++)
        others += kind_of(names[i]) < 0;
    printf("  other OS functions: %d (C library, screen, keys, time, memory, math...)\n", others);
    int found = 0;
    for (int i = 0; i < nfiles; i++)
        scan_source(files[i], &found);
    if (!any && !found)
        printf("  (nothing beyond the screen, keys, time, memory and computation)\n");
    for (int i = 0; i < nnames; i++)
        free(names[i]);
    nnames = 0;
}
