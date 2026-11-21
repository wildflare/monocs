// TCP のオプション（Linux と同じ値。setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...)）。
// Monocs の TCP はいつも Nagle を使わない（TCP_NODELAY と同じ）。ほかは受け取るだけ
#ifndef _NETINET_TCP_H
#define _NETINET_TCP_H 1

#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_CORK        3
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6

#endif
