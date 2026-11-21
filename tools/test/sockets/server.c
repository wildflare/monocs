// ソケットの試験のサーバー（Monocs で動かす）: select で複数の接続を同時に扱うエコーサーバー。
// "quit\n" を受けたら終わる。Linux でもそのまま動く。  server PORT
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 9008;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY};
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("server ready %d\n", port);
    fflush(stdout);
    int clients[16], n = 0, served = 0, maxconc = 0;
    long bytes = 0;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int maxfd = ls;
        for (int i = 0; i < n; i++) {
            FD_SET(clients[i], &rf);
            if (clients[i] > maxfd)
                maxfd = clients[i];
        }
        if (select(maxfd + 1, &rf, NULL, NULL, NULL) < 0) {
            perror("select");
            return 1;
        }
        if (FD_ISSET(ls, &rf)) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0 && n < 16) {
                clients[n++] = c;
                if (n > maxconc)
                    maxconc = n;
            }
        }
        for (int i = 0; i < n; i++) {
            if (!FD_ISSET(clients[i], &rf))
                continue;
            char buf[4096];
            ssize_t r = read(clients[i], buf, sizeof buf);
            if (r <= 0) {
                close(clients[i]);
                clients[i--] = clients[--n];
                served++;
                continue;
            }
            if (r >= 5 && !memcmp(buf, "quit\n", 5)) {
                printf("server done: served %d, max concurrent %d, %ld bytes\n", served, maxconc, bytes);
                return 0;
            }
            bytes += r;
            for (ssize_t w = 0; w < r;) {
                ssize_t k = write(clients[i], buf + w, (size_t)(r - w));
                if (k <= 0)
                    break;
                w += k;
            }
        }
    }
}
