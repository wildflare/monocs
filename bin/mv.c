// mv FROM... TO: 名前の変更・移動（TO がディレクトリなら、その中へ同じ名前で。行き先のファイルは置き換える）。
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

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mv FROM... TO\n");
        return 1;
    }
    const char *dst = argv[argc - 1];
    bool dst_dir = is_dir(dst);
    if (argc > 3 && !dst_dir) {
        fprintf(stderr, "mv: target '%s' is not a directory\n", dst);
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc - 1; i++) {
        char to[1024];
        if (dst_dir) {
            const char *b = strrchr(argv[i], '/');
            snprintf(to, sizeof(to), "%s/%s", dst, b && b[1] ? b + 1 : argv[i]);
        } else {
            snprintf(to, sizeof(to), "%s", dst);
        }
        if (rename(argv[i], to) != 0) {
            fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n", argv[i], to, strerror(errno));
            status = 1;
        }
    }
    return status;
}
