// シェルの wifi コマンド（内蔵 Wi-Fi。ドライバは os/drivers/wifi.c、つなぐ手順と自動の接続は wifi_auto.c）
//
//   wifi [start]                    チップを動かして、ファームウェアの版と MAC アドレスを表示する
//   wifi scan                       周りのアクセスポイント（強い順）
//   wifi connect [SSID [PASS]]      つなぐ（引数がなければ /etc/wpa_supplicant.conf から）
//   wifi disconnect                 切る（wifi connect まで、自動でもつながない）
//   wifi status                     つながっている先・アドレス・自動の接続の様子
//   調べるためのもの:
//   wifi log                        ファームウェアのログ
//   wifi events                     受信の数、割り込みの様子、最後の接続のイベント
//   wifi irq on|off                 受信を割り込みで読むか、10ms ごとのタイマーだけにするか
//   wifi iovar NAME [VALUE]         ファームウェアの設定（32 ビットの値）を読む・書く
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_cmd.h"
#include "../drivers/timer.h"
#include "../drivers/sdio.h"
#include "../drivers/wifi.h"
#include "../drivers/wifi_auto.h"
#include "../drivers/wifi_conf.h"
#include "../net/net.h"

static bool ensure_started(bool verbose)
{
    if (wifi_bringup(verbose))
        return true;
    if (!verbose) {
        const struct wifi_info *w = wifi_get_info();
        printf("wifi: cannot start (%s)\n", w->error ? w->error : "?");
    }
    return false;
}

static const char *error_text(int r)
{
    switch (r) {
    case WIFI_ERR_BAD_SSID: return "bad SSID";
    case WIFI_ERR_BAD_PASS: return "passphrase must be 8-63 characters (or 64 hex digits)";
    case WIFI_ERR_SETUP: return "firmware refused the settings";
    case WIFI_ERR_AUTH: return "authentication failed (wrong passphrase?)";
    case WIFI_ERR_TIMEOUT: return "timeout";
    case WIFI_ERR_JOIN - 1: return "join failed";
    case WIFI_ERR_JOIN - 2: return "join timeout";
    case WIFI_ERR_JOIN - 3: return "network not found";
    default: return "error";
    }
}

// 最後の接続の間にファームウェアから届いたイベント（番号は brcmfmac の fweh.h）
static void print_join_events(const char *indent)
{
    const struct wifi_event_rec *ev;
    int n = wifi_join_trace(&ev);
    for (int k = 0; k < n; k++)
        printf("wifi: %sevent %u status %u reason %u flags %u\n", indent, ev[k].type, ev[k].status, ev[k].reason,
               ev[k].flags);
}

static int cmd_start(int, char **)
{
    if (!ensure_started(true))
        return 1;
    char ver[256] = {0};
    if (wifi_iovar_get("ver", ver, sizeof ver - 1) == 0) {
        ver[strcspn(ver, "\n")] = 0;
        printf("wifi: %s\n", ver);
    }
    uint8_t mac[6] = {0};
    if (wifi_iovar_get("cur_etheraddr", mac, 6) == 0)
        printf("wifi: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int cmd_scan(int, char **)
{
    if (!ensure_started(false))
        return 1;
    const struct wifi_bss *list;
    uint64_t t0 = timer_now_us();
    int n = wifi_scan(&list, 15000);
    if (n < 0) {
        printf("wifi: scan failed (%d)\n", n);
        return 1;
    }
    // 強い順に並べる
    int order[WIFI_MAX_BSS];
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && list[order[j]].rssi > list[order[j - 1]].rssi; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    printf("%-32s  %-17s  %3s  %4s  %s\n", "SSID", "BSSID", "CH", "dBm", "SECURITY");
    for (int k = 0; k < n; k++) {
        const struct wifi_bss *b = &list[order[k]];
        printf("%-32s  %02x:%02x:%02x:%02x:%02x:%02x  %3d  %4d  %s\n", b->ssid[0] ? b->ssid : "(hidden)",
               b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5], b->channel, b->rssi,
               b->wpa2 ? "WPA2" : b->wpa ? "WPA" : b->privacy ? "WEP" : "open");
    }
    printf("%d networks, %llu ms\n", n, (unsigned long long)(timer_now_us() - t0) / 1000);
    return 0;
}

// つながった後: lwIP の口を上げて、DHCP でアドレスをもらうまで待つ
static int wait_for_address(uint64_t t0)
{
    if (!wifi_net_up()) {
        printf("wifi: cannot read the MAC address\n");
        return 1;
    }
    char ip[20], mask[20], gw[20];
    uint64_t end = timer_now_us() + 15000000;
    while (!net_wifi_address(ip, mask, gw, sizeof ip) && timer_now_us() < end)
        cpu_idle();
    if (!net_wifi_address(ip, mask, gw, sizeof ip)) {
        printf("wifi: no address from DHCP\n");
        return 1;
    }
    printf("wifi: address %s, netmask %s, gateway %s (DHCP, %llu ms)\n", ip, mask, gw,
           (unsigned long long)(timer_now_us() - t0) / 1000);
    return 0;
}

// wifi connect [SSID [PASSPHRASE]]。引数がなければ /etc/wpa_supplicant.conf から（スキャンで見えるもののうち、
// priority の大きいもの、同じなら強いもの）。打ち込んだパスフレーズを履歴に残さないよう、行の前に空白を
static int cmd_connect(int argc, char **argv)
{
    if (!ensure_started(false))
        return 1;
    // 試すネットワークを cand に並べる
    struct wifi_network cand[WIFI_CONF_MAX + 1];
    int ncand = 0, nbss = 0;
    const struct wifi_bss *bss = nullptr;
    if (argc >= 1) {
        memset(&cand[0], 0, sizeof cand[0]);
        snprintf(cand[0].ssid, sizeof cand[0].ssid, "%s", argv[0]);
        if (argc >= 2)
            snprintf(cand[0].psk, sizeof cand[0].psk, "%s", argv[1]);
        else
            cand[0].open = true;
        ncand = 1;
    } else {
        static struct wifi_conf conf;
        if (wifi_conf_read(WIFI_CONF_PATH, &conf) <= 0) {
            printf("wifi: no network in %s\n", WIFI_CONF_PATH);
            return 1;
        }
        nbss = wifi_scan(&bss, 15000);
        if (nbss < 0)
            nbss = 0;
        ncand = wifi_pick(&conf, bss, nbss, cand, false);
        memset(&conf, 0, sizeof conf);      // パスフレーズを残さない
    }
    // 順に試す
    int status = 1;
    for (int i = 0; i < ncand && status != 0; i++) {
        printf("wifi: connecting to \"%s\"...\n", cand[i].ssid);
        uint64_t t0 = timer_now_us();
        int r = wifi_connect(cand[i].ssid, cand[i].open ? "" : cand[i].psk, wifi_security_of(bss, nbss, cand[i].ssid),
                             wifi_bss_of(bss, nbss, cand[i].ssid), 15000);
        if (r) {
            printf("wifi: %s: %s (%d)\n", cand[i].ssid, error_text(r), r);
            print_join_events("  ");
            continue;
        }
        printf("wifi: connected in %llu ms\n", (unsigned long long)(timer_now_us() - t0) / 1000);
        status = wait_for_address(t0);
        break;
    }
    memset(cand, 0, sizeof cand);
    return status;
}

static int cmd_disconnect(int, char **)
{
    net_wifi_link(false);
    wifi_disconnect();
    printf("wifi: disconnected\n");
    return 0;
}

static int cmd_status(int, char **)
{
    const struct wifi_info *w = wifi_get_info();
    if (!w->running) {
        printf("wifi: not started (auto connect: %s)\n", wifi_auto_state());
        return 0;
    }
    if (!wifi_link_up()) {
        printf("wifi: not connected%s (auto connect: %s)\n", w->ssid[0] ? " (link lost)" : "", wifi_auto_state());
        // 自動の接続は画面に出さないので、うまくいかなかったときの手がかりをここで
        int tries, last = wifi_auto_last(&tries);
        if (tries && last) {
            printf("wifi: auto connect tried %d times, last: %s (%d)\n", tries, error_text(last), last);
            print_join_events("  ");
        }
        return 0;
    }
    char ip[20], mask[20], gw[20];
    bool bound = net_wifi_address(ip, mask, gw, sizeof ip);
    uint32_t rate = 0;          // WLC_GET_RATE（12。brcmfmac の BRCMF_C_GET_RATE）: 500kbps 単位
    wifi_ioctl(12, false, &rate, sizeof rate);
    printf("wifi: connected to \"%s\", %d dBm, %u.%u Mbps, %s\n", w->ssid, wifi_rssi(), rate / 2, rate % 2 * 5,
           bound ? "" : "no address yet (DHCP)");
    if (bound)
        printf("wifi: address %s, netmask %s, gateway %s\n", ip, mask, gw);
    printf("wifi: rx %llu data frames, tx %llu (dropped %llu)\n", (unsigned long long)w->rx_data,
           (unsigned long long)w->tx_data, (unsigned long long)w->tx_dropped);
    return 0;
}

static int cmd_log(int, char **)
{
    char *log = malloc(65536);
    if (log && wifi_read_console(log, 65536))
        printf("%s\n", log);
    else
        printf("wifi: cannot read the firmware log\n");
    free(log);
    return 0;
}

static int cmd_events(int, char **)
{
    const struct wifi_info *w = wifi_get_info();
    printf("wifi: rx %llu frames (%llu events, %llu data, %llu glom, %llu errors), txglom %d\n",
           (unsigned long long)w->rx_frames, (unsigned long long)w->rx_events, (unsigned long long)w->rx_data,
           (unsigned long long)w->rx_glom, (unsigned long long)w->rx_errors, w->glom_status);
    printf("wifi: rx by %s, %llu interrupts%s\n", w->irq_mode ? "interrupt" : "timer (10 ms)",
           (unsigned long long)w->irqs, w->irq_storms ? " (turned off: too many interrupts)" : "");
    if (w->irq_storms)
        printf("wifi: at turn-off: intstatus %08x, hostintmask %08x, CCCR int pending %02x, int enable %02x\n",
               w->storm_intstatus, w->storm_hostintmask, w->storm_cccr_pending, w->storm_cccr_ien);
    printf("wifi: rx gaps while sending %llu, waits for the chip to accept frames %llu\n",
           (unsigned long long)w->rx_gaps, (unsigned long long)w->credit_waits);
    printf("wifi: events by type:");
    for (int t = 0; t < WIFI_EVENT_TYPES; t++)
        if (w->event_counts[t])
            printf(" %d:%u", t, w->event_counts[t]);
    printf("\n");
    print_join_events("");
    return 0;
}

// wifi perf [reset]: 時間の内訳（2026-11-22。Wi-Fi を速くするために測る）
static int cmd_perf(int argc, char **argv)
{
    if (argc >= 1 && !strcmp(argv[0], "reset")) {
        wifi_perf_reset();
        printf("wifi perf: reset\n");
        return 0;
    }
    const struct wifi_perf *w = wifi_get_perf();
    const struct sdio_perf *d = sdio_get_perf();
    double f = (double)timer_freq() / 1e6;      // カウント → マイクロ秒
    double wall = (double)(timer_count() - w->since) / f;
#define MS(x) ((double)(x) / f / 1000.0)
    printf("wifi perf: %.0f ms since reset, SDIO %u MHz\n", wall / 1000.0, sdio_clock_hz() / 1000000);
    printf("  rx: %llu polls (%llu with frames), %llu frames, %llu bytes to lwIP (%.0f KB/s)\n",
           (unsigned long long)w->polls, (unsigned long long)w->polls_with_frames,
           (unsigned long long)wifi_get_info()->rx_frames, (unsigned long long)w->rx_bytes,
           wall > 0 ? (double)w->rx_bytes / wall * 1e6 / 1024 : 0);
    printf("      poll %.1f ms, lwIP %.1f ms\n", MS(w->t_poll), MS(w->t_hook));
    printf("  tx: %llu frames, %llu bytes (%.0f KB/s), %.1f ms\n", (unsigned long long)w->tx_frames,
           (unsigned long long)w->tx_bytes, wall > 0 ? (double)w->tx_bytes / wall * 1e6 / 1024 : 0, MS(w->t_tx));
    printf("  sdio: %llu CMD52, %llu CMD53 (%llu bytes)\n", (unsigned long long)d->cmd52,
           (unsigned long long)d->cmd53, (unsigned long long)d->bytes);
    printf("      command %.1f ms, wait ready %.1f ms, pio %.1f ms, wait done %.1f ms\n", MS(d->t_cmd),
           MS(d->t_ready), MS(d->t_pio), MS(d->t_done));
#undef MS
    return 0;
}

static int cmd_irq(int argc, char **argv)
{
    if (argc < 1)
        return -1;
    bool on = !strcmp(argv[0], "on");
    printf("wifi: rx by %s\n", wifi_set_irq_mode(on) == on && on ? "interrupt" : "timer (10 ms)");
    return 0;
}

// wifi pm [0|1|2]: チップの省電力（2026-11-22）。WLC_GET_PM（85）・WLC_SET_PM（86）。brcmfmac の値と同じ:
// 0 = 切る（PM_OFF。Monocs の既定）、1 = いちばん省電力（PM_MAX）、2 = 通信があれば起きている（PM_FAST。Linux の既定）
static int cmd_pm(int argc, char **argv)
{
    uint32_t v;
    if (argc >= 1) {
        v = (uint32_t)strtoul(argv[0], nullptr, 0);
        if (v > 2)
            return -1;
        int r = wifi_ioctl(86, true, &v, sizeof v);
        if (r)
            printf("wifi: set pm failed (%d)\n", r);
    }
    v = 0;
    int r = wifi_ioctl(85, false, &v, sizeof v);
    if (r)
        printf("wifi: get pm failed (%d)\n", r);
    else
        printf("wifi: pm %u (%s)\n", v, v == 0 ? "off" : v == 1 ? "max power saving" : v == 2 ? "fast" : "?");
    return 0;
}

static int cmd_iovar(int argc, char **argv)
{
    if (argc < 1)
        return -1;
    uint32_t v = 0;
    if (argc > 1) {
        v = (uint32_t)strtoul(argv[1], nullptr, 0);
        int r = wifi_iovar_set(argv[0], &v, 4);
        if (r)
            printf("wifi: set %s failed (%d)\n", argv[0], r);
    }
    v = 0;
    int r = wifi_iovar_get(argv[0], &v, 4);
    if (r)
        printf("wifi: get %s failed (%d)\n", argv[0], r);
    else
        printf("wifi: %s = %u (0x%x)\n", argv[0], v, v);
    return 0;
}

// manage: つなぐ・切るなど、自動の接続（サービス wifid）と重なってはいけないもの。ロックの中で行い、自動のほうは
// 最初からやり直させる
static const struct {
    const char *name;
    int (*fn)(int argc, char **argv);   // 引数は、サブコマンドの後のもの。使い方が違えば -1
    bool manage;
} subcommands[] = {
    {"start", cmd_start, true},   {"scan", cmd_scan, true},     {"connect", cmd_connect, true},
    {"disconnect", cmd_disconnect, true}, {"status", cmd_status, false}, {"log", cmd_log, false},
    {"events", cmd_events, false}, {"irq", cmd_irq, false}, {"perf", cmd_perf, false}, {"pm", cmd_pm, false},     {"iovar", cmd_iovar, false},
};

int cmd_wifi(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "start";
    for (unsigned i = 0; i < sizeof subcommands / sizeof subcommands[0]; i++) {
        if (strcmp(sub, subcommands[i].name))
            continue;
        if (subcommands[i].manage) {
            wifi_manage_lock();
            wifi_auto_defer(!strcmp(sub, "disconnect"));
        }
        int r = subcommands[i].fn(argc > 2 ? argc - 2 : 0, argv + 2);
        if (subcommands[i].manage)
            wifi_manage_unlock();
        if (r >= 0)
            return r;
        break;
    }
    printf("usage: wifi [start|scan|connect [SSID [PASS]]|disconnect|status|log|events|irq on|off|perf [reset]|pm [0|1|2]|iovar NAME [VALUE]]\n");
    return 1;
}
