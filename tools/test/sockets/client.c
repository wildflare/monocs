// ソケットの試験（Monocs で動かす。相手は Pi 5 の tools/test/sockets/run.py）。Linux でもそのまま動く
//   client HOST TCP_PORT UDP_PORT CLOSED_PORT [WEB_HOST]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int open_tcp(const char *host, const char *port, int nonblock)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    int e = getaddrinfo(host, port, &hints, &res);
    if (e) {
        printf("getaddrinfo %s: %s\n", host, gai_strerror(e));
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype | (nonblock ? SOCK_NONBLOCK : 0), res->ai_protocol);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    if (r < 0 && !(nonblock && errno == EINPROGRESS)) {
        printf("connect %s:%s: %s\n", host, port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: client HOST TCP_PORT UDP_PORT CLOSED_PORT [WEB_HOST]\n");
        return 2;
    }
    const char *host = argv[1];
    // 1. TCP: 1MB を送って、エコーを読む（送りながら読む: select で）
    int fd = open_tcp(host, argv[2], 0);
    if (fd < 0)
        return 1;
    enum { TOTAL = 1 << 20 };
    static unsigned char out[TOTAL], in[TOTAL];
    for (int i = 0; i < TOTAL; i++)
        out[i] = (unsigned char)(i * 7 + i / 251);
    size_t sent = 0, got = 0;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    while (got < TOTAL) {
        fd_set rf, wf;
        FD_ZERO(&rf);
        FD_ZERO(&wf);
        FD_SET(fd, &rf);
        if (sent < TOTAL)
            FD_SET(fd, &wf);
        struct timeval tv = {10, 0};
        if (select(fd + 1, &rf, &wf, NULL, &tv) <= 0) {
            printf("tcp select timeout (sent %zu got %zu)\n", sent, got);
            return 1;
        }
        if (FD_ISSET(fd, &wf)) {
            ssize_t n = send(fd, out + sent, TOTAL - sent > 8192 ? 8192 : TOTAL - sent, MSG_DONTWAIT);
            if (n > 0)
                sent += (size_t)n;
        }
        if (FD_ISSET(fd, &rf)) {
            ssize_t n = recv(fd, in + got, TOTAL - got, 0);
            if (n <= 0) {
                printf("tcp recv %zd: %s\n", n, strerror(errno));
                return 1;
            }
            got += (size_t)n;
        }
    }
    gettimeofday(&t1, NULL);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    printf("tcp echo 1MB: %s (%.2f s)\n", memcmp(in, out, TOTAL) ? "MISMATCH" : "match", sec);
    struct sockaddr_in me, peer;
    socklen_t ml = sizeof me, pl = sizeof peer;
    getsockname(fd, (struct sockaddr *)&me, &ml);
    getpeername(fd, (struct sockaddr *)&peer, &pl);
    printf("tcp ports: local>0 %d peer %d\n", ntohs(me.sin_port) > 0, ntohs(peer.sin_port) == atoi(argv[2]));
    // MSG_PEEK: 覗いた後に普通に読むと、同じものが読める
    send(fd, "peek", 4, 0);
    char pk[4], rd[4];
    ssize_t np = recv(fd, pk, 4, MSG_PEEK | MSG_WAITALL);
    ssize_t nr = recv(fd, rd, 4, MSG_WAITALL);
    printf("tcp peek: %s\n", np == 4 && nr == 4 && !memcmp(pk, "peek", 4) && !memcmp(rd, "peek", 4) ? "match" : "bad");
    shutdown(fd, SHUT_WR);  // 相手は読み終えて閉じる
    char c;
    printf("tcp after shutdown: recv %zd\n", recv(fd, &c, 1, 0));
    close(fd);

    // 2. 待たない connect + poll（書けるか）+ SO_ERROR
    fd = open_tcp(host, argv[2], 1);
    struct pollfd p = {fd, POLLOUT, 0};
    int pr = poll(&p, 1, 5000);
    int err = -1;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    printf("nonblock connect: poll %d out %d so_error %d\n", pr, (p.revents & POLLOUT) != 0, err);
    close(fd);

    // 3. 断られる
    fd = open_tcp(host, argv[4], 0);
    printf("closed port: %s\n", fd < 0 ? "refused (above)" : "connected?");
    if (fd >= 0)
        close(fd);

    // 4. UDP: エコーと時間切れ
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[3]))};
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res;
    getaddrinfo(host, NULL, &hints, &res);
    to.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    struct timeval tmo = {2, 0};
    setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    const char msg[] = "hello udp";
    sendto(us, msg, sizeof msg, 0, (struct sockaddr *)&to, sizeof to);
    char ubuf[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t un = recvfrom(us, ubuf, sizeof ubuf, 0, (struct sockaddr *)&from, &fl);
    printf("udp echo: %s from port %d\n", un == (ssize_t)sizeof msg && !memcmp(ubuf, msg, sizeof msg) ? "match" : "bad",
           ntohs(from.sin_port) == atoi(argv[3]));
    gettimeofday(&t0, NULL);
    un = recvfrom(us, ubuf, sizeof ubuf, 0, NULL, NULL);
    gettimeofday(&t1, NULL);
    sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    printf("udp timeout: %zd %s ~2s %d\n", un, errno == EAGAIN ? "EAGAIN" : strerror(errno), sec > 1.8 && sec < 2.6);
    close(us);

    // 5. 名前を引いて HTTP（Wi-Fi の先）
    if (argc > 5) {
        fd = open_tcp(argv[5], "http", 0);
        if (fd >= 0) {
            char req[256];
            snprintf(req, sizeof req, "HEAD / HTTP/1.0\r\nHost: %s\r\n\r\n", argv[5]);
            send(fd, req, strlen(req), 0);
            char resp[512];
            ssize_t n = recv(fd, resp, sizeof resp - 1, MSG_WAITALL);
            resp[n > 0 ? n : 0] = 0;
            char *nl = strchr(resp, '\r');
            if (nl)
                *nl = 0;
            printf("http %s: %s\n", argv[5], strncmp(resp, "HTTP/1.", 7) ? "bad" : resp + 9);
            close(fd);
        }
    }
    return 0;
}
