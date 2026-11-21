// df [-h]: SD カード（FAT の区画）の大きさと空き。Monocs に付いてくるアプリ（/bin）
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
#include <sys/statvfs.h>

static void human_size(char *buf, size_t n, long size, bool human)
{
    if (!human || size < 1024) {
        snprintf(buf, n, "%ld", size);
        return;
    }
    static const char units[] = "KMGT";
    double v = (double)size;
    int u = -1;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        u++;
    }
    if (v < 10)
        snprintf(buf, n, "%.1f%c", (double)((long)(v * 10 + 0.999)) / 10, units[u]);   // 切り上げ（GNU と同じ）
    else
        snprintf(buf, n, "%ld%c", (long)(v + 0.999), units[u]);
}

// df [-h]: SD カード（FAT の区画）の大きさと空き
int main(int argc, char **argv)
{
    bool human = argc > 1 && !strcmp(argv[1], "-h");
    struct statvfs v;
    if (statvfs("/", &v) != 0) {
        perror("df");
        return 1;
    }
    unsigned long long total = (unsigned long long)v.f_blocks * v.f_frsize;
    unsigned long long avail = (unsigned long long)v.f_bavail * v.f_frsize;
    unsigned long long used = total - avail;
    int pct = total ? (int)((used * 100 + total - 1) / total) : 0;
    if (human) {
        char t[16], u[16], a[16];
        human_size(t, sizeof(t), (long)total, true);
        human_size(u, sizeof(u), (long)used, true);
        human_size(a, sizeof(a), (long)avail, true);
        printf("Filesystem      Size  Used Avail Use%% Mounted on\n");
        printf("sdcard        %6s %5s %5s %3d%% /\n", t, u, a, pct);
    } else {
        printf("Filesystem     1K-blocks     Used Available Use%% Mounted on\n");
        printf("sdcard        %10llu %8llu %9llu %3d%% /\n", total / 1024, used / 1024, avail / 1024, pct);
    }
    return 0;
}
