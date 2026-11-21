// ソケット・名前・select・poll・alarm のうち、ネットワークの相手がなくても確かめられるもの（tools/test/netapi.sh）。
// 同じソースを Linux（gcc と glibc）と Monocs（QEMU）で動かして、出力を比べる。番号が環境で変わるもの（空いている
// ポートの番号、fd の番号）は出さない
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *ename(int e)
{
    switch (e) {
    case 0: return "0";
    case EAGAIN: return "EAGAIN";
    case EINPROGRESS: return "EINPROGRESS";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
    case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
    case ENOTCONN: return "ENOTCONN";
    case EDESTADDRREQ: return "EDESTADDRREQ";
    case ENOTSOCK: return "ENOTSOCK";
    case EBADF: return "EBADF";
    case EINVAL: return "EINVAL";
    case EINTR: return "EINTR";
    case ENOPROTOOPT: return "ENOPROTOOPT";
    case EPIPE: return "EPIPE";
    case ESPIPE: return "ESPIPE";
    default: return "other";
    }
}

#define CHECK(label, expr) do { errno = 0; long r_ = (long)(expr); printf("%s: %s %s\n", label, r_ < 0 ? "-1" : r_ == 0 ? "0" : "+", ename(r_ < 0 ? errno : 0)); } while (0)

static volatile sig_atomic_t alarmed;
static void on_alarm(int s) { (void)s; alarmed = 1; }
static volatile sig_atomic_t alarmed2;
static void on_alarm2(int s) { (void)s; alarmed2 = 1; }

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    // ---- アドレスの文字列 ----
    struct in_addr a;
    printf("pton %d %d %d %d\n", inet_pton(AF_INET, "192.168.7.2", &a), inet_pton(AF_INET, "1.2.3", &a),
           inet_pton(AF_INET, "256.1.1.1", &a), inet_pton(AF_INET, "01.2.3.4", &a));
    inet_pton(AF_INET, "10.20.30.40", &a);
    char buf[64];
    printf("ntop %s ntoa %s\n", inet_ntop(AF_INET, &a, buf, sizeof buf), inet_ntoa(a));
    printf("aton %d %08x %d %08x %d\n", inet_aton("127.1", &a), ntohl(a.s_addr), inet_aton("0x7f.0.0.1", &a),
           ntohl(a.s_addr), inet_aton("1.2.3.4.5", &a));
    printf("inet_addr %08x none %d\n", ntohl(inet_addr("192.168.1.1")), inet_addr("bad") == INADDR_NONE);
    printf("htons %04x ntohl %08x\n", htons(0x1234), ntohl(0x11223344));

    // ---- getaddrinfo ----
    struct addrinfo hints = {0}, *res, *p;
    hints.ai_family = AF_UNSPEC;
    int e = getaddrinfo("192.168.7.1", "80", &hints, &res);
    printf("gai numeric %d:", e);
    for (p = res; !e && p; p = p->ai_next) {
        struct sockaddr_in *in = (struct sockaddr_in *)p->ai_addr;
        printf(" [%d %d %d %s:%d len %u]", p->ai_family, p->ai_socktype, p->ai_protocol,
               inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf), ntohs(in->sin_port), (unsigned)p->ai_addrlen);
    }
    printf("\n");
    if (!e)
        freeaddrinfo(res);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;  // AF_UNSPEC だと glibc は IPv6 の :: も返す
    e = getaddrinfo(NULL, "http", &hints, &res);
    if (!e) {
        struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
        printf("gai passive %s:%d next %d\n", inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf), ntohs(in->sin_port),
               res->ai_next != NULL);
        freeaddrinfo(res);
    }
    hints.ai_flags = 0;
    e = getaddrinfo(NULL, "8080", &hints, &res);
    if (!e) {
        struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
        printf("gai loopback %s\n", inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf));
        freeaddrinfo(res);
    }
    hints.ai_flags = AI_NUMERICHOST;
    printf("gai errors %d %d", getaddrinfo("not-a-number", "80", &hints, &res), getaddrinfo(NULL, NULL, &hints, &res));
    hints.ai_flags = AI_NUMERICSERV;
    printf(" %d", getaddrinfo("1.2.3.4", "http", &hints, &res));
    hints.ai_flags = 0;
    printf(" %d", getaddrinfo("1.2.3.4", "no-such-service-xyz", &hints, &res));
    e = getaddrinfo("1.2.3.4", "99999", &hints, &res);
    printf(" %d:%d", e, e ? -1 : ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port));
    hints.ai_family = AF_INET6;
    printf(" %s", getaddrinfo("1.2.3.4", "80", &hints, &res) != 0 ? "v6-fails" : "v6-ok");
    printf("\n");
    printf("gai_strerror %s / %s\n", gai_strerror(EAI_NONAME), gai_strerror(EAI_SERVICE));

    // ---- getnameinfo ----
    struct sockaddr_in sin = {.sin_family = AF_INET, .sin_port = htons(443)};
    inet_pton(AF_INET, "192.168.7.2", &sin.sin_addr);
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    e = getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST);
    printf("getnameinfo %d %s %s\n", e, host, serv);
    e = getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, serv, sizeof serv,
                    NI_NUMERICHOST | NI_NUMERICSERV);
    printf("getnameinfo numeric %d %s %s\n", e, host, serv);

    // ---- ソケット ----
    CHECK("socket bad proto", socket(AF_INET, SOCK_STREAM, IPPROTO_UDP));
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    printf("socket tcp %s\n", ls >= 3 ? "ok" : "fail");
    int one = 1;
    CHECK("setsockopt reuseaddr", setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));
    CHECK("setsockopt nodelay", setsockopt(ls, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one));
    CHECK("setsockopt bogus", setsockopt(ls, SOL_SOCKET, 12345, &one, sizeof one));
    int v = 0;
    socklen_t vl = sizeof v;
    getsockopt(ls, SOL_SOCKET, SO_TYPE, &v, &vl);
    printf("so_type %d reuse ", v);
    vl = sizeof v;
    getsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &v, &vl);
    printf("%d\n", v != 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(0), .sin_addr.s_addr = htonl(INADDR_ANY)};
    CHECK("bind", bind(ls, (struct sockaddr *)&addr, sizeof addr));
    CHECK("listen", listen(ls, 4));
    struct sockaddr_in got;
    socklen_t gl = sizeof got;
    getsockname(ls, (struct sockaddr *)&got, &gl);
    printf("getsockname family %d port>0 %d len %u\n", got.sin_family, ntohs(got.sin_port) > 0, (unsigned)gl);
    vl = sizeof v;
    getsockopt(ls, SOL_SOCKET, SO_ACCEPTCONN, &v, &vl);
    printf("acceptconn %d\n", v);
    int fl = fcntl(ls, F_GETFL);
    printf("nonblock before %d\n", (fl & O_NONBLOCK) != 0);
    fcntl(ls, F_SETFL, fl | O_NONBLOCK);
    printf("nonblock after %d\n", (fcntl(ls, F_GETFL) & O_NONBLOCK) != 0);
    CHECK("accept nonblock", accept(ls, NULL, NULL));
    fcntl(ls, F_SETFL, fl);     // 待つ形に戻す
    // SA_RESTART のない SIGALRM で、待っていた accept が EINTR で戻る（Linux と同じ）
    struct sigaction sa2 = {0};
    sa2.sa_handler = on_alarm2;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGALRM, &sa2, NULL);
    alarm(1);
    CHECK("accept eintr", accept(ls, NULL, NULL));
    printf("accept eintr handler %d\n", (int)alarmed2);
    // accept4 の SOCK_NONBLOCK（待ち受けは待つ形のまま。相手がいないので、時間切れの代わりに MSG_DONTWAIT のような
    // 戻り方は見ない）: 待ち受けを待たない形にして accept4
    fcntl(ls, F_SETFL, fl | O_NONBLOCK);
    CHECK("accept4 nonblock", accept4(ls, NULL, NULL, SOCK_NONBLOCK));
    CHECK("getpeername listen", getpeername(ls, (struct sockaddr *)&got, &gl));
    struct stat_like { int dummy; };
    CHECK("lseek socket", lseek(ls, 0, SEEK_SET));

    // select・poll（相手がいないので時間切れ）
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    struct timeval tv = {0, 200000};
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int sr = select(ls + 1, &rf, NULL, NULL, &tv);
    gettimeofday(&t1, NULL);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
    printf("select timeout %d isset %d waited~200 %d left %ld\n", sr, FD_ISSET(ls, &rf), ms >= 180 && ms < 400,
           tv.tv_sec * 1000000 + tv.tv_usec);
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(1, &wf);
    tv = (struct timeval){1, 0};
    sr = select(ls + 1, &rf, &wf, NULL, &tv);
    printf("select stdout %d w1 %d rls %d\n", sr, FD_ISSET(1, &wf), FD_ISSET(ls, &rf));
    FD_ZERO(&rf);
    FD_SET(55, &rf);
    tv = (struct timeval){0, 0};
    CHECK("select badfd", select(56, &rf, NULL, NULL, &tv));
    struct timespec pts = {0, 100000000};
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    sigset_t pmask;
    sigemptyset(&pmask);
    CHECK("pselect timeout", pselect(ls + 1, &rf, NULL, NULL, &pts, &pmask));
    struct pollfd pf[3] = {{ls, POLLIN, 0}, {1, POLLOUT, 0}, {57, POLLIN, 0}};
    int pr = poll(pf, 3, 100);
    printf("poll %d revents %x %x %x\n", pr, pf[0].revents, pf[1].revents, pf[2].revents);
    close(ls);
    CHECK("close again", close(ls));

    // つないでいない TCP
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    CHECK("send unconnected", send(cs, "x", 1, MSG_NOSIGNAL));
    CHECK("recv unconnected", recv(cs, buf, 1, 0));
    CHECK("getpeername unconnected", getpeername(cs, (struct sockaddr *)&got, &gl));
    struct pollfd cp = {cs, POLLIN | POLLOUT, 0};
    poll(&cp, 1, 0);
    printf("poll unconnected %x\n", cp.revents);
    struct sockaddr_in lo = {.sin_family = AF_INET, .sin_port = htons(1)};
    inet_pton(AF_INET, "127.0.0.1", &lo.sin_addr);
    CHECK("connect loopback refused", connect(cs, (struct sockaddr *)&lo, sizeof lo));
    close(cs);

    // UDP
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    printf("socket udp %s\n", us >= 3 ? "ok" : "fail");
    addr.sin_port = htons(0);
    CHECK("bind udp", bind(us, (struct sockaddr *)&addr, sizeof addr));
    CHECK("send udp no dest", send(us, "x", 1, 0));
    gl = sizeof got;
    getsockname(us, (struct sockaddr *)&got, &gl);
    printf("udp port>0 %d\n", ntohs(got.sin_port) > 0);
    CHECK("recvfrom nonblock", recvfrom(us, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL));
    struct pollfd up = {us, POLLIN | POLLOUT, 0};
    poll(&up, 1, 0);
    printf("poll udp %x\n", up.revents);
    close(us);

    // ソケットでない fd
    CHECK("send on stdout", send(1, "", 0, 0));
    CHECK("getsockopt bad fd", getsockopt(99, SOL_SOCKET, SO_TYPE, &v, &vl));

    // alarm
    signal(SIGALRM, on_alarm);
    printf("alarm first %u\n", alarm(1));
    printf("alarm again %u\n", alarm(1));
    pause();
    printf("alarmed %d\n", (int)alarmed);
    alarm(5);
    printf("alarm cancel %u\n", alarm(0) > 0);
    char hn[64];
    printf("gethostname %d\n", gethostname(hn, sizeof hn) == 0 && hn[0]);
    return 0;
}
