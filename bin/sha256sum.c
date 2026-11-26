// sha256sum [-c] [FILE...]: SHA-256（GNU coreutils の sha256sum と同じ形の出力）。ファイルがなければ stdin。
//   sha256sum *.c *.h > cweb.txt     C の Web のフォルダのページの目録を作る（docs/cweb.md）
//   sha256sum -c cweb.txt            目録のとおりか確かめる（OK・FAILED。合わないものがあれば終了の値 1）
// 計算は OS の monocs_sha256（os/kernel/sha256.c）。Monocs に付いてくるアプリ（/usr/bin）
#include <monocs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(FILE *f, size_t *len)
{
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    for (size_t r; buf && (r = fread(buf + n, 1, cap - n, f)) > 0;) {
        n += r;
        if (n == cap) {
            char *b = realloc(buf, cap *= 2);
            if (!b) {
                free(buf);
                return nullptr;
            }
            buf = b;
        }
    }
    *len = n;
    return buf;
}

static bool hash_file(const char *name, char out[65])
{
    FILE *f = strcmp(name, "-") ? fopen(name, "rb") : stdin;
    if (!f) {
        fprintf(stderr, "sha256sum: %s: No such file or directory\n", name);
        return false;
    }
    size_t n;
    char *buf = read_all(f, &n);
    if (f != stdin)
        fclose(f);
    if (!buf) {
        fprintf(stderr, "sha256sum: %s: out of memory\n", name);
        return false;
    }
    uint8_t d[32];
    monocs_sha256(buf, n, d);
    free(buf);
    for (int i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
    return true;
}

static int check(const char *list)
{
    FILE *f = strcmp(list, "-") ? fopen(list, "r") : stdin;
    if (!f) {
        fprintf(stderr, "sha256sum: %s: No such file or directory\n", list);
        return 1;
    }
    char line[1024];
    int bad = 0, total = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
            continue;
        const char *name = line + 66;
        char got[65];
        bool ok = hash_file(name, got) && !strncasecmp(got, line, 64);
        printf("%s: %s\n", name, ok ? "OK" : "FAILED");
        bad += !ok;
        total++;
    }
    if (f != stdin)
        fclose(f);
    if (bad)
        fprintf(stderr, "sha256sum: WARNING: %d computed checksum%s did NOT match\n", bad, bad > 1 ? "s" : "");
    return bad || !total ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-c"))
        return check(argc > 2 ? argv[2] : "-");
    int r = 0;
    for (int i = 1; i < argc || i == 1; i++) {
        const char *name = i < argc ? argv[i] : "-";
        char h[65];
        if (hash_file(name, h))
            printf("%s  %s\n", h, name);
        else
            r = 1;
        if (i >= argc)
            break;
    }
    return r;
}
