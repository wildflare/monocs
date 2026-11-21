// /lib/tls と /bin/curl を Linux（Pi 5）で試すための代わり: monocs.h の TCP・名前・時計を、Linux のソケットで。
// tools/tls/test.sh が使う（Monocs の上では使わない）。
#define _GNU_SOURCE
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

uint64_t timer_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

bool monocs_clock_known(void)
{
    return true;
}

int monocs_resolve(const char *name, char *ip, int size, int timeout_ms)
{
    (void)timeout_ms;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0)
        return -1;
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, (socklen_t)size);
    freeaddrinfo(res);
    return 0;
}

int monocs_tcp_connect(const char *host, int port, int timeout_ms)
{
    char ip[16];
    if (monocs_resolve(host, ip, sizeof ip, timeout_ms) != 0)
        return -1;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, ip, &sa.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

int monocs_tcp_send(int h, const void *buf, int len, int timeout_ms)
{
    (void)timeout_ms;
    ssize_t n = send(h, buf, (size_t)len, MSG_NOSIGNAL);
    return n < 0 ? -1 : (int)n;
}

int monocs_tcp_recv(int h, void *buf, int len, int timeout_ms)
{
    struct pollfd p = {.fd = h, .events = POLLIN};
    int r = poll(&p, 1, timeout_ms);
    if (r == 0)
        return -1;
    if (r < 0)
        return -2;
    ssize_t n = recv(h, buf, (size_t)len, 0);
    return n < 0 ? -2 : (int)n;
}

void monocs_tcp_close(int h)
{
    close(h);
}
