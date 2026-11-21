// ar.h を Linux で確かめる: 既存の .a のメンバーを読み、ar_write で書き直す（GNU ar の出力と比べる）
//   gcc -O2 -o ar_test tools/selfhost/ar_test.c && ./ar_test IN.a OUT.a && cmp IN.a OUT.a
#include "ar.h"

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(size);
    fread(d, 1, size, f);
    fclose(f);
    ArMember *m = calloc(size / 60 + 1, sizeof(ArMember));
    size_t n = 0;
    const char *ltab = NULL;
    for (long off = 8; off + 60 <= size;) {
        const char *h = (const char *)d + off;
        size_t msize = strtoul(h + 48, NULL, 10);
        if (!strncmp(h, "// ", 3)) {
            ltab = (const char *)d + off + 60;
        } else if (strncmp(h, "/ ", 2)) {
            char *name = calloc(1, 256);
            if (h[0] == '/') {
                const char *p = ltab + strtoul(h + 1, NULL, 10);
                size_t k = strcspn(p, "/");
                memcpy(name, p, k);
            } else {
                size_t k = strcspn(h, "/");
                memcpy(name, h, k);
            }
            m[n].name = name;
            m[n].data = d + off + 60;
            m[n].size = msize;
            n++;
        }
        off += 60 + msize + (msize & 1);
    }
    printf("%zu members\n", n);
    return ar_write(argv[2], m, n);
}
