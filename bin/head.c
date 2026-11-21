// head [-n N] [FILE...]: ファイルの先頭の N 行（既定 10）。ファイルがなければ stdin（パイプ・<）。
// ファイルが 2 つ以上なら「==> 名前 <==」を付ける（GNU と同じ）。Monocs に付いてくるアプリ（/bin）（head で動く）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void head(FILE *f, long n)
{
    int c;
    while (n > 0 && (c = fgetc(f)) != EOF) {
        putchar(c);
        if (c == '\n')
            n--;
    }
}

int main(int argc, char **argv)
{
    long n = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        n = atol(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        n = atol(argv[i] + 1);  // head -5
        i++;
    }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;    // オプションの終わり（GNU と同じ）
    if (i == argc) {
        head(stdin, n);
        return 0;
    }
    int status = 0;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) {
            fprintf(stderr, "head: cannot open '%s' for reading\n", argv[k]);
            status = 1;
            continue;
        }
        if (argc - i > 1)
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        head(f, n);
        fclose(f);
    }
    return status;
}
