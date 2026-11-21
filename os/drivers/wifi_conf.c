// /etc/wpa_supplicant.conf を読む（Linux の wpa_supplicant と同じ形。使うのはその一部）
//   country=JP
//   network={
//       ssid="名前"
//       psk="パスフレーズ"        （または 16 進の 64 文字。key_mgmt=NONE なら暗号なし）
//       priority=1
//   }
// 出典: wpa_supplicant の wpa_supplicant.conf（例の文書）の書き方。# からは注釈、値は "..." か、そのまま

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_conf.h"

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

// 値: "..." なら中身（\" などの書き方は使わない）、ほかはそのまま
static void value(const char *v, char *out, size_t size)
{
    size_t n;
    if (v[0] == '"') {
        const char *q = strrchr(v + 1, '"');
        n = q ? (size_t)(q - v - 1) : strlen(v + 1);
        v++;
    } else {
        n = strlen(v);
    }
    if (n >= size)
        n = size - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

int wifi_conf_read(const char *path, struct wifi_conf *conf)
{
    memset(conf, 0, sizeof *conf);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    struct wifi_network *cur = nullptr;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == '#' || *s == 0)
            continue;
        if (!cur && !strncmp(s, "network", 7)) {
            if (conf->count < WIFI_CONF_MAX) {
                cur = &conf->net[conf->count++];
                memset(cur, 0, sizeof *cur);
            } else {
                static struct wifi_network dummy;
                cur = &dummy;
            }
            continue;
        }
        if (cur && *s == '}') {
            cur = nullptr;
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (!cur) {
            if (!strcmp(key, "country"))
                value(val, conf->country, sizeof conf->country);
            continue;
        }
        if (!strcmp(key, "ssid"))
            value(val, cur->ssid, sizeof cur->ssid);
        else if (!strcmp(key, "psk"))
            value(val, cur->psk, sizeof cur->psk);
        else if (!strcmp(key, "key_mgmt"))
            cur->open = !strcmp(val, "NONE");
        else if (!strcmp(key, "priority"))
            cur->priority = atoi(val);
    }
    fclose(f);
    return conf->count;
}
