// Wi-Fi の速さを測る（2026-11-22。tools/perf/wifibench.py が Pi 5 から使う）。
//   wifibench [PORT]   TCP の PORT（既定 5001）で 1 回だけ受け付け、相手の頼みのとおりに送る・受ける。
//   相手が最初に送る 1 行: "send N"（Monocs から N バイト送る）か "recv"（相手が閉じるまで受けて、数を返す）。
//   転送の前に wifi perf reset、後に wifi perf を動かして、時間の内訳を出す。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 5001;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        perror("wifibench");
        return 1;
    }
    printf("wifibench: listening on %d\n", port);
    fflush(stdout);
    int c = accept(ls, NULL, NULL);
    if (c < 0) {
        perror("accept");
        return 1;
    }
    char line[64];
    int n = 0;
    while (n < (int)sizeof line - 1 && recv(c, line + n, 1, 0) == 1 && line[n] != '\n')
        n++;
    line[n] = 0;
    static char buf[16384];
    system("wifi perf reset > /dev/null");
    double t0 = now();
    long long total = 0;
    if (!strncmp(line, "send ", 5)) {
        long long want = atoll(line + 5);
        memset(buf, 'x', sizeof buf);
        while (total < want) {
            int k = want - total < (long long)sizeof buf ? (int)(want - total) : (int)sizeof buf;
            int w = send(c, buf, k, 0);
            if (w <= 0)
                break;
            total += w;
        }
    } else {
        int r;
        while ((r = recv(c, buf, sizeof buf, 0)) > 0)
            total += r;
        char reply[32];
        int k = snprintf(reply, sizeof reply, "%lld\n", total);
        send(c, reply, k, 0);
    }
    double t = now() - t0;
    close(c);
    close(ls);
    printf("wifibench: %s %lld bytes in %.3f s (%.0f KB/s)\n", line, total, t, total / t / 1024);
    fflush(stdout);
    system("wifi perf");
    return 0;
}
