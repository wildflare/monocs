// utime（Linux の glibc と同じく、utime.h で struct utimbuf と utime を宣言する。picolibc の utime.h は
// struct utimbuf だけで、utime の宣言がない。2026-11-20、vim の bufwrite.c。本体は os/fs/vfs.c）
#ifndef _UTIME_H
#define _UTIME_H 1

#include <sys/utime.h>

#ifdef __cplusplus
extern "C" {
#endif

int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif
