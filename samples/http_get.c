// TCP のクライアントの例: HTTP で GET して、応答を表示する
//   cc -o http_get samples/http_get.c && ./http_get [IP] [PORT] [PATH]    （既定は 192.168.7.1 8000 /）
// Pi 5 で: python3 -m http.server 8000
#include <stdio.h>
#include <stdlib.h>
#include <monocs.h>

int main(int argc, char **argv)
{
    const char *ip = argc > 1 ? argv[1] : "192.168.7.1";
    int port = argc > 2 ? atoi(argv[2]) : 8000;
    const char *path = argc > 3 ? argv[3] : "/";
    int c = monocs_tcp_connect(ip, port, 3000);
    if (c < 0) {
        printf("cannot connect to %s:%d\n", ip, port);
        return 1;
    }
    char req[512];
    int len = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, ip);
    monocs_tcp_send(c, req, len, -1);
    char buf[1024];
    int n;
    long total = 0;
    while ((n = monocs_tcp_recv(c, buf, sizeof(buf), 5000)) > 0) {
        fwrite(buf, 1, n, stdout);
        total += n;
    }
    monocs_tcp_close(c);
    printf("\n(%ld bytes)\n", total);
    return 0;
}
