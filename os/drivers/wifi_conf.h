// /etc/wpa_supplicant.conf（os/drivers/wifi_conf.c）
#pragma once

#define WIFI_CONF_PATH  "/etc/wpa_supplicant.conf"
#define WIFI_CONF_MAX   8

struct wifi_network {
    char ssid[33];
    char psk[65];       // パスフレーズ（8〜63 文字）か 16 進の 64 文字
    bool open;          // key_mgmt=NONE
    int priority;
};

struct wifi_conf {
    char country[3];
    int count;
    struct wifi_network net[WIFI_CONF_MAX];
};

// 読めたネットワークの数。ファイルがなければ -1
int wifi_conf_read(const char *path, struct wifi_conf *conf);
