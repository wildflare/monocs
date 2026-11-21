// monocs_inflate のテスト（Linux）: inftest FORMAT < in > out   終了の値 0 = 成功
#include <stdio.h>
#include <stdlib.h>
#include "monocs_inflate.h"
static int chunk = 16384;
static int rd(void *c, void *b, int n) { if (n > chunk) n = chunk; return (int)fread(b, 1, (size_t)n, c); }
static int wr(void *c, const void *b, int n) { return fwrite(b, 1, (size_t)n, c) == (size_t)n ? 0 : -1; }
int main(int argc, char **argv) {
    int fmt = argc > 1 ? atoi(argv[1]) : 2;
    if (argc > 2) chunk = atoi(argv[2]);
    const char *err = 0;
    int r = monocs_inflate(fmt, rd, stdin, wr, stdout, &err);
    if (r) fprintf(stderr, "error %d: %s\n", r, err);
    return r ? 1 : 0;
}
