// ネットワーク: USB イーサネット（os/drivers/usb.c の CDC-ECM）と lwIP（third_party/lwip）のつなぎ。
// lwIP は NO_SYS=1（OS の仕組みを使わない）で、raw API（コールバック）を使う。
// lwIP を呼べるのは一度に 1 か所だけなので、すべて net_lock の中で呼ぶ（USB の割り込みの受信、
// タイマー割り込みの lwIP のタイマー、アプリやコンソールからの送信）。
// 出典: lwIP の doc/NO_SYS_SampleCode.c、src/netif/ethernet.c、include/lwip/netif.h

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "net.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "../drivers/timer.h"
#include "../drivers/usb.h"
#include "../drivers/wifi.h"
#include "../kernel/sysreg.h"

static struct netif usb_netif;
static struct netif wifi_netif;
static bool ready, wifi_attached;
static uint64_t rx_frames, tx_frames, rx_nomem, tx_fail;
static uint64_t wifi_rx_frames, wifi_tx_frames, wifi_tx_fail;

// ---- ロック ----

static atomic_int lock_owner = -1;
static int lock_depth;

uint64_t net_lock(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    int me = cpu_id();
    if (atomic_load_explicit(&lock_owner, memory_order_relaxed) == me) {
        lock_depth++;
        return daif;
    }
    int expected = -1;
    while (!atomic_compare_exchange_weak_explicit(&lock_owner, &expected, me, memory_order_acquire,
                                                  memory_order_relaxed)) {
        expected = -1;
        __asm__ volatile("yield");
    }
    lock_depth = 1;
    return daif;
}

void net_unlock(uint64_t daif)
{
    if (--lock_depth == 0)
        atomic_store_explicit(&lock_owner, -1, memory_order_release);
    write_sysreg(daif, daif);
}

// lwIP の時刻（ミリ秒）
u32_t sys_now(void)
{
    return (u32_t)(timer_now_us() / 1000);
}

// ---- ネットワークのインターフェース（USB イーサネット） ----

static err_t usb_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    static uint8_t frame[1514];
    if (p->tot_len > sizeof(frame))
        return ERR_IF;
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    if (!usb_eth_send(frame, p->tot_len)) {
        tx_fail++;
        return ERR_IF;
    }
    tx_frames++;
    return ERR_OK;
}

static err_t usb_netif_init(struct netif *netif)
{
    static const uint8_t mac[6] = NET_MAC_ADDR;
    netif->name[0] = 'u';
    netif->name[1] = '0';
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = usb_linkoutput;
    return ERR_OK;
}

void net_input(const uint8_t *frame, uint32_t len)
{
    if (!ready)
        return;
    uint64_t daif = net_lock();
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) {
        rx_nomem++;
    } else {
        pbuf_take(p, frame, (u16_t)len);
        rx_frames++;
        if (usb_netif.input(p, &usb_netif) != ERR_OK)
            pbuf_free(p);
    }
    net_unlock(daif);
}

void net_timer(void)
{
    if (!ready)
        return;
    uint64_t daif = net_lock();
    sys_check_timeouts();
    net_unlock(daif);
    wifi_net_poll();        // Wi-Fi のチップから届いたものを読み、待たせている送信を送る（バスが空いていれば）
    if (wifi_attached && netif_is_link_up(&wifi_netif) && !wifi_link_up())
        net_wifi_link(false);   // アクセスポイントから切られた
}

// ---- ネットワークのインターフェース（内蔵 Wi-Fi。os/drivers/wifi.c。アドレスは DHCP）----

static err_t wifi_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    static uint8_t frame[1514];
    if (p->tot_len > sizeof(frame))
        return ERR_IF;
    uint64_t daif = net_lock();     // frame を共有するので（lwIP からはいつもロックの中だが念のため）
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    bool ok = wifi_send_eth(frame, p->tot_len);
    net_unlock(daif);
    if (!ok) {
        wifi_tx_fail++;
        return ERR_IF;
    }
    wifi_tx_frames++;
    return ERR_OK;
}

static err_t wifi_netif_init(struct netif *netif)
{
    netif->name[0] = 'w';
    netif->name[1] = '0';
    netif->hwaddr_len = 6;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = wifi_linkoutput;
#if LWIP_NETIF_HOSTNAME
    netif->hostname = "monocs";
#endif
    return ERR_OK;
}

static void wifi_rx(const uint8_t *frame, uint32_t len)
{
    if (!ready || !wifi_attached)
        return;
    uint64_t daif = net_lock();
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) {
        rx_nomem++;
    } else {
        pbuf_take(p, frame, (u16_t)len);
        wifi_rx_frames++;
        if (wifi_netif.input(p, &wifi_netif) != ERR_OK)
            pbuf_free(p);
    }
    net_unlock(daif);
}

void net_wifi_attach(const uint8_t mac[6])
{
    uint64_t daif = net_lock();
    if (!wifi_attached) {
        ip4_addr_t zero;
        ip4_addr_set_zero(&zero);
        netif_add(&wifi_netif, &zero, &zero, &zero, nullptr, wifi_netif_init, ethernet_input);
        wifi_attached = true;
    }
    memcpy(wifi_netif.hwaddr, mac, 6);
    wifi_set_rx_hook(wifi_rx);
    net_unlock(daif);
}

// アクセスポイントにつながった・切れた。つながったら DHCP でアドレスをもらい、インターネットへの道（既定の
// 経路）を Wi-Fi にする。USB の 192.168.7.0/24 はそのまま USB を通る
void net_wifi_link(bool up)
{
    if (!wifi_attached)
        return;
    uint64_t daif = net_lock();
    if (up) {
        netif_set_up(&wifi_netif);
        netif_set_link_up(&wifi_netif);
        netif_set_default(&wifi_netif);
        dhcp_start(&wifi_netif);
    } else {
        dhcp_release_and_stop(&wifi_netif);
        netif_set_link_down(&wifi_netif);
        netif_set_down(&wifi_netif);
        netif_set_default(&usb_netif);
    }
    net_unlock(daif);
}

bool net_wifi_address(char *ip, char *mask, char *gw, int size)
{
    if (!wifi_attached)
        return false;
    uint64_t daif = net_lock();
    bool bound = dhcp_supplied_address(&wifi_netif);
    if (ip)
        snprintf(ip, size, "%s", ip4addr_ntoa(netif_ip4_addr(&wifi_netif)));
    if (mask)
        snprintf(mask, size, "%s", ip4addr_ntoa(netif_ip4_netmask(&wifi_netif)));
    if (gw)
        snprintf(gw, size, "%s", ip4addr_ntoa(netif_ip4_gw(&wifi_netif)));
    net_unlock(daif);
    return bound;
}

// ---- TCP のエコー（RFC 862、ポート 7）。USB イーサネットの速さと一致を確かめるためのもの（tools/test/echotest.py、hwcheck.sh）----

static err_t echo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)err;
    if (!p) {   // 相手が閉じた
        tcp_close(pcb);
        return ERR_OK;
    }
    // 送信バッファに入る分だけ送り返す（入らなければ、受け取りを遅らせて相手を待たせる）
    u16_t n = p->tot_len;
    if (n > tcp_sndbuf(pcb))
        return ERR_MEM;     // lwIP があとでもう一度渡してくれる
    for (struct pbuf *q = p; q; q = q->next)
        tcp_write(pcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    tcp_recved(pcb, n);
    pbuf_free(p);
    return ERR_OK;
}

static err_t echo_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb)
        return ERR_VAL;
    tcp_recv(pcb, echo_recv);
    return ERR_OK;
}

static void echo_init(void)
{
    struct tcp_pcb *pcb = tcp_new();
    // USB からだけ受ける（確認用で、認証がない。Wi-Fi からは受けない）。アドレスだけでなく口も決める:
    // lwIP は、ほかの口のアドレスあてのパケットも受け取るので（Wi-Fi の側から 192.168.7.2 あてに送れる）
    if (!pcb || tcp_bind(pcb, netif_ip_addr4(&usb_netif), 7) != ERR_OK)
        return;
    tcp_bind_netif(pcb, &usb_netif);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, echo_accept);
}

struct netif *net_netif_of_addr(const struct ip4_addr *addr)
{
    if (ip4_addr_isany(addr))
        return nullptr;
    if (ip4_addr_eq(addr, netif_ip4_addr(&usb_netif)))
        return &usb_netif;
    if (wifi_attached && ip4_addr_eq(addr, netif_ip4_addr(&wifi_netif)))
        return &wifi_netif;
    return nullptr;
}

int net_input_kind(void)
{
    return wifi_attached && ip_current_input_netif() == &wifi_netif ? NET_IF_WIFI : NET_IF_USB;
}

void net_init(void)
{
    uint64_t daif = net_lock();
    lwip_init();
    ip4_addr_t ip, mask, gw;
    // NET_IP_ADDR は「a, b, c, d」なので、展開してから IP4_ADDR に渡す
#define SET_ADDR(addr, ...) IP4_ADDR(addr, __VA_ARGS__)
    SET_ADDR(&ip, NET_IP_ADDR);
    SET_ADDR(&mask, NET_NETMASK);
#undef SET_ADDR
    ip4_addr_set_zero(&gw);
    netif_add(&usb_netif, &ip, &mask, &gw, nullptr, usb_netif_init, ethernet_input);
    netif_set_default(&usb_netif);
    netif_set_up(&usb_netif);
    netif_set_link_up(&usb_netif);
    echo_init();
    ready = true;
    net_unlock(daif);
}

void net_status(void)
{
    printf("net: %s, rx %llu frames (no memory %llu), tx %llu frames (failed %llu)\n",
           ipaddr_ntoa(&usb_netif.ip_addr), (unsigned long long)rx_frames,
           (unsigned long long)rx_nomem, (unsigned long long)tx_frames, (unsigned long long)tx_fail);
    if (wifi_attached)
        printf("net: wifi %s, rx %llu frames, tx %llu frames (failed %llu)\n", ipaddr_ntoa(&wifi_netif.ip_addr),
               (unsigned long long)wifi_rx_frames, (unsigned long long)wifi_tx_frames,
               (unsigned long long)wifi_tx_fail);
}
