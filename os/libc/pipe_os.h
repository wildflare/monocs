// 同じプログラムの中の pipe（os/libc/pipe.c）のうち、カーネルのほかの場所（VFS・select・ioctl・アプリの終わり）から使うもの
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct stat;
bool pipe_is_fd(int fd);
ssize_t pipe_read(int fd, void *buf, size_t len);
ssize_t pipe_write(int fd, const void *buf, size_t len);
int pipe_close(int fd);
bool pipe_ready(int fd, bool *readable, bool *writable, bool *hangup);   // 待たずに調べる（fd が pipe でなければ false）
int pipe_get_flags(int fd);         // fcntl の F_GETFL（O_RDONLY・O_WRONLY と O_NONBLOCK）
int pipe_set_flags(int fd, int flags);
size_t pipe_available(int fd);      // FIONREAD
void pipe_fstat(int fd, struct stat *st);
void pipe_close_owner(int owner);   // owner（svc_owner）が開いたままの pipe を閉じる
