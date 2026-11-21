// 名前から IP アドレスを引く（lwIP の DNS。待つ形。monocs.h の monocs_resolve、TCP・UDP の API の中からも）
//
// DNS のサーバーは、内蔵 Wi-Fi の DHCP がもらったもの（lwIP が自分で設定する）。Wi-Fi がつながっていなければ
// 引けない。lwIP は答えを TTL の間覚えている（DNS_TABLE_SIZE 個）ので、2 回目からはすぐ返る。
//
// 答えは lwIP のコールバック（net_lock の中、主に割り込み）で届く。待っている側が Ctrl-C などでいなくなっても
// よいように、答えを置く場所は静的な枠（SLOTS 個を順に使い回す）で、枠の世代が合うときだけ書く。
// 出典: lwIP の src/include/lwip/dns.h（dns_gethostbyname）

#include <stdio.h>
#include <string.h>
#include "dns_api.h"
#include "net.h"
#include "lwip/dns.h"
#include "../drivers/timer.h"

#define SLOTS 8

static struct {
    uint32_t gen;
    bool done, ok;
    ip_addr_t addr;
} slots[SLOTS];
static uint32_t next_slot;

// lwIP のコールバック（net_lock の中）。arg は「世代 × SLOTS + 枠の番号」
static void on_found(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    uintptr_t key = (uintptr_t)arg;
    unsigned i = key % SLOTS;
    if (slots[i].gen != key / SLOTS)
        return;     // 待っていた側はもういない
    slots[i].ok = addr != nullptr;
    if (addr)
        ip_addr_copy(slots[i].addr, *addr);
    slots[i].done = true;
}

int dns_api_lookup(const char *name, ip_addr_t *addr, int timeout_ms)
{
    if (!name || !*name)
        return -1;
    if (ipaddr_aton(name, addr))
        return 0;
    uint64_t daif = net_lock();
    unsigned i = next_slot++ % SLOTS;
    uint32_t gen = ++slots[i].gen;
    slots[i].done = false;
    err_t err = dns_gethostbyname(name, addr, on_found, (void *)((uintptr_t)gen * SLOTS + i));
    net_unlock(daif);
    if (err == ERR_OK)
        return 0;       // 覚えていた
    if (err != ERR_INPROGRESS)
        return -1;      // 名前が長すぎる、サーバーがない、問い合わせの枠がない
    uint64_t start = timer_now_us();
    for (;;) {
        daif = net_lock();
        bool mine = slots[i].gen == gen, done = slots[i].done, ok = slots[i].ok;
        if (mine && done && ok)
            ip_addr_copy(*addr, slots[i].addr);
        net_unlock(daif);
        if (!mine)
            return -1;  // 枠を使い回された（同時に 8 つを超える問い合わせ）
        if (done)
            return ok ? 0 : -1;
        if (timeout_ms >= 0 && timer_now_us() - start >= (uint64_t)timeout_ms * 1000)
            return -1;
        cpu_idle();     // 次の割り込みまで眠る（コア 0 だけ）
    }
}

int dns_api_resolve(const char *name, char *ip, int size, int timeout_ms)
{
    ip_addr_t addr;
    if (size < 16 || dns_api_lookup(name, &addr, timeout_ms) != 0)
        return -1;
    uint64_t daif = net_lock();     // ipaddr_ntoa_r は状態を持たないが、ほかと同じくロックの中で
    ipaddr_ntoa_r(&addr, ip, size);
    net_unlock(daif);
    return 0;
}
