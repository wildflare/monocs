// wc [-l] [-w] [-c] [FILE...]: 行・語・バイトの数。ファイルがなければ stdin（パイプ・<）。
// 2 つ以上なら合計も（GNU と同じ。オプションがなければ 3 つとも）。Monocs に付いてくるアプリ（/bin）（wc で動く）
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool show_l, show_w, show_c;
static int width = 7;   // 数の幅（GNU と同じく、ファイルなら合計のバイト数の桁。stdin なら 7）

static void count(FILE *f, long *l, long *w, long *c)
{
    int ch;
    bool in_word = false;
    *l = *w = *c = 0;
    while ((ch = fgetc(f)) != EOF) {
        (*c)++;
        if (ch == '\n')
            (*l)++;
        if (isspace(ch)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            (*w)++;
        }
    }
}

static void print(long l, long w, long c, const char *name)
{
    const char *sep = "";
    if (show_l)
        printf("%s%*ld", sep, width, l), sep = " ";
    if (show_w)
        printf("%s%*ld", sep, width, w), sep = " ";
    if (show_c)
        printf("%s%*ld", sep, width, c), sep = " ";
    printf(name ? " %s\n" : "\n", name);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--"); i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l')
                show_l = true;
            else if (*o == 'w')
                show_w = true;
            else if (*o == 'c')
                show_c = true;
            else {
                fprintf(stderr, "wc: invalid option -- '%c'\nusage: wc [-lwc] [FILE...]\n", *o);
                return 1;
            }
        }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;    // オプションの終わり（GNU と同じ。続くものは、- で始まっても名前）
    if (!show_l && !show_w && !show_c)
        show_l = show_w = show_c = true;
    long l, w, c;
    if (i == argc) {
        if (show_l + show_w + show_c == 1)
            width = 1;  // stdin の 1 つの数だけなら、そろえない（GNU と同じ。3 つなら 7 桁）
        count(stdin, &l, &w, &c);
        print(l, w, c, nullptr);
        return 0;
    }
    // 幅: 合計のバイト数の桁（GNU の wc と同じ）
    long total = 0;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            total += ftell(f);
            fclose(f);
        }
    }
    width = 1;
    for (long t = total; t >= 10; t /= 10)
        width++;
    if ((show_l + show_w + show_c) == 1 && argc - i == 1)
        width = 1;      // 1 つのファイルの 1 つの数だけなら、そろえない（GNU と同じ）
    long tl = 0, tw = 0, tc = 0;
    int status = 0;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) {
            fprintf(stderr, "wc: %s: No such file or directory\n", argv[k]);
            status = 1;
            continue;
        }
        count(f, &l, &w, &c);
        fclose(f);
        print(l, w, c, argv[k]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (argc - i > 1)
        print(tl, tw, tc, "total");
    return status;
}
