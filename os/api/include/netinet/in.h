// IPv4 のアドレスとポート（Linux（glibc 2.41）と同じ形。os/api/include/sys/socket.h）
#ifndef _NETINET_IN_H
#define _NETINET_IN_H 1

#include <stdint.h>
#include "../sys/socket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;             // ネットワークの順（htons）
    struct in_addr sin_addr;        // ネットワークの順（htonl）
    unsigned char sin_zero[8];
};

// IPv6 の型（Monocs は IPv6 を使わない。型だけ。AF_INET6 のソケットは EAFNOSUPPORT）
struct in6_addr {
    union {
        uint8_t __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};
#define s6_addr __in6_u.__u6_addr8

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_RAW     255

#define IP_TOS              1
#define IP_TTL              2
#define IP_MULTICAST_TTL    33
#define IP_ADD_MEMBERSHIP   35

#define INADDR_ANY          ((in_addr_t)0x00000000)
#define INADDR_BROADCAST    ((in_addr_t)0xffffffff)
#define INADDR_NONE         ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK     ((in_addr_t)0x7f000001)

#define INET_ADDRSTRLEN     16
#define INET6_ADDRSTRLEN    46

// バイトの順（ネットワークはビッグエンディアン、Monocs（AArch64）はリトルエンディアン）。picolibc の endian.h の
// __bswap32 は __builtin_bswap32 を使い、Monocs のコンパイラ（slimcc）にはないので、ここで書く。glibc と同じく関数の形
static inline uint16_t __monocs_bswap16(uint16_t x)
{
    return (uint16_t)(x << 8 | x >> 8);
}
static inline uint32_t __monocs_bswap32(uint32_t x)
{
    return x << 24 | (x & 0xff00) << 8 | (x >> 8 & 0xff00) | x >> 24;
}
#undef htons
#undef htonl
#undef ntohs
#undef ntohl
#define htons(x) __monocs_bswap16((uint16_t)(x))
#define ntohs(x) __monocs_bswap16((uint16_t)(x))
#define htonl(x) __monocs_bswap32((uint32_t)(x))
#define ntohl(x) __monocs_bswap32((uint32_t)(x))

#ifdef __cplusplus
}
#endif

#endif
