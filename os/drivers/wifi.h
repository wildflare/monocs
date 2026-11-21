// 内蔵 Wi-Fi（CYW43436、SDIO。os/drivers/wifi.c）
#pragma once

#include <stdint.h>

#define WIFI_EVENT_TYPES 192
struct wifi_info {
    bool found;
    const char *error;      // 見つからなかったときの理由
    uint32_t ocr;           // SDIO の CMD5 の応答
    uint32_t chipid_reg;    // ChipCommon の chipid レジスタ
    uint32_t chip, rev;     // チップの番号（43430 など）と版
    uint64_t probe_us;
    // wifi_start
    bool running;
    int cores;              // バックプレーンのコアの数
    uint32_t ramsize;       // チップの RAM
    uint32_t fw_size, nvram_size;
    uint32_t shared_addr;   // ファームウェアが RAM の最後に書く、自分の情報の番地
    uint32_t shared_flags;  // その中の flags（下位 8 ビットが版。0x400 はファームウェアの例外）
    uint64_t download_us, start_us;
    uint32_t mailbox;       // チップからの知らせ（tohostmailboxdata）
    uint64_t rx_frames, rx_events, rx_data, rx_errors, rx_glom;
    uint32_t last_event;
    int clm_status, event_status, country_status, up_status, glom_status;   // wifi_setup の各段の答え
    bool connected;
    char ssid[33];
    uint64_t tx_data, tx_dropped;
    bool irq_mode;          // 受信を割り込みで読んでいる（だめなら 10ms ごとのタイマーだけ）
    uint64_t irqs, irq_storms;
    uint32_t storm_intstatus, storm_hostintmask, storm_cccr_pending, storm_cccr_ien;   // やめたときの状態（調べる用）
    uint32_t event_counts[WIFI_EVENT_TYPES];    // イベントの種類ごとの数（wifi events）
    uint64_t rx_gaps, credit_waits;             // 送っているのに 1 秒受け取らなかった回数、送ってよい番号を 0.2 秒以上待った回数
};

#define WIFI_MAX_BSS 48
struct wifi_bss {
    char ssid[33];
    uint8_t bssid[6];
    int rssi;               // dBm
    int channel;
    uint16_t chanspec;      // ファームウェアの形のチャンネル（スキャンの結果のまま。接続のときに渡す）
    bool privacy, wpa, wpa2;
};

bool wifi_probe(void);      // チップをリセットし直して SDIO でつなぎ、番号を読む
const struct wifi_info *wifi_get_info(void);
// ファームウェアと nvram（SD カードのファイル）をチップに読み込ませて動かし、機能 2 が使えるまで待つ
bool wifi_start(const char *fw_path, const char *nvram_path);
uint32_t wifi_read_console(char *out, uint32_t size);  // ファームウェアのログ（古いものから）

// 制御のコマンド（ファームウェアの WLC_*）と、名前の設定（iovar）。0 で成功
int wifi_ioctl(uint32_t cmd, bool set, void *buf, uint32_t len);
int wifi_iovar_get(const char *name, void *out, uint32_t len);
int wifi_iovar_set(const char *name, const void *val, uint32_t len);

bool wifi_setup(const char *clm_path, const char *country);    // CLM、イベント、国、WLC_UP
int wifi_scan(const struct wifi_bss **list, uint32_t timeout_ms);
// 待たない形（自動の接続 wifi_auto.c が使う）: 始める（0 で成功）、終わっていれば数・まだなら -1
int wifi_scan_begin(void);
int wifi_scan_poll(const struct wifi_bss **list);
void wifi_poll(void);       // 届いている枠を読む

// 接続（WPA2・WPA の PSK、または暗号なし）。0 で成功、負は WIFI_ERR_*
#define WIFI_ERR_NOT_RUNNING    -1
#define WIFI_ERR_BAD_SSID       -2
#define WIFI_ERR_BAD_PASS       -3
#define WIFI_ERR_SETUP          -4
#define WIFI_ERR_AUTH           -5      // 鍵を作れなかった（パスワードが違うなど）
#define WIFI_ERR_TIMEOUT        -6
#define WIFI_ERR_JOIN           -10     // -11: 失敗、-12: 時間切れ、-13: 見つからない
#define WIFI_SEC_WPA2           0       // WPA2（AES）。わからないときもこれ
#define WIFI_SEC_WPA            1       // WPA だけのアクセスポイント（TKIP・AES）
int wifi_connect(const char *ssid, const char *pass, int security, const struct wifi_bss *hint, uint32_t timeout_ms);
// 待たない形: 始める（0 で成功）、0 = つながった・WIFI_PENDING = まだ・負 = WIFI_ERR_*
#define WIFI_PENDING            1
int wifi_connect_begin(const char *ssid, const char *pass, int security, const struct wifi_bss *hint, uint32_t timeout_ms);
int wifi_connect_poll(void);
// 最後の接続の間にファームウェアから届いたイベント（調べる用）
#define WIFI_TRACE_MAX          24
struct wifi_event_rec { uint32_t type, status, reason, flags; };
int wifi_join_trace(const struct wifi_event_rec **list);
void wifi_disconnect(void);
bool wifi_link_up(void);
int wifi_rssi(void);

// データ（os/net/net.c から）
bool wifi_send_eth(const uint8_t *frame, uint32_t len);
void wifi_set_rx_hook(void (*hook)(const uint8_t *frame, uint32_t len));
void wifi_net_poll(void);   // タイマー割り込みから
bool wifi_set_irq_mode(bool on);
// 受信が途切れた・送ってよい番号を待ったことを dmesg に書く（調べる用。割り込みの中では書かず、wifid のコア 3 から呼ぶ）
void wifi_diag_report(void);    // 受信を割り込みで読むか（false なら 10ms ごとのタイマーだけ）
void wifi_handle_irq(void); // EMMC の割り込みから（チップが、読むものがあると知らせた）
