// ソケット（os/libc/socket.c）のうち、カーネルのほかの場所（VFS・select・ioctl・アプリの終わり）から使うもの
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define SOCKET_MAX      32      // 同時に開けるソケット（fd はファイルと同じ並びから。os/fs/vfs.h の VFS_MAX_FILES）

bool socket_is_fd(int fd);
ssize_t socket_read(int fd, void *buf, size_t len);
ssize_t socket_write(int fd, const void *buf, size_t len);
int socket_close(int fd);
// 待たずに調べる（select・poll）。fd がソケットでなければ false
bool socket_ready(int fd, bool *readable, bool *writable, bool *error, bool *hangup);
int socket_ioctl(int fd, unsigned long op, void *param);    // FIONBIO・FIONREAD
int socket_get_flags(int fd);           // fcntl の F_GETFL（O_NONBLOCK）
int socket_set_flags(int fd, int flags);
void socket_close_owner(int owner);     // owner（svc_owner）が開いたままのソケットを閉じる
