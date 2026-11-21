// IPv4 のアドレスの文字列との変換（Linux と同じ関数。Monocs の実装は os/libc/netdb.c）。picolibc の arpa/inet.h
// （htonl などのマクロだけ）を置き換える
#ifndef _ARPA_INET_H
#define _ARPA_INET_H 1
#define __ARPA_INET_H__     // picolibc のものを読まないように

#include "../netinet/in.h"

#ifdef __cplusplus
extern "C" {
#endif

int inet_pton(int af, const char *restrict src, void *restrict dst);
const char *inet_ntop(int af, const void *restrict src, char *restrict dst, socklen_t size);
int inet_aton(const char *cp, struct in_addr *inp);
in_addr_t inet_addr(const char *cp);
char *inet_ntoa(struct in_addr in);

#ifdef __cplusplus
}
#endif

#endif
