// mkdir [-p] DIR...: ディレクトリの作成（-p: 途中も作り、あってもエラーにしない）。Monocs に付いてくるアプリ（/bin）
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

// mkdir [-p] DIR...（-p: 途中のディレクトリも作り、あってもエラーにしない）
int main(int argc, char **argv)
{
    bool parents = argc > 1 && !strcmp(argv[1], "-p");
    int status = 0;
    for (int i = parents ? 2 : 1; i < argc; i++) {
        if (parents) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", argv[i]);
            for (char *p = path + 1;; p++) {
                if (*p == '/' || *p == '\0') {
                    char c = *p;
                    *p = '\0';
                    if (!is_dir(path) && mkdir(path, 0755) != 0) {
                        fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", path, strerror(errno));
                        status = 1;
                        break;
                    }
                    *p = c;
                    if (!c)
                        break;
                }
            }
        } else if (mkdir(argv[i], 0755) != 0) {
            fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
