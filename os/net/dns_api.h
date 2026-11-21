// 名前から IP アドレスを引く（os/net/dns_api.c。アプリからは monocs_resolve）
#pragma once

#include "lwip/ip_addr.h"

// name（"192.168.7.1" の形ならそのまま、ほかは DNS で引く）を addr に。0 = 成功、-1 = 引けない・時間切れ。
// timeout_ms: 0 = 待たない（問い合わせは始まる。引けたら lwIP が覚えているので、次は成功する）、負 = 答えが出るまで
int dns_api_lookup(const char *name, ip_addr_t *addr, int timeout_ms);
// 文字列で返す形（ip は 16 バイト以上）
int dns_api_resolve(const char *name, char *ip, int size, int timeout_ms);
