// tail [-n N] [FILE...]: ファイルの最後の N 行（既定 10）。ファイルがなければ stdin（パイプ・<）。
// ファイルが 2 つ以上なら「==> 名前 <==」を付ける（GNU と同じ）。Monocs に付いてくるアプリ（/bin）（tail で動く）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 全体を読み、最後の n 行を出す（行の始まりを後ろから数える）
static void tail(FILE *f, long n)
{
    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    int c;
    while (buf && (c = fgetc(f)) != EOF) {
        if (len == cap) {
            char *p = realloc(buf, cap * 2);
            if (!p)
                break;
            buf = p;
            cap *= 2;
        }
        buf[len++] = (char)c;
    }
    if (!buf)
        return;
    size_t start = len;
    long lines = 0;
    if (start > 0 && buf[start - 1] == '\n')
        start--;    // 最後の改行は数えない
    while (start > 0) {
        if (buf[start - 1] == '\n' && ++lines == n)
            break;
        start--;
    }
    if (n <= 0)
        start = len;
    fwrite(buf + start, 1, len - start, stdout);
    free(buf);
}

int main(int argc, char **argv)
{
    long n = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        n = atol(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        n = atol(argv[i] + 1);  // tail -5
        i++;
    }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;    // オプションの終わり（GNU と同じ）
    if (i == argc) {
        tail(stdin, n);
        return 0;
    }
    int status = 0;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) {
            fprintf(stderr, "tail: cannot open '%s' for reading\n", argv[k]);
            status = 1;
            continue;
        }
        if (argc - i > 1)
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        tail(f, n);
        fclose(f);
    }
    return status;
}
