// アプリから使える TCP の API（待つ形。monocs.h の monocs_tcp_*）
//
// lwIP の raw API（コールバック。net_lock の中、主に割り込みから呼ばれる）と、アプリ（普通の文脈。どのコアからも）
// の間をつなぐ。接続ごとに受信バッファ（64KB）を持ち、コールバックはそこに書くだけ。TCP の窓は、アプリが読んだ分
// だけ開ける（tcp_recved）ので、受信バッファは TCP_WND（約 23KB）を超えてあふれない。
// 待つ関数は、条件を調べて少し待つことを繰り返す（lwIP は割り込みで進む）。アプリが終わると（Ctrl-C でも）、
// そのアプリが開いた接続は tcp_api_close_app で閉じる。
// コールバックの中では malloc しない（割り込みが C ライブラリのロックを持つ処理を止めていることがあるため）

#include <stdio.h>
#include <string.h>
#include "tcp_api.h"
#include "net.h"
#include "dns_api.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "../drivers/timer.h"
#include "../kernel/svc.h"

#define MAX_CONNS   16
#define RX_SIZE     (128 * 1024)     // 受信の窓（lwipopts.h の TCP_WND、128KB 近く）が入る大きさ
#define BACKLOG     4

enum { C_FREE, C_LISTENING, C_CONNECTING, C_CONNECTED, C_CLOSED };

typedef struct {
    int state;
    int owner;                      // 開いたもの（0 = アプリ・カーネル、1〜 = サービス。os/kernel/svc.c）
    struct tcp_pcb *pcb;
    // 受信（C_CONNECTED）
    uint8_t *rx;                    // rx_bufs[添字]
    uint32_t rx_head, rx_tail;
    bool peer_closed, error;
    bool refused;                   // 相手に断られた（RST。lwIP の ERR_RST）。ほかのエラーは時間切れなど
    int netif;                      // 受け付けた接続が届いた口（NET_IF_*）
    // 待ち受け（C_LISTENING）: 受け付けたが、まだ accept で渡していない接続
    int backlog[BACKLOG];
    int nbacklog;
} Conn;

static Conn conns[MAX_CONNS];
static uint8_t rx_bufs[MAX_CONNS][RX_SIZE];

static uint32_t rx_used(Conn *c)
{
    return (c->rx_head - c->rx_tail) % RX_SIZE;
}

// net_lock の中で
static int alloc_conn(int owner)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].state == C_FREE) {
            Conn *c = &conns[i];
            memset(c, 0, sizeof(*c));
            c->state = C_CLOSED;
            c->owner = owner;
            c->rx = rx_bufs[i];
            return i;
        }
    return -1;
}

// ---- lwIP のコールバック（net_lock の中） ----

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb;
    (void)err;
    Conn *c = &conns[(int)(intptr_t)arg];
    if (!p) {
        c->peer_closed = true;
        return ERR_OK;
    }
    if (RX_SIZE - 1 - rx_used(c) < p->tot_len)
        return ERR_MEM;     // 窓を超えて届くことはないはずだが、念のため（lwIP があとで渡し直す）
    for (struct pbuf *q = p; q; q = q->next) {
        // 輪の終わりまでと、先頭に回る分の 2 つに分けて写す
        uint32_t first = RX_SIZE - c->rx_head;
        if (first > q->len)
            first = q->len;
        memcpy(c->rx + c->rx_head, q->payload, first);
        memcpy(c->rx, (const uint8_t *)q->payload + first, q->len - first);
        c->rx_head = (c->rx_head + q->len) % RX_SIZE;
    }
    pbuf_free(p);   // 窓は、アプリが読んだときに開ける
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    Conn *c = &conns[(int)(intptr_t)arg];
    c->refused = err == ERR_RST;
    c->pcb = nullptr;   // lwIP が接続を捨てた（pcb はもうない）
    c->error = true;
    c->peer_closed = true;
}

static void attach(int h, struct tcp_pcb *pcb)
{
    Conn *c = &conns[h];
    c->pcb = pcb;
    c->state = C_CONNECTED;
    tcp_arg(pcb, (void *)(intptr_t)h);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    Conn *l = &conns[(int)(intptr_t)arg];
    if (err != ERR_OK || !pcb)
        return ERR_VAL;
    int h = l->nbacklog < BACKLOG ? alloc_conn(l->owner) : -1;
    if (h < 0) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    attach(h, pcb);
    conns[h].netif = net_input_kind();  // 今処理しているパケット（ハンドシェイクの最後の ACK）が届いた口
    l->backlog[l->nbacklog++] = h;
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    Conn *c = &conns[(int)(intptr_t)arg];
    if (err == ERR_OK)
        c->state = C_CONNECTED;
    return ERR_OK;
}

// ---- API ----

static bool valid(int h, int state)
{
    return h >= 0 && h < MAX_CONNS && conns[h].state == state;
}

int tcp_api_listen(int port)
{
    return tcp_api_listen_addr(nullptr, port);
}

// ip のアドレスに届いた接続だけを受ける（NULL ならどこでも。Linux の bind と同じ）。ip が自分の口（USB・Wi-Fi）の
// アドレスなら、その口から届いたものだけにする（tcp_bind_netif。lwIP は、ほかの口のアドレスあてのパケットも
// 受け取るので、アドレスだけでは Wi-Fi の側から USB のアドレスあてに送ったものを受けてしまう）
int tcp_api_listen_addr(const char *ip, int port)
{
    ip_addr_t addr;
    if (ip && !ipaddr_aton(ip, &addr))
        return -1;
    uint64_t daif = net_lock();
    int h = alloc_conn(svc_owner());
    struct tcp_pcb *pcb = h >= 0 ? tcp_new() : nullptr;
    if (pcb)    // 閉じたばかりの接続（TIME_WAIT）が残っていても、同じポートで待ち受けられるように
        ip_set_option(pcb, SOF_REUSEADDR);
    if (pcb && tcp_bind(pcb, ip ? &addr : IP_ADDR_ANY, (u16_t)port) == ERR_OK) {
        struct netif *nif = ip ? net_netif_of_addr(ip_2_ip4(&addr)) : nullptr;
        if (nif)
            tcp_bind_netif(pcb, nif);
        struct tcp_pcb *lpcb = tcp_listen_with_backlog(pcb, BACKLOG);
        if (lpcb) {
            conns[h].pcb = lpcb;
            conns[h].state = C_LISTENING;
            tcp_arg(lpcb, (void *)(intptr_t)h);
            tcp_accept(lpcb, on_accept);
            net_unlock(daif);
            return h;
        }
    }
    if (pcb)
        tcp_close(pcb);
    if (h >= 0)
        conns[h].state = C_FREE;
    net_unlock(daif);
    return -1;
}

// timeout_ms: 0 = 待たない、負 = いつまでも
static bool expired(uint64_t start, int timeout_ms)
{
    return timeout_ms >= 0 && timer_now_us() - start >= (uint64_t)timeout_ms * 1000;
}

int tcp_api_accept(int lh, int timeout_ms)
{
    uint64_t start = timer_now_us();
    for (;;) {
        uint64_t daif = net_lock();
        if (!valid(lh, C_LISTENING)) {
            net_unlock(daif);
            return -1;
        }
        Conn *l = &conns[lh];
        if (l->nbacklog) {
            int h = l->backlog[0];
            memmove(l->backlog, l->backlog + 1, sizeof(int) * --l->nbacklog);
            net_unlock(daif);
            return h;
        }
        net_unlock(daif);
        if (expired(start, timeout_ms))
            return -1;
        cpu_idle(); // 次の割り込み（USB の受信・タイマー）まで眠る
    }
}

// つなぎ始める（待たない。ip は "192.168.7.1" の形）。ハンドルか -1。つながったかは tcp_api_connect_state
static int connect_start(const ip_addr_t *addr, int port)
{
    uint64_t daif = net_lock();
    int h = alloc_conn(svc_owner());
    struct tcp_pcb *pcb = h >= 0 ? tcp_new() : nullptr;
    if (!pcb) {
        if (h >= 0)
            conns[h].state = C_FREE;
        net_unlock(daif);
        return -1;
    }
    attach(h, pcb);
    conns[h].state = C_CONNECTING;
    if (tcp_connect(pcb, addr, (u16_t)port, on_connected) != ERR_OK) {
        tcp_abort(pcb);
        conns[h].state = C_FREE;
        net_unlock(daif);
        return -1;
    }
    net_unlock(daif);
    return h;
}

int tcp_api_connect_start(const char *ip, int port)
{
    ip_addr_t addr;
    if (!ipaddr_aton(ip, &addr))
        return -1;
    return connect_start(&addr, port);
}

// 1 = つながった、0 = つないでいる途中、-1 = つながらなかった（断られた・ハンドルが違う）
int tcp_api_connect_state(int h)
{
    if (h < 0 || h >= MAX_CONNS)
        return -1;
    uint64_t daif = net_lock();
    int st = conns[h].state;
    bool err = conns[h].error;
    net_unlock(daif);
    if (err)
        return -1;
    return st == C_CONNECTED ? 1 : st == C_CONNECTING ? 0 : -1;
}

int tcp_api_connect(const char *ip, int port, int timeout_ms)
{
    ip_addr_t addr;
    uint64_t start = timer_now_us();
    if (dns_api_lookup(ip, &addr, timeout_ms) != 0)    // 名前なら引く（待つのは timeout_ms まで）
        return -1;
    int h = connect_start(&addr, port);
    if (h < 0)
        return -1;
    for (;;) {
        int st = tcp_api_connect_state(h);
        if (st == 1)
            return h;
        if (st < 0 || expired(start, timeout_ms)) {
            tcp_api_close(h);
            return -1;
        }
        cpu_idle();
    }
}

// 待たずに調べる（select・poll。os/libc/select.c）: 読めるか（データ・相手が閉じた・エラー、待ち受けなら受け付けた
// 接続がある）と、書けるか（送信バッファに空きがある・エラー、つないでいる途中なら、つながったか断られた）。
// ハンドルが違えば -1
int tcp_api_ready(int h, bool *readable, bool *writable)
{
    if (h < 0 || h >= MAX_CONNS)
        return -1;
    uint64_t daif = net_lock();
    Conn *c = &conns[h];
    int r = 0;
    switch (c->state) {
    case C_LISTENING:
        *readable = c->nbacklog > 0;
        *writable = false;
        break;
    case C_CONNECTING:
        *readable = c->error;
        *writable = c->error;
        break;
    case C_CONNECTED:
        *readable = rx_used(c) > 0 || c->peer_closed || c->error;
        *writable = c->error || !c->pcb ||
                    (tcp_sndbuf(c->pcb) > 0 && tcp_sndqueuelen(c->pcb) < TCP_SND_QUEUELEN - 4);
        break;
    default:
        r = -1;
    }
    net_unlock(daif);
    return r;
}

// エラーで終わった接続が、相手に断られた（RST）のか。connect の errno（ECONNREFUSED か ETIMEDOUT）のため
bool tcp_api_refused(int h)
{
    return h >= 0 && h < MAX_CONNS && conns[h].refused;
}

// 受信バッファにあって、まだ読んでいないバイト数（ioctl の FIONREAD）。ハンドルが違えば -1
int tcp_api_available(int h)
{
    uint64_t daif = net_lock();
    int n = valid(h, C_CONNECTED) ? (int)rx_used(&conns[h]) : -1;
    net_unlock(daif);
    return n;
}

// 自分のアドレス（ip は 16 バイト以上。NULL 可）とポート（NULL 可）。0 で成功
int tcp_api_local(int h, char *ip, int *port)
{
    uint64_t daif = net_lock();
    bool ok = h >= 0 && h < MAX_CONNS && conns[h].state != C_FREE && conns[h].pcb;
    if (ok) {
        if (ip)
            ipaddr_ntoa_r(&conns[h].pcb->local_ip, ip, 16);
        if (port)
            *port = conns[h].pcb->local_port;
    }
    net_unlock(daif);
    return ok ? 0 : -1;
}

// 送る側だけ閉じる（shutdown(SHUT_WR)。相手には終わりが届き、受け取りは続けられる）。0 で成功
int tcp_api_shutdown_write(int h)
{
    uint64_t daif = net_lock();
    bool ok = valid(h, C_CONNECTED) && conns[h].pcb && tcp_shutdown(conns[h].pcb, 0, 1) == ERR_OK;
    net_unlock(daif);
    return ok ? 0 : -1;
}

// 受け付けた接続が届いた口（NET_IF_USB・NET_IF_WIFI）。ハンドルが違うときは -1（自分からつないだものは意味がない）
int tcp_api_netif(int h)
{
    uint64_t daif = net_lock();
    int r = valid(h, C_CONNECTED) ? conns[h].netif : -1;
    net_unlock(daif);
    return r;
}

// 相手のアドレス（ip は 16 バイト以上。NULL 可）とポート（NULL 可）。0 で成功
int tcp_api_peer(int h, char *ip, int *port)
{
    uint64_t daif = net_lock();
    bool ok = valid(h, C_CONNECTED) && conns[h].pcb;
    if (ok) {
        if (ip)
            ipaddr_ntoa_r(&conns[h].pcb->remote_ip, ip, 16);
        if (port)
            *port = conns[h].pcb->remote_port;
    }
    net_unlock(daif);
    return ok ? 0 : -1;
}

int tcp_api_send(int h, const void *buf, int len, int timeout_ms)
{
    const uint8_t *p = buf;
    int sent = 0;
    uint64_t start = timer_now_us();
    while (sent < len) {
        uint64_t daif = net_lock();
        if (!valid(h, C_CONNECTED) || !conns[h].pcb || conns[h].error) {
            net_unlock(daif);
            return sent ? sent : -1;
        }
        struct tcp_pcb *pcb = conns[h].pcb;
        int n = len - sent;
        if (n > tcp_sndbuf(pcb))
            n = tcp_sndbuf(pcb);
        if (n > 0 && tcp_sndqueuelen(pcb) < TCP_SND_QUEUELEN - 4 &&
            tcp_write(pcb, p + sent, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            sent += n;
            tcp_output(pcb);
            n = -1;
        } else {
            tcp_output(pcb);
        }
        net_unlock(daif);
        if (sent < len && expired(start, timeout_ms))
            break;
        if (n >= 0 && sent < len)
            cpu_idle(); // 送信バッファが空く（ACK が届く）まで眠る
    }
    return sent;
}

static int recv_common(int h, void *buf, int len, int timeout_ms, bool peek);

int tcp_api_recv(int h, void *buf, int len, int timeout_ms)
{
    return recv_common(h, buf, len, timeout_ms, false);
}

// 受け取るが、受信バッファから取り除かない（recv の MSG_PEEK）
int tcp_api_recv_peek(int h, void *buf, int len, int timeout_ms)
{
    return recv_common(h, buf, len, timeout_ms, true);
}

static int recv_common(int h, void *buf, int len, int timeout_ms, bool peek)
{
    uint8_t *p = buf;
    uint64_t start = timer_now_us();
    for (;;) {
        uint64_t daif = net_lock();
        if (!valid(h, C_CONNECTED)) {
            net_unlock(daif);
            return -2;
        }
        Conn *c = &conns[h];
        uint32_t n = rx_used(c);
        if (n > (uint32_t)len)
            n = (uint32_t)len;
        if (n) {
            uint32_t first = RX_SIZE - c->rx_tail;  // 輪の終わりまでと、先頭に回る分
            if (first > n)
                first = n;
            memcpy(p, c->rx + c->rx_tail, first);
            memcpy(p + first, c->rx, n - first);
            if (!peek) {
                c->rx_tail = (c->rx_tail + n) % RX_SIZE;
                if (c->pcb)
                    tcp_recved(c->pcb, (u16_t)n);   // 読んだ分だけ窓を開ける
            }
            net_unlock(daif);
            return (int)n;
        }
        bool closed = c->peer_closed, err = c->error;
        net_unlock(daif);
        if (closed)
            return err ? -2 : 0;
        if (expired(start, timeout_ms))
            return -1;
        cpu_idle();
    }
}

void tcp_api_close(int h)
{
    if (h < 0 || h >= MAX_CONNS)
        return;
    uint64_t daif = net_lock();
    Conn *c = &conns[h];
    if (c->state != C_FREE) {
        if (c->state == C_LISTENING) {
            for (int i = 0; i < c->nbacklog; i++) {
                net_unlock(daif);
                tcp_api_close(c->backlog[i]);
                daif = net_lock();
            }
            c->nbacklog = 0;
        }
        if (c->pcb) {
            tcp_arg(c->pcb, nullptr);
            if (c->state != C_LISTENING) {
                tcp_recv(c->pcb, nullptr);
                tcp_err(c->pcb, nullptr);
            } else {
                tcp_accept(c->pcb, nullptr);
            }
            if (tcp_close(c->pcb) != ERR_OK)
                tcp_abort(c->pcb);
        }
        c->state = C_FREE;
    }
    net_unlock(daif);
}

void tcp_api_status(void)
{
    static const char *const names[] = { "free", "listen", "connecting", "connected", "closed" };
    uint64_t daif = net_lock();
    for (int h = 0; h < MAX_CONNS; h++) {
        Conn *c = &conns[h];
        if (c->state == C_FREE)
            continue;
        int port = c->pcb ? c->pcb->local_port : 0;
        printf("  tcp[%d] %-10s port %-5d owner %d%s\n", h, names[c->state], port, c->owner,
               c->peer_closed ? " (peer closed)" : "");
    }
    net_unlock(daif);
}

// owner が開いたままの接続を閉じる（0 = アプリ: アプリが終わった後に。os/slimcc/run.c。
// 1〜 = サービス: サービスを止めたとき）。カーネルが開いたもの（owner 0 で、アプリが動く前）は
// アプリの終わりで閉じられてしまうので、カーネルはこの API で待ち受けない
void tcp_api_close_owner(int owner)
{
    for (int h = 0; h < MAX_CONNS; h++)
        if (conns[h].state != C_FREE && conns[h].owner == owner)
            tcp_api_close(h);
}
