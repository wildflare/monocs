// 同時に開ける fd（ファイルとソケットで番号を分け合う。os/fs/vfs.h の VFS_MAX_FILES）の試験。
// Monocs の実機で: nos.py run tools/test/fds/fdtest.c。Linux でも同じ出力になる（ulimit -n が 256 のとき。2026-11-21 に 64 から）
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int fds[300];

int main(void)
{
#ifdef __linux__
    struct rlimit rl = {256, 256};
    setrlimit(RLIMIT_NOFILE, &rl);
#endif
    int n = 0;
    char name[64];
    for (;;) {
        snprintf(name, sizeof name, "/tmp/fdtest%d", n);
        int fd = open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            printf("opened %d files, then %s\n", n, errno == EMFILE ? "EMFILE" : strerror(errno));
            break;
        }
        if (fd != n + 3)
            printf("file %d got fd %d (want %d)\n", n, fd, n + 3);
        fds[n++] = fd;
        if (n == 300)
            break;
    }
    // 真ん中を 2 つ閉じると、いちばん小さい番号から使い回す（ソケットでも）
    close(fds[10]);
    close(fds[20]);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int f = open("/tmp/fdtest0", O_RDONLY);
    printf("socket got fd %d, file got fd %d\n", s, f);
    int s2 = socket(AF_INET, SOCK_DGRAM, 0);
    printf("one more socket: %d (%s)\n", s2, s2 < 0 && errno == EMFILE ? "EMFILE" : "?");
    // いちばん大きい番号のソケットで select
    close(fds[n - 1]);
    int big = socket(AF_INET, SOCK_DGRAM, 0);
    fd_set w;
    FD_ZERO(&w);
    FD_SET(big, &w);
    struct timeval tv = {0, 0};
    int r = select(big + 1, NULL, &w, NULL, &tv);
    printf("socket fd %d, select -> %d, writable %d\n", big, r, FD_ISSET(big, &w) ? 1 : 0);
    // 閉じたソケットの番号はファイルに使える
    close(s);
    int f2 = open("/tmp/fdtest1", O_RDONLY);
    printf("file after closing socket %d: fd %d\n", s, f2);
    char buf[4];
    ssize_t rd = read(s == f2 ? 999 : s, buf, 1);
    printf("read on a socket fd closed: %zd (%s)\n", rd, errno == EBADF ? "EBADF" : "?");
    for (int i = 0; i < n; i++)
        snprintf(name, sizeof name, "/tmp/fdtest%d", i), unlink(name);
    return 0;
}
