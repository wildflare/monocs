// 内蔵 Wi-Fi をつなぐ手順と、自動の接続（os/drivers/wifi_auto.c）
#pragma once

#include "wifi.h"
#include "wifi_conf.h"

// シェルの wifi コマンドと自動の接続が重ならないためのロック
void wifi_manage_lock(void);
void wifi_manage_unlock(void);

bool wifi_bringup(bool verbose);    // チップを動かす（動いていれば何もしない）
// 設定のネットワークを試す順に cand（WIFI_CONF_MAX 個）へ。入れた数
int wifi_pick(const struct wifi_conf *conf, const struct wifi_bss *list, int n, struct wifi_network *cand,
              bool visible_only);
int wifi_security_of(const struct wifi_bss *list, int n, const char *ssid);    // WIFI_SEC_*
const struct wifi_bss *wifi_bss_of(const struct wifi_bss *list, int n, const char *ssid);
bool wifi_net_up(void);             // つながった後: lwIP の口を上げて DHCP を始める

// 自動の接続。サービス（services/wifid.c）が呼び続ける。待たない。何かしたら 1
int wifi_auto_poll(void);
// シェルが自分で操作したとき: 自動の接続を最初からやり直させる。hold_off なら、次の wifi connect まで止める
void wifi_auto_defer(bool hold_off);
const char *wifi_auto_state(void);
int wifi_auto_last(int *count);     // 最後の自動の接続の結果（0・WIFI_ERR_*）と、試した回数
