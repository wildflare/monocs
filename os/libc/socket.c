// ソケット（BSD ソケットの API。IPv4 の TCP・UDP。2026-11-14）。Linux のネットワークのプログラムを、ソースのまま
// 動かすため。中身は Monocs の TCP・UDP の API（os/net/tcp_api.c・udp_api.c）の上にかぶせたもの。
//   - ソケットは fd（ファイルと同じ番号の並び。os/fs/vfs.h の VFS_MAX_FILES）。read・write・close・fcntl・ioctl・select・poll でも使える
//   - bind(INADDR_ANY) は、すべての口（USB と Wi-Fi）から受ける（Linux と同じ。ユーザーと合意）
//   - 待つ操作（accept・connect・recv・send など）は、待たずに試す → 次の割り込みまで眠る、を繰り返す。
//     O_NONBLOCK・MSG_DONTWAIT なら待たずに EAGAIN（connect は EINPROGRESS）。SO_RCVTIMEO・SO_SNDTIMEO で時間切れ
//     （EAGAIN）。待つ間に SA_RESTART のないシグナルの関数が呼ばれたら EINTR（Linux と同じ。signal() の関数は
//     SA_RESTART なので、続けて待つ）
//   - つながっていない TCP に送ると EPIPE と SIGPIPE（MSG_NOSIGNAL がなければ。Linux と同じ）
//   - UDP は、bind か最初の送り・受けで口を開く（bind しなければ空いている番号）。connect すると送り先を覚える
//   - アプリが閉じ忘れたソケットは、アプリの終わりに閉じる（socket_close_owner。サービスのものはサービスを止めたとき）
// ないもの: IPv6、AF_UNIX、SOCK_RAW、マルチキャスト、sendmsg・recvmsg、dup。127.0.0.1（ループバック）もない

#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "../api/include/sys/socket.h"
#include "../api/include/netinet/in.h"
#include "../api/include/netinet/tcp.h"
#include "socket_os.h"
#include "../fs/vfs.h"
#include "signal_os.h"
#include "../drivers/timer.h"
#include "../kernel/svc.h"
#include "../net/tcp_api.h"
#include "../net/udp_api.h"

enum { S_NEW, S_LISTEN, S_CONNECTING, S_CONNECTED, S_FAILED };

typedef struct {
    bool used;
    int owner;              // 開いたもの（svc_owner。0 = アプリ、負 = 入れ子のアプリ、1〜 = サービス）
    int type;               // SOCK_STREAM・SOCK_DGRAM
    int h;                  // TCP・UDP の API のハンドル（まだなければ -1）
    int state;              // TCP（S_*）
    bool nonblock;
    int rcvtimeo, sndtimeo; // ミリ秒（負 = いつまでも）
    bool bound;
    char bind_ip[16];       // "" = どこでも（INADDR_ANY）
    int bind_port;
    bool has_peer;          // UDP の connect
    char peer_ip[16];
    int peer_port;
    int so_error;           // SO_ERROR で返すもの（つながらなかったとき）
    bool shut_rd;
    int reuseaddr, keepalive, broadcast;    // 受け取って返すだけのオプション
    int fd;
} Sock;

static Sock socks[SOCKET_MAX];
static int8_t sock_at[VFS_MAX_FILES];   // fd → socks の番号 + 1（0 = ソケットでない）
static atomic_flag table_lock = ATOMIC_FLAG_INIT;

static void lock_table(void)
{
    while (atomic_flag_test_and_set_explicit(&table_lock, memory_order_acquire))
        __asm__ volatile("yield");
}

static void unlock_table(void)
{
    atomic_flag_clear_explicit(&table_lock, memory_order_release);
}

bool socket_is_fd(int fd)
{
    return fd >= 0 && fd < VFS_MAX_FILES && sock_at[fd] && socks[sock_at[fd] - 1].used;
}

static Sock *sock_of(int fd)
{
    return &socks[sock_at[fd] - 1];
}

// fd のソケット。違えば errno を EBADF（fd が開いていない）か ENOTSOCK（ソケットでない）にして NULL
static Sock *get(int fd)
{
    if (socket_is_fd(fd))
        return sock_of(fd);
    errno = (fd >= 0 && fd < 3) || vfs_fd_open(fd) ? ENOTSOCK : EBADF;
    return nullptr;
}

static int fail(int e)
{
    errno = e;
    return -1;
}

// ---- 待つ ----

// 待つ形の操作の時間切れ（ミリ秒。0 = 待たない、負 = いつまでも）
static int wait_ms(const Sock *s, int flags, bool sending)
{
    if (s->nonblock || (flags & MSG_DONTWAIT))
        return 0;
    return sending ? s->sndtimeo : s->rcvtimeo;
}

typedef struct {
    uint64_t start;
    int timeout_ms;
    uint32_t sigs;
} Wait;

static Wait wait_begin(int timeout_ms)
{
    return (Wait){timer_now_us(), timeout_ms, signal_delivered_norestart()};
}

// まだ待つなら 0（次の割り込みまで眠ってから戻る）。時間切れなら EAGAIN、シグナルなら EINTR
static int wait_more(Wait *w)
{
    if (w->timeout_ms == 0)
        return EAGAIN;
    if (w->timeout_ms > 0 && timer_now_us() - w->start >= (uint64_t)w->timeout_ms * 1000)
        return EAGAIN;
    cpu_idle();     // 次の割り込み（受信・タイマー）まで眠る。シグナルの関数もここで呼ばれる（signal_point）
    if (signal_delivered_norestart() != w->sigs)
        return EINTR;
    return 0;
}

// ---- アドレス ----

static bool get_in(const struct sockaddr *addr, socklen_t len, char ip[16], int *port)
{
    if (!addr || len < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET)
        return false;
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    uint32_t a = ntohl(in->sin_addr.s_addr);
    snprintf(ip, 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    *port = ntohs(in->sin_port);
    return true;
}

// ip（"1.2.3.4"。"" は 0.0.0.0）と port を addr に（*len は書ける大きさ。切り詰めて、本当の大きさを返す。POSIX と同じ）
static void put_in(struct sockaddr *addr, socklen_t *len, const char *ip, int port)
{
    if (!addr || !len)
        return;
    struct sockaddr_in in = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (ip && *ip)
        sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d);
    in.sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
    memcpy(addr, &in, *len < sizeof in ? *len : sizeof in);
    *len = sizeof in;
}

// ---- ソケットを作る・閉じる ----

static int alloc_sock(int type)
{
    int fd = vfs_fd_take_socket();
    if (fd < 0)
        return -1;
    lock_table();
    for (int i = 0; i < SOCKET_MAX; i++)
        if (!socks[i].used) {
            socks[i] = (Sock){.used = true, .owner = svc_owner(), .type = type, .h = -1, .rcvtimeo = -1,
                              .sndtimeo = -1, .fd = fd};
            sock_at[fd] = (int8_t)(i + 1);
            unlock_table();
            return fd;
        }
    unlock_table();
    vfs_fd_put_socket(fd);
    return -1;
}

int socket(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return fail(EAFNOSUPPORT);
    int base = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (base != SOCK_STREAM && base != SOCK_DGRAM)
        return fail(base == SOCK_RAW ? EPERM : EPROTONOSUPPORT);
    if (protocol && protocol != (base == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))
        return fail(EPROTONOSUPPORT);
    int fd = alloc_sock(base);
    if (fd < 0)
        return fail(EMFILE);
    sock_of(fd)->nonblock = type & SOCK_NONBLOCK;
    return fd;
}

int socket_close(int fd)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (s->h >= 0) {
        if (s->type == SOCK_STREAM)
            tcp_api_close(s->h);
        else
            udp_api_close(s->h);
    }
    lock_table();
    s->used = false;
    sock_at[fd] = 0;
    unlock_table();
    vfs_fd_put_socket(fd);
    return 0;
}

void socket_close_owner(int owner)
{
    for (int i = 0; i < SOCKET_MAX; i++)
        if (socks[i].used && socks[i].owner == owner)
            socket_close(socks[i].fd);
}

// UDP の口を開く（bind したアドレスか、どこでも・空いている番号）
static bool udp_open(Sock *s)
{
    if (s->h >= 0)
        return true;
    s->h = udp_api_open_addr(s->bound && s->bind_ip[0] ? s->bind_ip : nullptr, s->bound ? s->bind_port : 0);
    return s->h >= 0;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    char ip[16];
    int port;
    if (!get_in(addr, len, ip, &port))
        return fail(addr && addr->sa_family != AF_INET ? EAFNOSUPPORT : EINVAL);
    if (s->bound || s->h >= 0)
        return fail(EINVAL);
    s->bound = true;
    snprintf(s->bind_ip, sizeof s->bind_ip, "%s", strcmp(ip, "0.0.0.0") ? ip : "");
    s->bind_port = port;
    if (s->type == SOCK_DGRAM && !udp_open(s)) {
        s->bound = false;
        return fail(EADDRINUSE);
    }
    return 0;
}

int listen(int fd, int backlog)
{
    (void)backlog;      // いつも 4（os/net/tcp_api.c の BACKLOG）
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (s->type != SOCK_STREAM)
        return fail(EOPNOTSUPP);
    if (s->state == S_LISTEN)
        return 0;
    if (s->state != S_NEW)
        return fail(EINVAL);
    s->h = tcp_api_listen_addr(s->bind_ip[0] ? s->bind_ip : nullptr, s->bound ? s->bind_port : 0);
    if (s->h < 0)
        return fail(EADDRINUSE);
    s->state = S_LISTEN;
    return 0;
}

int accept4(int fd, struct sockaddr *restrict addr, socklen_t *restrict len, int flags)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (s->state != S_LISTEN)
        return fail(EINVAL);
    Wait w = wait_begin(wait_ms(s, 0, false));
    for (;;) {
        int h = tcp_api_accept(s->h, 0);
        if (h >= 0) {
            int nfd = alloc_sock(SOCK_STREAM);
            if (nfd < 0) {
                tcp_api_close(h);
                return fail(EMFILE);
            }
            Sock *n = sock_of(nfd);
            n->h = h;
            n->state = S_CONNECTED;
            n->nonblock = flags & SOCK_NONBLOCK;
            char ip[16] = "";
            int port = 0;
            tcp_api_peer(h, ip, &port);
            put_in(addr, len, ip, port);
            return nfd;
        }
        int e = wait_more(&w);
        if (e)
            return fail(e);
    }
}

int accept(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    return accept4(fd, addr, len, 0);
}

// つないでいる途中の TCP の様子を進める（つながった・つながらなかった）
static void update_connecting(Sock *s)
{
    if (s->state != S_CONNECTING)
        return;
    int st = tcp_api_connect_state(s->h);
    if (st == 1) {
        s->state = S_CONNECTED;
    } else if (st < 0) {
        s->so_error = tcp_api_refused(s->h) ? ECONNREFUSED : ETIMEDOUT;
        tcp_api_close(s->h);
        s->h = -1;
        s->state = S_FAILED;
    }
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (s->type == SOCK_DGRAM) {
        if (addr && addr->sa_family == AF_UNSPEC) {     // 送り先を忘れる
            s->has_peer = false;
            return 0;
        }
        if (!get_in(addr, len, s->peer_ip, &s->peer_port))
            return fail(EAFNOSUPPORT);
        s->has_peer = true;
        return udp_open(s) ? 0 : fail(EADDRINUSE);
    }
    switch (s->state) {
    case S_CONNECTED:
        return fail(EISCONN);
    case S_LISTEN:
        return fail(EINVAL);
    case S_CONNECTING:
        update_connecting(s);
        if (s->state == S_CONNECTING)
            return fail(EALREADY);
        if (s->state == S_CONNECTED)
            return fail(EISCONN);
        return fail(s->so_error);
    default:
        break;
    }
    char ip[16];
    int port;
    if (!get_in(addr, len, ip, &port))
        return fail(addr && addr->sa_family != AF_INET ? EAFNOSUPPORT : EINVAL);
    if (!strcmp(ip, "127.0.0.1"))
        return fail(ECONNREFUSED);  // ループバックはない
    s->h = tcp_api_connect_start(ip, port);
    if (s->h < 0)
        return fail(ENETUNREACH);
    s->state = S_CONNECTING;
    s->so_error = 0;
    Wait w = wait_begin(wait_ms(s, 0, true));
    for (;;) {
        update_connecting(s);
        if (s->state == S_CONNECTED)
            return 0;
        if (s->state == S_FAILED) {
            int e = s->so_error;
            s->so_error = 0;
            return fail(e);
        }
        int e = wait_more(&w);
        if (e == EAGAIN)
            return fail(w.timeout_ms == 0 ? EINPROGRESS : ETIMEDOUT);  // 待たない形: つないでいる途中
        if (e)
            return fail(e);
    }
}

// ---- 送る・受け取る ----

static ssize_t tcp_send(Sock *s, const void *buf, size_t len, int flags)
{
    update_connecting(s);
    if (s->state == S_CONNECTING)
        return fail(EAGAIN);
    if (s->state != S_CONNECTED) {
        if (!(flags & MSG_NOSIGNAL))
            raise(SIGPIPE);
        return fail(EPIPE);     // つないでいない TCP でも EPIPE（Linux と同じ）
    }
    if (len > 0x7fffffff)
        len = 0x7fffffff;
    const char *p = buf;
    size_t sent = 0;
    Wait w = wait_begin(wait_ms(s, flags, true));
    while (sent < len) {
        int n = tcp_api_send(s->h, p + sent, (int)(len - sent), 0);
        if (n < 0) {
            if (sent)
                break;
            if (!(flags & MSG_NOSIGNAL))
                raise(SIGPIPE);
            return fail(EPIPE);
        }
        sent += (size_t)n;
        if (sent == len)
            break;
        int e = wait_more(&w);
        if (e) {
            if (sent)
                break;
            return fail(e);
        }
    }
    return (ssize_t)sent;
}

static ssize_t tcp_recv(Sock *s, void *buf, size_t len, int flags)
{
    update_connecting(s);
    if (s->state != S_CONNECTED)
        return fail(s->state == S_CONNECTING ? EAGAIN : ENOTCONN);
    if (s->shut_rd || len == 0)
        return 0;
    if (len > 0x7fffffff)
        len = 0x7fffffff;
    char *p = buf;
    size_t got = 0;
    Wait w = wait_begin(wait_ms(s, flags, false));
    for (;;) {
        int n = (flags & MSG_PEEK) ? tcp_api_recv_peek(s->h, p + got, (int)(len - got), 0)
                                   : tcp_api_recv(s->h, p + got, (int)(len - got), 0);
        if (n > 0) {
            got += (size_t)n;
            if (!(flags & MSG_WAITALL) || (flags & MSG_PEEK) || got == len)
                return (ssize_t)got;
            continue;
        }
        if (n == 0)
            return (ssize_t)got;    // 相手が閉じた
        if (n == -2)
            return got ? (ssize_t)got : fail(ECONNRESET);
        int e = wait_more(&w);
        if (e)
            return got ? (ssize_t)got : fail(e);
    }
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (s->type == SOCK_STREAM)
        return tcp_send(s, buf, len, flags);    // つながった TCP では、宛先は使わない
    char ip[16];
    int port;
    if (to) {
        if (!get_in(to, tolen, ip, &port))
            return fail(EAFNOSUPPORT);
    } else if (s->has_peer) {
        memcpy(ip, s->peer_ip, sizeof ip);
        port = s->peer_port;
    } else {
        return fail(EDESTADDRREQ);
    }
    if (len > 1472)
        return fail(EMSGSIZE);  // 1 つのデータグラム（os/net/udp_api.c の UDP_MAX）
    if (!udp_open(s))
        return fail(ENOBUFS);
    int n = udp_api_sendto(s->h, ip, port, buf, (int)len);
    return n < 0 ? fail(ENETUNREACH) : n;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, nullptr, 0);
}

ssize_t recvfrom(int fd, void *restrict buf, size_t len, int flags, struct sockaddr *restrict from,
                 socklen_t *restrict fromlen)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (s->type == SOCK_STREAM) {
        ssize_t n = tcp_recv(s, buf, len, flags);
        if (n >= 0 && from) {
            char ip[16] = "";
            int port = 0;
            tcp_api_peer(s->h, ip, &port);
            put_in(from, fromlen, ip, port);
        }
        return n;
    }
    if (!udp_open(s))
        return fail(ENOBUFS);
    if (flags & MSG_PEEK)
        return fail(EOPNOTSUPP);
    Wait w = wait_begin(wait_ms(s, flags, false));
    for (;;) {
        char ip[16];
        int port;
        int n = udp_api_recvfrom(s->h, buf, len > 0x7fffffff ? 0x7fffffff : (int)len, ip, &port, 0);
        if (n >= 0) {
            put_in(from, fromlen, ip, port);
            return n;
        }
        if (n == -2)
            return fail(EBADF);
        int e = wait_more(&w);
        if (e)
            return fail(e);
    }
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, nullptr, nullptr);
}

ssize_t socket_read(int fd, void *buf, size_t len)
{
    return recv(fd, buf, len, 0);
}

ssize_t socket_write(int fd, const void *buf, size_t len)
{
    return send(fd, buf, len, 0);
}

int shutdown(int fd, int how)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (how < SHUT_RD || how > SHUT_RDWR)
        return fail(EINVAL);
    if (s->type == SOCK_STREAM && s->state != S_CONNECTED)
        return fail(ENOTCONN);
    if (how != SHUT_WR)
        s->shut_rd = true;
    if (how != SHUT_RD && s->type == SOCK_STREAM && tcp_api_shutdown_write(s->h) != 0)
        return fail(ENOTCONN);
    return 0;
}

// ---- アドレスとオプション ----

int getsockname(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    char ip[16] = "";
    int port = 0;
    bool ok = s->h >= 0 && (s->type == SOCK_STREAM ? tcp_api_local(s->h, ip, &port) : udp_api_local(s->h, ip, &port)) == 0;
    if (!ok && s->bound) {
        snprintf(ip, sizeof ip, "%s", s->bind_ip);
        port = s->bind_port;
    }
    put_in(addr, len, ip, port);
    return 0;
}

int getpeername(int fd, struct sockaddr *restrict addr, socklen_t *restrict len)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    char ip[16] = "";
    int port = 0;
    if (s->type == SOCK_DGRAM) {
        if (!s->has_peer)
            return fail(ENOTCONN);
        snprintf(ip, sizeof ip, "%s", s->peer_ip);
        port = s->peer_port;
    } else {
        update_connecting(s);
        if (s->state != S_CONNECTED || tcp_api_peer(s->h, ip, &port) != 0)
            return fail(ENOTCONN);
    }
    put_in(addr, len, ip, port);
    return 0;
}

static int timeval_ms(const void *val, socklen_t len)
{
    if (len < sizeof(struct timeval))
        return -2;
    const struct timeval *tv = val;
    if (tv->tv_sec == 0 && tv->tv_usec == 0)
        return -1;      // 0 は「いつまでも」（Linux と同じ）
    long long ms = (long long)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}

static int int_opt(const void *val, socklen_t len)
{
    return val && len >= sizeof(int) ? *(const int *)val : 0;
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            int ms = timeval_ms(val, len);
            if (ms == -2)
                return fail(EINVAL);
            if (name == SO_RCVTIMEO)
                s->rcvtimeo = ms;
            else
                s->sndtimeo = ms;
            return 0;
        }
        case SO_REUSEADDR: s->reuseaddr = int_opt(val, len) != 0; return 0;    // いつも再利用できる
        case SO_KEEPALIVE: s->keepalive = int_opt(val, len) != 0; return 0;
        case SO_BROADCAST: s->broadcast = int_opt(val, len) != 0; return 0;
        case SO_REUSEPORT: case SO_LINGER: case SO_RCVBUF: case SO_SNDBUF: case SO_DEBUG:
        case SO_OOBINLINE: case SO_DONTROUTE: case SO_RCVLOWAT: case SO_SNDLOWAT:
            return 0;   // 受け取るだけ
        default:
            return fail(ENOPROTOOPT);
        }
    }
    if (level == IPPROTO_TCP && s->type == SOCK_STREAM) {
        switch (name) {
        case TCP_NODELAY: case TCP_MAXSEG: case TCP_CORK: case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT:
            return 0;   // Monocs の TCP はいつも Nagle を使わない（TCP_NODELAY と同じ）
        default:
            return fail(ENOPROTOOPT);
        }
    }
    if (level == IPPROTO_IP && (name == IP_TOS || name == IP_TTL))
        return 0;
    return fail(ENOPROTOOPT);   // マルチキャストなど
}

int getsockopt(int fd, int level, int name, void *restrict val, socklen_t *restrict len)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (!val || !len)
        return fail(EFAULT);
    int v;
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        int ms = name == SO_RCVTIMEO ? s->rcvtimeo : s->sndtimeo;
        struct timeval tv = {0};
        if (ms > 0)
            tv = (struct timeval){ms / 1000, (ms % 1000) * 1000};
        memcpy(val, &tv, *len < sizeof tv ? *len : sizeof tv);
        *len = sizeof tv;
        return 0;
    }
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_ERROR:
            update_connecting(s);
            v = s->so_error;
            s->so_error = 0;
            break;
        case SO_TYPE: v = s->type; break;
        case SO_ACCEPTCONN: v = s->state == S_LISTEN; break;
        case SO_REUSEADDR: v = s->reuseaddr; break;
        case SO_KEEPALIVE: v = s->keepalive; break;
        case SO_BROADCAST: v = s->broadcast; break;
        case SO_RCVBUF: case SO_SNDBUF: v = 65536; break;
        default: return fail(ENOPROTOOPT);
        }
    } else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
        v = 1;
    } else {
        return fail(ENOPROTOOPT);
    }
    memcpy(val, &v, *len < sizeof v ? *len : sizeof v);
    *len = sizeof v;
    return 0;
}

// ---- select・poll・fcntl・ioctl から ----

bool socket_ready(int fd, bool *readable, bool *writable, bool *error, bool *hangup)
{
    if (!socket_is_fd(fd))
        return false;
    Sock *s = sock_of(fd);
    *readable = *writable = *error = *hangup = false;
    if (s->type == SOCK_DGRAM) {
        bool rd = false;
        if (s->h >= 0)
            udp_api_ready(s->h, &rd);
        *readable = rd;
        *writable = true;
        return true;
    }
    update_connecting(s);
    switch (s->state) {
    case S_NEW:
        *writable = true;   // Linux はつないでいない TCP も「書ける」と返し、書くと EPIPE
        *hangup = true;
        break;
    case S_FAILED:
        *readable = *writable = *error = *hangup = true;
        break;
    case S_CONNECTING:
        break;
    default: {
        bool rd = false, wr = false;
        if (tcp_api_ready(s->h, &rd, &wr) != 0) {
            *error = *hangup = true;
        } else {
            *readable = rd || s->shut_rd;
            *writable = wr && s->state == S_CONNECTED;
        }
        break;
    }
    }
    return true;
}

int socket_get_flags(int fd)
{
    Sock *s = get(fd);
    return s ? O_RDWR | (s->nonblock ? O_NONBLOCK : 0) : -1;
}

int socket_set_flags(int fd, int flags)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    s->nonblock = flags & O_NONBLOCK;
    return 0;
}

int socket_ioctl(int fd, unsigned long op, void *param)
{
    Sock *s = get(fd);
    if (!s)
        return -1;
    if (op == FIONBIO) {
        s->nonblock = param && *(int *)param;
        return 0;
    }
    if (op == FIONREAD) {
        int n = 0;
        if (s->type == SOCK_STREAM && s->state == S_CONNECTED)
            n = tcp_api_available(s->h);
        else if (s->type == SOCK_DGRAM && s->h >= 0) {
            bool rd = false;
            udp_api_ready(s->h, &rd);
            n = rd ? 1 : 0;     // 次のデータグラムの大きさは調べない（あるかだけ）
        }
        *(int *)param = n < 0 ? 0 : n;
        return 0;
    }
    return fail(ENOTTY);
}
