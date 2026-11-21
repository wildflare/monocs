// 内蔵 Wi-Fi をつなぐ手順（シェルの wifi コマンドと、自動の接続が共有する）
//
// 自動の接続（wifi_auto_poll）: /etc/wpa_supplicant.conf にネットワークがあれば、スキャンで見えるものに
// つなぎ、切れたらつなぎ直す。サービス services/wifid.c がコア 3 から呼び続ける（monocs_wifi_auto_poll）。
// ほかのサービス（HTTP・telnet）を待たせないよう、スキャンと接続は「始める」と「調べる」に分けてあり
// （wifi.c の wifi_scan_begin・wifi_connect_begin）、1 回の呼び出しは待たずに戻る。待つのは最初の 1 回の
// ファームウェアの読み込み（約 0.5 秒）だけ。画面には何も出さない（シェルの行に割り込むため。wifi status で見る）。
//
// シェルの wifi コマンドと重ならないように、どちらも wifi_manage_lock の中で行い、シェルが何かしたら
// 自動の接続は最初からやり直す（wifi_auto_defer）。wifi disconnect の後は、wifi connect まで自動ではつながない。

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../kernel/klog.h"
#include "wifi.h"
#include "wifi_conf.h"
#include "wifi_auto.h"
#include "timer.h"
#include "../kernel/sysreg.h"
#include "../net/net.h"

// ファームウェアの置き場所: /usr/lib/firmware/brcm/（2026-11-17 から。Linux の /lib/firmware と同じもの）。なければ前の /lib/firmware/brcm/
#define WIFI_FW_DIR     "/usr/lib/firmware/brcm/"
#define WIFI_FW_DIR_OLD "/lib/firmware/brcm/"
#define WIFI_FW         "brcmfmac43436s-sdio.bin"
#define WIFI_NVRAM      "brcmfmac43436s-sdio.txt"
#define WIFI_CLM        "brcmfmac43430-sdio.clm_blob"

static const char *fw_path(char *buf, size_t size, const char *name)
{
    snprintf(buf, size, "%s%s", WIFI_FW_DIR, name);
    if (access(buf, F_OK) != 0)
        snprintf(buf, size, "%s%s", WIFI_FW_DIR_OLD, name);
    return buf;
}

#define RETRY_US        30000000ULL     // つながらなかった・設定がないとき、次に試すまで
#define LOST_US         3000000ULL      // 切れたとき、つなぎ直すまで
#define SCAN_US         15000000ULL     // スキャンの結果を待つ上限
#define JOIN_MS         15000

// ---- シェルと自動の接続が重ならないためのロック（同じコアなら取り直せる。Ctrl-C で抜けても詰まらない）----
static atomic_int manage_owner = -1;

void wifi_manage_lock(void)
{
    int me = cpu_id(), expected = -1;
    if (atomic_load(&manage_owner) == me)
        return;
    while (!atomic_compare_exchange_weak(&manage_owner, &expected, me)) {
        expected = -1;
        __asm__ volatile("yield");
    }
}

static bool manage_trylock(void)
{
    int expected = -1;
    return atomic_compare_exchange_strong(&manage_owner, &expected, cpu_id());
}

void wifi_manage_unlock(void)
{
    atomic_store(&manage_owner, -1);
}

// ---- チップを動かす: SDIO でつなぎ、ファームウェア、CLM、国、WLC_UP ----
bool wifi_bringup(bool verbose)
{
    const struct wifi_info *w = wifi_get_info();
    if (w->running)
        return true;
    if (!wifi_probe()) {
        if (verbose)
            printf("wifi: not found (%s)\n", w->error ? w->error : "?");
        return false;
    }
    if (verbose)
        printf("wifi: chip %u rev %u, SDIO %u functions\n", w->chip, w->rev, (w->ocr >> 28) & 7);
    char fw[64], nvram[64], clm[64];
    if (!wifi_start(fw_path(fw, sizeof fw, WIFI_FW), fw_path(nvram, sizeof nvram, WIFI_NVRAM))) {
        if (verbose)
            printf("wifi: start failed (%s)\n", w->error ? w->error : "?");
        return false;
    }
    if (verbose)
        printf("wifi: firmware %u bytes, nvram %u bytes, RAM %u KB, running in %llu ms\n", w->fw_size,
               w->nvram_size, w->ramsize / 1024, (unsigned long long)w->start_us / 1000);
    static struct wifi_conf conf;
    const char *country = wifi_conf_read(WIFI_CONF_PATH, &conf) >= 0 && conf.country[0] ? conf.country : "JP";
    bool up = wifi_setup(fw_path(clm, sizeof clm, WIFI_CLM), country);
    memset(&conf, 0, sizeof conf);
    if (verbose)
        printf("wifi: clm %d, events %d, country %d, up %d\n", w->clm_status, w->event_status,
               w->country_status, w->up_status);
    return up;
}

// ---- 設定のネットワークを試す順に並べる: 見えるものを priority・強さの順に。見えないものは後ろ（隠れた SSID
// かもしれない。visible_only なら入れない）。入れた数 ----
int wifi_pick(const struct wifi_conf *conf, const struct wifi_bss *list, int n, struct wifi_network *cand,
              bool visible_only)
{
    int best_rssi[WIFI_CONF_MAX];
    bool used[WIFI_CONF_MAX] = {0};
    for (int i = 0; i < conf->count; i++) {
        best_rssi[i] = -1000;
        for (int k = 0; k < n; k++)
            if (!strcmp(list[k].ssid, conf->net[i].ssid) && list[k].rssi > best_rssi[i])
                best_rssi[i] = list[k].rssi;
    }
    int ncand = 0;
    for (int round = 0; round < conf->count; round++) {
        int pick = -1;
        for (int i = 0; i < conf->count; i++) {
            if (used[i])
                continue;
            if (pick < 0 || (best_rssi[i] > -1000) > (best_rssi[pick] > -1000) ||
                ((best_rssi[i] > -1000) == (best_rssi[pick] > -1000) &&
                 (conf->net[i].priority > conf->net[pick].priority ||
                  (conf->net[i].priority == conf->net[pick].priority && best_rssi[i] > best_rssi[pick]))))
                pick = i;
        }
        used[pick] = true;
        if (conf->net[pick].ssid[0] && (!visible_only || best_rssi[pick] > -1000))
            cand[ncand++] = conf->net[pick];
    }
    return ncand;
}

// スキャンで見えた ssid のうち、いちばん強いもの（接続のときに BSSID とチャンネルを渡す）。なければ NULL
const struct wifi_bss *wifi_bss_of(const struct wifi_bss *list, int n, const char *ssid)
{
    const struct wifi_bss *best = nullptr;
    for (int k = 0; k < n; k++)
        if (!strcmp(list[k].ssid, ssid) && (!best || list[k].rssi > best->rssi))
            best = &list[k];
    return best;
}

// スキャンで WPA だけ（RSN がない）とわかっているときは WPA。ほかは WPA2
int wifi_security_of(const struct wifi_bss *list, int n, const char *ssid)
{
    for (int k = 0; k < n; k++)
        if (!strcmp(list[k].ssid, ssid) && list[k].wpa && !list[k].wpa2)
            return WIFI_SEC_WPA;
    return WIFI_SEC_WPA2;
}

// つながった後: lwIP の口を上げて DHCP を始める
bool wifi_net_up(void)
{
    uint8_t mac[6];
    if (wifi_iovar_get("cur_etheraddr", mac, 6) != 0)
        return false;
    net_wifi_attach(mac);
    net_wifi_link(true);
    return true;
}

// ---- 自動の接続 ----
static enum { A_IDLE, A_SCANNING, A_JOINING, A_CONNECTED, A_OFF } state;
static bool hold;                   // wifi disconnect の後（wifi connect まで自動ではつながない）
static uint64_t next_us, scan_end_us;
static struct wifi_conf aconf;      // パスフレーズを含むので、使い終わったら消す
static struct wifi_network cand[WIFI_CONF_MAX];
static int ncand, icand;
static const struct wifi_bss *alist;
static int alist_n;
static int last_result, attempts;   // 最後の接続の結果（WIFI_ERR_*。調べる用）と、試した回数
static bool addr_logged;            // DHCP のアドレスを記録した
static int fail_logged_r;           // 最後に記録した接続の失敗
static char fail_logged_ssid[33];

static void forget(void)
{
    memset(&aconf, 0, sizeof aconf);
    memset(cand, 0, sizeof cand);
    ncand = icand = 0;
}

void wifi_auto_defer(bool hold_off)
{
    forget();
    hold = hold_off;
    if (state != A_OFF)
        state = A_IDLE;
    next_us = timer_now_us() + RETRY_US;
}

static void idle(uint64_t wait_us)
{
    forget();
    state = A_IDLE;
    next_us = timer_now_us() + wait_us;
}

static void join_next(void)
{
    while (icand < ncand) {
        struct wifi_network *c = &cand[icand++];
        int r = wifi_connect_begin(c->ssid, c->open ? "" : c->psk, wifi_security_of(alist, alist_n, c->ssid),
                                   wifi_bss_of(alist, alist_n, c->ssid), JOIN_MS);
        memset(c->psk, 0, sizeof c->psk);
        if (r == 0) {
            state = A_JOINING;
            return;
        }
    }
    idle(RETRY_US);
}

// 少しだけ進める（待たない）。何かしたら 1
int wifi_auto_poll(void)
{
    wifi_diag_report();
    if (state == A_OFF || hold)
        return 0;
    uint64_t now = timer_now_us();
    if (state == A_IDLE && now < next_us && !wifi_link_up())
        return 0;
    if (!manage_trylock())
        return 0;
    int did = 1;
    switch (state) {
    case A_IDLE:
        if (wifi_link_up()) {       // シェルの wifi connect でつながっている
            state = A_CONNECTED;
            break;
        }
        if (wifi_conf_read(WIFI_CONF_PATH, &aconf) <= 0) {
            idle(RETRY_US);         // 設定がない（後で書かれるかもしれない）
            break;
        }
        if (!wifi_bringup(false)) {
            forget();
            state = A_OFF;          // チップがない（QEMU など）・ファームウェアがない
            break;
        }
        klog_printf("wifi: chip ready, scanning");
        if (wifi_scan_begin() != 0) {
            idle(RETRY_US);
            break;
        }
        scan_end_us = now + SCAN_US;
        state = A_SCANNING;
        break;
    case A_SCANNING:
        alist_n = wifi_scan_poll(&alist);
        if (alist_n < 0) {
            if (now < scan_end_us)
                did = 0;
            else
                idle(RETRY_US);
            break;
        }
        ncand = wifi_pick(&aconf, alist, alist_n, cand, true);
        klog_printf("wifi: scan done (%d networks), joining", alist_n);
        icand = 0;
        memset(&aconf, 0, sizeof aconf);
        join_next();
        break;
    case A_JOINING: {
        int r = wifi_connect_poll();
        const char *ssid = icand > 0 ? cand[icand - 1].ssid : "?";
        if (r != WIFI_PENDING) {
            last_result = r;
            attempts++;
        }
        if (r == WIFI_PENDING)
            did = 0;
        else if (r == 0 && wifi_net_up()) {
            klog_printf("wifi: connected to %s", ssid);    // 記録（dmesg。os/kernel/klog.c）
            addr_logged = false;
            fail_logged_r = 0;
            fail_logged_ssid[0] = 0;
            forget();
            state = A_CONNECTED;
        } else {
            // 同じ失敗の繰り返し（30 秒ごとにやり直す）は記録しない
            if (r != fail_logged_r || strcmp(ssid, fail_logged_ssid) != 0) {
                klog_printf("wifi: could not join %s (error %d)", ssid, r);
                fail_logged_r = r;
                snprintf(fail_logged_ssid, sizeof fail_logged_ssid, "%s", ssid);
            }
            join_next();
        }
        break;
    }
    case A_CONNECTED:
        if (wifi_link_up()) {
            did = 0;
            char ip[20];
            if (!addr_logged && net_wifi_address(ip, nullptr, nullptr, sizeof ip)) {
                klog_printf("wifi: address %s (DHCP)", ip);
                addr_logged = true;
            }
        } else {
            klog_printf("wifi: link lost; reconnecting");
            idle(LOST_US);          // 切れた: 少し待ってつなぎ直す
        }
        break;
    default:
        did = 0;
        break;
    }
    wifi_manage_unlock();
    return did;
}

int wifi_auto_last(int *count)
{
    *count = attempts;
    return last_result;
}

const char *wifi_auto_state(void)
{
    if (hold)
        return "off (until wifi connect)";
    switch (state) {
    case A_IDLE: return "waiting";
    case A_SCANNING: return "scanning";
    case A_JOINING: return "connecting";
    case A_CONNECTED: return "connected";
    default: return "off";
    }
}
