// cat [FILE...]: ファイルの中身を出す。なければ stdin（パイプ・<。キーボードなら Ctrl-D で終わり）。
// Monocs に付いてくるアプリ（/bin）
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        // ファイルがなければ stdin（パイプ・< のとき。キーボードなら Ctrl-D で終わり）
        int c;
        bool file = !isatty(0);
        while ((c = getchar()) != EOF && (file || c != 0x04)) {
            if (!file && c == '\r')
                c = '\n';
            putchar(c);
        }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        int c;
        while ((c = fgetc(f)) != EOF)
            putchar(c);
        fclose(f);
    }
    return status;
}
