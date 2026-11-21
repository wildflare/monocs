// rmdir DIR...: 空のディレクトリの削除。Monocs に付いてくるアプリ（/bin）
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

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!is_dir(argv[i]) || rmdir(argv[i]) != 0) {
            fprintf(stderr, "rmdir: failed to remove '%s': %s\n", argv[i],
                    is_dir(argv[i]) ? "Directory not empty" : "Not a directory");
            status = 1;
        }
    }
    return status;
}
