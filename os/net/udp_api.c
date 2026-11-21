// アプリ・サービスから使える UDP の API（待つ形。monocs.h の monocs_udp_*）。TCP の API（tcp_api.c）と同じ作り
//
// ソケットごとに受信の枠（UDP_SLOTS 個のデータグラム）を持ち、lwIP のコールバック（net_lock の中、主に割り込み）は
// そこに写すだけ。枠がいっぱいなら捨てる（UDP なので届かないこともある）。コールバックの中では malloc しない。
// アプリが終わると（Ctrl-C でも）、そのアプリが開いたソケットは udp_api_close_owner で閉じる（サービスのものは
// サービスを止めたとき）。

#include <stdio.h>
#include <string.h>
#include "udp_api.h"
#include "net.h"
#include "dns_api.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "../drivers/timer.h"
#include "../kernel/svc.h"

#define MAX_SOCKS   8
#define UDP_SLOTS   8
#define UDP_MAX     1472    // 1 つのデータグラムの上限（イーサネットの 1500 - IP 20 - UDP 8）

typedef struct {
    uint16_t len;
    uint16_t port;
    ip_addr_t from;
    uint8_t data[UDP_MAX];
} Datagram;

typedef struct {
    bool used;
    int owner;                      // 開いたもの（0 = アプリ・カーネル、1〜 = サービス。os/kernel/svc.c）
    struct udp_pcb *pcb;
    Datagram slots[UDP_SLOTS];
    int head, count;                // 受け取って、まだ渡していないもの
    uint32_t dropped;
} Sock;

static Sock socks[MAX_SOCKS];

static bool valid(int h)
{
    return h >= 0 && h < MAX_SOCKS && socks[h].used;
}

// lwIP のコールバック（net_lock の中）
static void on_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    Sock *s = &socks[(int)(intptr_t)arg];
    if (s->count == UDP_SLOTS || p->tot_len > UDP_MAX) {
        s->dropped++;
        pbuf_free(p);
        return;
    }
    Datagram *d = &s->slots[(s->head + s->count) % UDP_SLOTS];
    d->len = (uint16_t)pbuf_copy_partial(p, d->data, p->tot_len, 0);
    d->port = port;
    ip_addr_copy(d->from, *addr);
    s->count++;
    pbuf_free(p);
}

int udp_api_open(int port)
{
    return udp_api_open_addr(nullptr, port);
}

// ip のアドレスで受ける（NULL ならどこでも。bind）。ハンドルか -1
int udp_api_open_addr(const char *ip, int port)
{
    ip_addr_t local;
    if (ip && !ipaddr_aton(ip, &local))
        return -1;
    uint64_t daif = net_lock();
    int h = -1;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (!socks[i].used) {
            h = i;
            break;
        }
    struct udp_pcb *pcb = h >= 0 ? udp_new() : nullptr;
    if (!pcb || udp_bind(pcb, ip ? &local : IP_ADDR_ANY, (u16_t)port) != ERR_OK) {
        if (pcb)
            udp_remove(pcb);
        net_unlock(daif);
        return -1;
    }
    memset(&socks[h], 0, sizeof(Sock));
    socks[h].used = true;
    socks[h].owner = svc_owner();
    socks[h].pcb = pcb;
    udp_recv(pcb, on_recv, (void *)(intptr_t)h);
    net_unlock(daif);
    return h;
}

int udp_api_sendto(int h, const char *ip, int port, const void *buf, int len)
{
    ip_addr_t addr;
    // 名前のときは待たない: まだ引けていなければ問い合わせを始めて -1（引けたら覚えているので、次は送れる）
    if (len < 0 || len > UDP_MAX || dns_api_lookup(ip, &addr, 0) != 0)
        return -1;
    uint64_t daif = net_lock();
    if (!valid(h)) {
        net_unlock(daif);
        return -1;
    }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    err_t err = ERR_MEM;
    if (p) {
        pbuf_take(p, buf, (u16_t)len);
        err = udp_sendto(socks[h].pcb, p, &addr, (u16_t)port);
        pbuf_free(p);
    }
    net_unlock(daif);
    return err == ERR_OK ? len : -1;
}

int udp_api_recvfrom(int h, void *buf, int len, char *ip, int *port, int timeout_ms)
{
    uint64_t start = timer_now_us();
    for (;;) {
        uint64_t daif = net_lock();
        if (!valid(h)) {
            net_unlock(daif);
            return -2;
        }
        Sock *s = &socks[h];
        if (s->count) {
            Datagram *d = &s->slots[s->head];
            int n = d->len < len ? d->len : len;
            memcpy(buf, d->data, (size_t)n);
            if (ip)
                ipaddr_ntoa_r(&d->from, ip, 16);
            if (port)
                *port = d->port;
            s->head = (s->head + 1) % UDP_SLOTS;
            s->count--;
            net_unlock(daif);
            return n;
        }
        net_unlock(daif);
        if (timeout_ms >= 0 && timer_now_us() - start >= (uint64_t)timeout_ms * 1000)
            return -1;
        cpu_idle(); // 次の割り込み（USB の受信・タイマー）まで眠る（コア 0 だけ）
    }
}

// 待たずに調べる（select・poll）: 受け取ったものがあるか。ハンドルが違えば -1
int udp_api_ready(int h, bool *readable)
{
    uint64_t daif = net_lock();
    int r = valid(h) ? 0 : -1;
    if (r == 0)
        *readable = socks[h].count > 0;
    net_unlock(daif);
    return r;
}

// 自分のアドレス（ip は 16 バイト以上。NULL 可）とポート（NULL 可）。0 で成功
int udp_api_local(int h, char *ip, int *port)
{
    uint64_t daif = net_lock();
    bool ok = valid(h);
    if (ok) {
        if (ip)
            ipaddr_ntoa_r(&socks[h].pcb->local_ip, ip, 16);
        if (port)
            *port = socks[h].pcb->local_port;
    }
    net_unlock(daif);
    return ok ? 0 : -1;
}

void udp_api_close(int h)
{
    uint64_t daif = net_lock();
    if (valid(h)) {
        udp_remove(socks[h].pcb);
        socks[h].used = false;
    }
    net_unlock(daif);
}

void udp_api_close_owner(int owner)
{
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used && socks[i].owner == owner)
            udp_api_close(i);
}
