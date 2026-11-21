// echo [-n] 文字...: 文字を出す（> FILE と組み合わせて）。Monocs に付いてくるアプリ（/bin）
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

// echo [-n] 文字...
int main(int argc, char **argv)
{
    bool newline = !(argc > 1 && !strcmp(argv[1], "-n"));
    for (int i = newline ? 1 : 2; i < argc; i++)
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (newline)
        putchar('\n');
    return 0;
}
