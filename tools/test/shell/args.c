// 引数を [ ] で囲んで 1 行に（語の分け直しの試験）。-e NAME なら環境変数 NAME（なければ (unset)）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "-e") == 0) {
        const char *v = getenv(argv[2]);
        printf("%s\n", v ? v : "(unset)");
        return 0;
    }
    for (int i = 1; i < argc; i++)
        printf("%s[%s]", i > 1 ? " " : "", argv[i]);
    printf("\n");
    return 0;
}
