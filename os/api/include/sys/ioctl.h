// ioctl（Monocs の実装は os/libc/termios.c と os/libc/socket.c）。picolibc の sys/ioctl.h には struct winsize がなく、
// Linux のプログラム（#include <sys/ioctl.h> だけで ioctl(fd, TIOCGWINSZ, &ws) を使う）がコンパイルできないので
// 置き換える。値は Monocs の実装に合わせて picolibc のもの（TIOCGWINSZ など）と、Linux のもの（FIONREAD・FIONBIO）。
// ioctl は Linux と同じく可変長の引数（ioctl(fd, 命令, 0) のように数を渡すものも通る。AArch64 では同じ渡し方）
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H 1
#define _SYS_IOCTL_H_   // picolibc のものを読まないように

#include <sys/termios.h>    // struct winsize（picolibc）

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long op, ...);

#define TIOCGWINSZ  (('T' << 8) | 1)
#define TIOCSWINSZ  (('T' << 8) | 2)
#define TIOCLINUX   (('T' << 8) | 3)
#ifndef FIONREAD
#define FIONREAD    0x541B
#endif
#ifndef FIONBIO
#define FIONBIO     0x5421
#endif

#ifdef __cplusplus
}
#endif

#endif
