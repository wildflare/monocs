// touch FILE...: なければ空のファイルを作り、あれば日時を今にする（utime）。Monocs に付いてくるアプリ（/bin）
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
#include <monocs.h>

// touch FILE...（なければ空のファイルを作り、あれば日時を今にする）
int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        int r;
        if (stat(argv[i], &st) == 0) {
            r = utime(argv[i], nullptr);
        } else {
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
            r = fd < 0 ? -1 : close(fd);
        }
        if (r != 0) {
            fprintf(stderr, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
