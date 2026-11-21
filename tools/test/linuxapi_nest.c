// system() の子のアプリが変えた環境変数・getopt の状態が、親に残らないこと（tools/test/linuxapi.sh）
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "p:")) != -1)
        printf("parent opt %c %s\n", c, optarg);
    int keep = optind;
    setenv("MONOCS_PARENT", "p", 1);
    const char *child = argc > keep ? argv[keep] : "";
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s -a -b child", child);
    int r = system(cmd);
    printf("child status %d\n", r >> 8);
    printf("parent optind %d (was %d), env MONOCS_TEST=%s MONOCS_PARENT=%s\n", optind, keep,
           getenv("MONOCS_TEST") ? getenv("MONOCS_TEST") : "(none)", getenv("MONOCS_PARENT"));
    return 0;
}
