// gunzip [-c] [-k] [-f] [FILE.gz...]: .gz を展開する。zcat は gunzip -c と同じ（bin/zcat）。
// Monocs に付いてくるアプリ（/bin）。Linux の gunzip に似せた小さなもの（圧縮する gzip はない）
//   FILE.gz → FILE（.tgz は .tar）。うまくいったら元の .gz を消す（-k なら残す）。もうあれば上書きしない（-f なら上書き）
//   -c は標準出力へ（元は残す）。FILE がなければ標準入力から標準出力へ
// 中身は共有のライブラリ /lib/inflate（monocs_inflate.h）。
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <monocs_inflate.h>

#ifndef ZCAT
#define ZCAT 0
#endif
#define PROG (ZCAT ? "zcat" : "gunzip")

static int rd(void *ctx, void *buf, int size)
{
    size_t n = fread(buf, 1, (size_t)size, ctx);
    return n == 0 && ferror((FILE *)ctx) ? -1 : (int)n;
}

static int wr(void *ctx, const void *buf, int len)
{
    return fwrite(buf, 1, (size_t)len, ctx) == (size_t)len ? 0 : -1;
}

int main(int argc, char **argv)
{
    bool to_stdout = ZCAT, keep = false, force = false;
    int status = 0, nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == 'c')
                    to_stdout = true;
                else if (*p == 'k')
                    keep = true;
                else if (*p == 'f')
                    force = true;
                else if (*p != 'd' && *p != 'q') {
                    fprintf(stderr, "Usage: %s [-c] [-k] [-f] [FILE.gz...]\n", PROG);
                    return 1;
                }
            }
            continue;
        }
        nfiles++;
        const char *in_name = argv[i];
        char out_name[512];
        size_t n = strlen(in_name);
        if (n > 3 && !strcmp(in_name + n - 3, ".gz")) {
            snprintf(out_name, sizeof out_name, "%.*s", (int)(n - 3), in_name);
        } else if (n > 4 && !strcmp(in_name + n - 4, ".tgz")) {
            snprintf(out_name, sizeof out_name, "%.*s.tar", (int)(n - 4), in_name);
        } else if (!to_stdout) {
            fprintf(stderr, "%s: %s: unknown suffix -- ignored\n", PROG, in_name);
            status = 2;
            continue;
        }
        FILE *in = fopen(in_name, "rb");
        if (!in) {
            fprintf(stderr, "%s: ", PROG);
            perror(in_name);
            status = 1;
            continue;
        }
        FILE *out = stdout;
        if (!to_stdout) {
            struct stat st;
            if (!force && stat(out_name, &st) == 0) {
                fprintf(stderr, "%s: %s already exists; not overwritten (use -f)\n", PROG, out_name);
                fclose(in);
                status = 2;
                continue;
            }
            out = fopen(out_name, "wb");
            if (!out) {
                fprintf(stderr, "%s: ", PROG);
                perror(out_name);
                fclose(in);
                status = 1;
                continue;
            }
        }
        const char *err = nullptr;
        int r = monocs_inflate(MONOCS_INFLATE_GZIP, rd, in, wr, out, &err);
        fclose(in);
        bool closed = to_stdout || fclose(out) == 0;
        if (r != 0 || !closed) {
            fprintf(stderr, "%s: %s: %s\n", PROG, in_name, r ? err : "write error");
            if (!to_stdout)
                unlink(out_name);   // 途中までのものは残さない
            status = 1;
        } else if (!to_stdout && !keep) {
            unlink(in_name);
        }
    }
    if (nfiles == 0) {
        const char *err = nullptr;
        if (monocs_inflate(MONOCS_INFLATE_GZIP, rd, stdin, wr, stdout, &err) != 0) {
            fprintf(stderr, "%s: stdin: %s\n", PROG, err);
            status = 1;
        }
    }
    return status;
}
