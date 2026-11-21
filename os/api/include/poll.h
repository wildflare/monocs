// poll（Monocs の実装は os/libc/select.c）。picolibc の poll.h は __size_t の宣言を読まずに使っていて、単独で読むと
// コンパイルできないので、Linux（glibc 2.41）と同じ形のもので置き換える（定数の値は Linux・picolibc と同じ）
#ifndef _POLL_H
#define _POLL_H 1
#define _POLL_H_    // picolibc のものを読まないように

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd {
    int fd;
    short events;
    short revents;
};
typedef unsigned long nfds_t;

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLRDBAND  0x0080
#define POLLWRNORM  0x0100
#define POLLWRBAND  0x0200
#define POLLMSG     0x0400
#define POLLRDHUP   0x2000

int poll(struct pollfd fds[], nfds_t n, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
