// nos.py pushdir が送った束（パック）を展開する（Monocs 上で run する）
//   run unpack.c PACK DIR
// 形式: 「パスの長さ（4 バイト、リトルエンディアン）、パス（DIR からの相対）、中身の長さ（4 バイト）、中身」の繰り返し
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void make_parent_dirs(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(buf, 0777);
        *p = '/';
    }
}

static int rd32(FILE *f, unsigned *v)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *v = b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: run unpack.c PACK DIR\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("cannot open %s\n", argv[1]);
        return 1;
    }
    unsigned n, size, count = 0;
    unsigned long total = 0;
    static char rel[400], path[512];
    static unsigned char buf[65536];
    while (rd32(f, &n)) {
        if (n >= sizeof(rel) || fread(rel, 1, n, f) != n || !rd32(f, &size)) {
            printf("broken pack\n");
            return 1;
        }
        rel[n] = '\0';
        snprintf(path, sizeof(path), "%s/%s", argv[2], rel);
        make_parent_dirs(path);
        FILE *o = fopen(path, "wb");
        if (!o) {
            printf("cannot write %s\n", path);
            return 1;
        }
        for (unsigned left = size; left;) {
            unsigned k = left < sizeof(buf) ? left : sizeof(buf);
            if (fread(buf, 1, k, f) != k || fwrite(buf, 1, k, o) != k) {
                printf("i/o error at %s\n", path);
                return 1;
            }
            left -= k;
        }
        if (fclose(o) != 0) {
            printf("cannot write %s\n", path);
            return 1;
        }
        count++;
        total += size;
    }
    fclose(f);
    printf("unpack: %u files, %lu bytes\n", count, total);
    return 0;
}
