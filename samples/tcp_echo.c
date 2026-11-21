// TCP のサーバーの例: 受け取ったものを大文字にして送り返す（Ctrl-C で終わる）
//   cc -o tcp_echo samples/tcp_echo.c && ./tcp_echo [PORT]       （既定は 7000）
// Pi 5 から: nc 192.168.7.2 7000
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <monocs.h>

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 7000;
    int l = monocs_tcp_listen(port);
    if (l < 0) {
        printf("cannot listen on port %d\n", port);
        return 1;
    }
    printf("listening on 192.168.7.2:%d (Ctrl-C to stop)\n", port);
    for (;;) {
        int c = monocs_tcp_accept(l, -1);
        if (c < 0)
            continue;
        printf("connected\n");
        static const char hello[] = "hello from Monocs\n";
        monocs_tcp_send(c, hello, sizeof(hello) - 1, -1);   // 長さは文字列から（名前を変えても合うように）
        char buf[1024];
        int n;
        while ((n = monocs_tcp_recv(c, buf, sizeof(buf), -1)) > 0) {
            for (int i = 0; i < n; i++)
                buf[i] = (char)toupper((unsigned char)buf[i]);
            monocs_tcp_send(c, buf, n, -1);
        }
        monocs_tcp_close(c);
        printf("disconnected\n");
    }
}
