// cp [-r] FROM... TO: コピー（-r でディレクトリごと。TO がディレクトリなら、その中へ同じ名前で）。
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

static int copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    static char buf[32 * 1024];
    ssize_t n;
    int r = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n) {
            r = -1;
            break;
        }
    if (n < 0)
        r = -1;
    close(in);
    if (close(out) != 0)
        r = -1;
    return r;
}

static int copy_tree(const char *from, const char *to)
{
    if (!is_dir(from))
        return copy_file(from, to);
    if (!is_dir(to) && mkdir(to, 0755) != 0)
        return -1;
    DIR *d = opendir(from);
    if (!d)
        return -1;
    int r = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char a[1024 * 2], b[1024 * 2];
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        if (copy_tree(a, b) != 0)
            r = -1;
    }
    closedir(d);
    return r;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

// cp [-r] FROM... TO（TO がディレクトリなら、その中へ同じ名前で）
int main(int argc, char **argv)
{
    bool recursive = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--"); i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R')
                recursive = true;
            else {
                fprintf(stderr, "cp: invalid option -- '%c'\nusage: cp [-r] FROM... TO\n", *o);
                return 1;
            }
        }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;    // オプションの終わり（GNU と同じ。続くものは、- で始まっても名前）
    if (argc - i < 2) {
        fprintf(stderr, "usage: cp [-r] FROM... TO\n");
        return 1;
    }
    const char *dst = argv[argc - 1];
    bool dst_dir = is_dir(dst);
    if (argc - i > 2 && !dst_dir) {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dst);
        return 1;
    }
    int status = 0;
    for (; i < argc - 1; i++) {
        const char *src = argv[i];
        struct stat st;
        if (stat(src, &st) != 0) {
            fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
            status = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode) && !recursive) {
            fprintf(stderr, "cp: -r not specified; omitting directory '%s'\n", src);
            status = 1;
            continue;
        }
        char to[1024 * 2];
        if (dst_dir)
            snprintf(to, sizeof(to), "%s/%s", dst, base_name(src));
        else
            snprintf(to, sizeof(to), "%s", dst);
        char ra[PATH_MAX], rb[PATH_MAX];
        if (realpath(src, ra) && realpath(to, rb) && !strcasecmp(ra, rb)) {
            fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, to);
            status = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode) && realpath(src, ra) && realpath(dst, rb) && !strncasecmp(rb, ra, strlen(ra)) &&
            (rb[strlen(ra)] == '/' || rb[strlen(ra)] == '\0')) {
            fprintf(stderr, "cp: cannot copy a directory, '%s', into itself, '%s'\n", src, to);
            status = 1;
            continue;
        }
        if ((S_ISDIR(st.st_mode) ? copy_tree(src, to) : copy_file(src, to)) != 0) {
            fprintf(stderr, "cp: cannot copy '%s' to '%s': %s\n", src, to, strerror(errno));
            status = 1;
        }
    }
    return status;
}
