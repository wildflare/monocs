// rm [-rf] PATH...: 削除（GNU と同じく、ディレクトリは -r のときだけ。-f はなくてもエラーにしない）。
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

// ディレクトリの中身ごと消す
static int remove_tree(const char *path)
{
    if (!is_dir(path))
        return unlink(path);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int r = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[1024 * 2];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        if (remove_tree(sub) != 0)
            r = -1;
    }
    closedir(d);
    return r == 0 ? rmdir(path) : r;
}

// rm [-rf] PATH...（GNU と同じく、ディレクトリは -r のときだけ）
int main(int argc, char **argv)
{
    bool recursive = false, force = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--"); i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R')
                recursive = true;
            else if (*o == 'f')
                force = true;
            else {
                fprintf(stderr, "rm: invalid option -- '%c'\nusage: rm [-rf] PATH...\n", *o);
                return 1;
            }
        }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;    // オプションの終わり（GNU と同じ。続くものは、- で始まっても名前）
    int status = 0;
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            if (!force) {
                fprintf(stderr, "rm: cannot remove '%s': %s\n", argv[i], strerror(errno));
                status = 1;
            }
            continue;
        }
        if (S_ISDIR(st.st_mode) && !recursive) {
            fprintf(stderr, "rm: cannot remove '%s': Is a directory\n", argv[i]);
            status = 1;
            continue;
        }
        if ((S_ISDIR(st.st_mode) ? remove_tree(argv[i]) : unlink(argv[i])) != 0) {
            fprintf(stderr, "rm: cannot remove '%s': %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
