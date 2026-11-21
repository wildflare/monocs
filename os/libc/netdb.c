// 名前とアドレス: getaddrinfo freeaddrinfo gai_strerror getnameinfo gethostbyname getservbyname getservbyport
// hstrerror、inet_pton inet_ntop inet_aton inet_addr inet_ntoa、gethostname（2026-11-14）。IPv4 だけ。
// 名前は DNS で引く（os/net/dns_api.c。サーバーは内蔵 Wi-Fi の DHCP から。10 秒で諦める）。"localhost" は 127.0.0.1
// （ただしループバックはないので、つながらない）。逆引きはしない（getnameinfo は数字の形を返す。NI_NAMEREQD なら
// EAI_NONAME）。サービスの名前は、よく使うものだけの表（/etc/services は読まない）。
// 動きは glibc に合わせた: getaddrinfo の hints の ai_socktype が 0 なら、TCP・UDP・RAW の 3 つを返す。エラーの番号と
// 文字列も glibc と同じ

#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../api/include/netdb.h"
#include "../api/include/arpa/inet.h"

// 名前から IP アドレス（os/net/dns_api.c）。dns_api.h は lwIP のヘッダー（htons などを決める）を読むので、宣言だけ
int dns_api_resolve(const char *name, char *ip, int size, int timeout_ms);

int h_errno;

#define RESOLVE_MS 10000

// ---- アドレスの文字列 ----

// "a.b.c.d"（10 進、それぞれ 0〜255、先頭の 0 は不可。inet_pton と同じく厳しく）
static bool parse_dotted(const char *s, uint32_t *out)
{
    uint32_t a = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*s))
            return false;
        unsigned v = 0;
        int digits = 0;
        const char *start = s;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return false;
        }
        if (digits > 1 && *start == '0')
            return false;
        a = a << 8 | v;
        if (part < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *out = a;
    return true;
}

int inet_pton(int af, const char *restrict src, void *restrict dst)
{
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    uint32_t a;
    if (!parse_dotted(src, &a))
        return 0;
    struct in_addr in = {htonl(a)};
    memcpy(dst, &in, sizeof in);
    return 1;
}

const char *inet_ntop(int af, const void *restrict src, char *restrict dst, socklen_t size)
{
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return nullptr;
    }
    struct in_addr in;
    memcpy(&in, src, sizeof in);
    uint32_t a = ntohl(in.s_addr);
    char buf[INET_ADDRSTRLEN];
    int n = snprintf(buf, sizeof buf, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    if ((socklen_t)n >= size) {
        errno = ENOSPC;
        return nullptr;
    }
    memcpy(dst, buf, (size_t)n + 1);
    return dst;
}

// inet_aton: glibc と同じく a、a.b、a.b.c、a.b.c.d の形と、8 進（0 で始まる）・16 進（0x）も受ける
int inet_aton(const char *cp, struct in_addr *inp)
{
    unsigned long parts[4];
    int n = 0;
    const char *s = cp;
    for (;;) {
        if (!isdigit((unsigned char)*s))
            return 0;
        char *end;
        unsigned long v = strtoul(s, &end, 0);
        if (end == s || n == 4)
            return 0;
        parts[n++] = v;
        s = end;
        if (*s == '.') {
            s++;
            continue;
        }
        if (*s && !isspace((unsigned char)*s))
            return 0;
        break;
    }
    unsigned long a;
    switch (n) {
    case 1: a = parts[0]; if (a > 0xffffffffUL) return 0; break;
    case 2: if (parts[0] > 255 || parts[1] > 0xffffff) return 0; a = parts[0] << 24 | parts[1]; break;
    case 3: if (parts[0] > 255 || parts[1] > 255 || parts[2] > 0xffff) return 0;
            a = parts[0] << 24 | parts[1] << 16 | parts[2]; break;
    default: if (parts[0] > 255 || parts[1] > 255 || parts[2] > 255 || parts[3] > 255) return 0;
             a = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]; break;
    }
    if (inp)
        inp->s_addr = htonl((uint32_t)a);
    return 1;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr in;
    return inet_aton(cp, &in) ? in.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in, buf, sizeof buf);
    return buf;
}

int gethostname(char *name, size_t len)
{
    static const char host[] = "monocs";
    if (len < sizeof host) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, host, sizeof host);
    return 0;
}

// ---- サービス ----

static const struct { const char *name; int port; } services[] = {
    {"echo", 7}, {"discard", 9}, {"daytime", 13}, {"ftp-data", 20}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23},
    {"smtp", 25}, {"time", 37}, {"domain", 53}, {"http", 80}, {"pop3", 110}, {"ntp", 123}, {"imap", 143},
    {"snmp", 161}, {"ldap", 389}, {"https", 443}, {"submission", 587}, {"imaps", 993}, {"pop3s", 995},
    {"mqtt", 1883}, {"http-alt", 8080},
};

static int service_port(const char *name)
{
    for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
        if (!strcmp(services[i].name, name))
            return services[i].port;
    return -1;
}

static const char *service_name(int port)
{
    for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
        if (services[i].port == port)
            return services[i].name;
    return nullptr;
}

struct servent *getservbyname(const char *name, const char *proto)
{
    static struct servent se;
    static char *aliases[1];
    static char pname[8];
    int port = service_port(name);
    if (port < 0)
        return nullptr;
    snprintf(pname, sizeof pname, "%s", proto ? proto : "tcp");
    se = (struct servent){(char *)name, aliases, (int)htons((uint16_t)port), pname};
    return &se;
}

struct servent *getservbyport(int port, const char *proto)
{
    const char *name = service_name(ntohs((uint16_t)port));
    return name ? getservbyname(name, proto) : nullptr;
}

// ---- 名前 ----

// name を IPv4 のアドレス（ホストの順）に。0 か EAI_*
static int lookup(const char *name, bool numeric_only, uint32_t *out)
{
    struct in_addr in;
    if (inet_aton(name, &in)) {
        *out = ntohl(in.s_addr);
        return 0;
    }
    if (numeric_only)
        return EAI_NONAME;
    if (!strcmp(name, "localhost")) {
        *out = INADDR_LOOPBACK;
        return 0;
    }
    char ip[16];
    if (dns_api_resolve(name, ip, sizeof ip, RESOLVE_MS) != 0)
        return EAI_NONAME;
    if (!inet_aton(ip, &in))
        return EAI_FAIL;
    *out = ntohl(in.s_addr);
    return 0;
}

int getaddrinfo(const char *restrict node, const char *restrict service, const struct addrinfo *restrict hints,
                struct addrinfo **restrict res)
{
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    *res = nullptr;
    if (!node && !service)
        return EAI_NONAME;
    if (family != AF_UNSPEC && family != AF_INET)
        return family == AF_INET6 ? EAI_ADDRFAMILY : EAI_FAMILY;
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM && socktype != SOCK_RAW)
        return EAI_SOCKTYPE;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV))
        return EAI_BADFLAGS;
    if ((flags & AI_CANONNAME) && !node)
        return EAI_BADFLAGS;

    int port = 0;
    bool named_service = false;     // サービスを名前で（"http"）指定した（RAW を返さない。glibc と同じ）
    if (service && *service) {
        char *end;
        unsigned long v = strtoul(service, &end, 10);
        if (*end == '\0') {
            port = (uint16_t)v;     // glibc と同じく、範囲を調べずに下の 16 ビット
        } else if (flags & AI_NUMERICSERV) {
            return EAI_NONAME;
        } else if (socktype == SOCK_RAW) {
            return EAI_SERVICE;
        } else if ((port = service_port(service)) < 0) {
            return EAI_SERVICE;
        } else {
            named_service = true;
        }
    }
    uint32_t addr;
    if (!node) {
        addr = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    } else {
        int e = lookup(node, flags & AI_NUMERICHOST, &addr);
        if (e)
            return e;
    }

    // ai_socktype が 0 なら、TCP・UDP・RAW の 3 つ（glibc と同じ。サービスを名前で指定したときは RAW を除く）
    static const int types[][2] = {{SOCK_STREAM, IPPROTO_TCP}, {SOCK_DGRAM, IPPROTO_UDP}, {SOCK_RAW, 0}};
    struct addrinfo **tail = res;
    for (int i = 0; i < 3; i++) {
        if (socktype ? types[i][0] != socktype : (types[i][0] == SOCK_RAW && named_service))
            continue;
        if (protocol && types[i][1] && protocol != types[i][1])
            continue;
        size_t canon = (flags & AI_CANONNAME) && !*res ? strlen(node) + 1 : 0;
        struct addrinfo *ai = calloc(1, sizeof *ai + sizeof(struct sockaddr_in) + canon);
        if (!ai) {
            freeaddrinfo(*res);
            *res = nullptr;
            return EAI_MEMORY;
        }
        struct sockaddr_in *in = (struct sockaddr_in *)(ai + 1);
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)port);
        in->sin_addr.s_addr = htonl(addr);
        ai->ai_family = AF_INET;
        ai->ai_socktype = types[i][0];
        ai->ai_protocol = socktype == SOCK_RAW && protocol ? protocol : types[i][1];
        ai->ai_addrlen = sizeof *in;
        ai->ai_addr = (struct sockaddr *)in;
        if (canon) {
            ai->ai_canonname = (char *)(in + 1);
            memcpy(ai->ai_canonname, node, canon);
        }
        *tail = ai;
        tail = &ai->ai_next;
    }
    return *res ? 0 : EAI_SOCKTYPE;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

const char *gai_strerror(int code)
{
    switch (code) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_NODATA: return "No address associated with hostname";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_ADDRFAMILY: return "Address family for hostname not supported";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown error";
    }
}

int getnameinfo(const struct sockaddr *restrict addr, socklen_t addrlen, char *restrict host, socklen_t hostlen,
                char *restrict serv, socklen_t servlen, int flags)
{
    if (!addr || addr->sa_family != AF_INET || addrlen < sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    if (host && hostlen) {
        if (flags & NI_NAMEREQD)
            return EAI_NONAME;  // 逆引きはしない
        if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen))
            return EAI_OVERFLOW;
    }
    if (serv && servlen) {
        int port = ntohs(in->sin_port);
        const char *name = (flags & NI_NUMERICSERV) ? nullptr : service_name(port);
        int n = name ? snprintf(serv, servlen, "%s", name) : snprintf(serv, servlen, "%d", port);
        if (n < 0 || (socklen_t)n >= servlen)
            return EAI_OVERFLOW;
    }
    return 0;
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static struct in_addr addr;
    static char *addrs[2], *aliases[1];
    static char hname[256];
    uint32_t a;
    if (!name || lookup(name, false, &a) != 0) {
        h_errno = HOST_NOT_FOUND;
        return nullptr;
    }
    addr.s_addr = htonl(a);
    addrs[0] = (char *)&addr;
    addrs[1] = nullptr;
    aliases[0] = nullptr;
    snprintf(hname, sizeof hname, "%s", name);
    he = (struct hostent){hname, aliases, AF_INET, 4, addrs};
    return &he;
}

const char *hstrerror(int err)
{
    switch (err) {
    case 0: return "Resolver Error 0 (no error)";
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN: return "Host name lookup failure";
    case NO_RECOVERY: return "Unknown server error";
    case NO_DATA: return "No address associated with name";
    default: return "Unknown resolver error";
    }
}
