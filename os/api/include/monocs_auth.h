// monocs_auth.h: Wi-Fi から受けるサービスのパスワード（HTTP・telnet のサービスと passwd コマンドが使う）
//
// パスワードそのものは保存しない。保存するのは /etc/htdigest の 1 行（Apache の htdigest と同じ形）:
//     ユーザー名:Monocs:MD5("ユーザー名:Monocs:パスワード") の 16 進 32 桁
// これは HTTP のダイジェスト認証（RFC 7616・2617）の HA1 そのもの。telnet は、打たれたユーザー名とパスワードから
// 同じ値を作って比べる。Pi 5 で作るなら `htdigest -c htdigest Monocs ユーザー名`。Monocs では `passwd`。
// 注意: この 1 行が読めれば、パスワードを知らなくても HTTP の認証は通る（ダイジェスト認証の性質）。
//
// MD5 は RFC 1321 の手順どおりに書いたもの（ダイジェスト認証でどのブラウザ・curl でも使えるのが MD5 なので）。
// 関数はすべて static（このヘッダーを読んだファイルごとに持つ）。
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MONOCS_AUTH_FILE
#define MONOCS_AUTH_FILE  "/etc/htdigest"
#endif
#define MONOCS_AUTH_REALM "Monocs"

// ---- MD5（RFC 1321）----
typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;           // これまでのバイト数
    uint8_t buf[64];
} MonocsMd5;

static void monocs_md5_block(MonocsMd5 *m, const uint8_t *p)
{
    // 各回の回転の量と、定数 T[i] = floor(2^32 × |sin(i + 1)|)（RFC 1321 の 3.4）
    static const uint8_t rot[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static const uint32_t T[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f += a + T[i] + x[g];
        a = d;
        d = c;
        c = b;
        b += f << rot[i] | f >> (32 - rot[i]);
    }
    m->a += a;
    m->b += b;
    m->c += c;
    m->d += d;
}

static void monocs_md5_init(MonocsMd5 *m)
{
    m->a = 0x67452301;
    m->b = 0xefcdab89;
    m->c = 0x98badcfe;
    m->d = 0x10325476;
    m->len = 0;
}

static void monocs_md5_update(MonocsMd5 *m, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n > 0) {
        size_t used = m->len % 64, k = 64 - used;
        if (k > n)
            k = n;
        memcpy(m->buf + used, p, k);
        m->len += k;
        p += k;
        n -= k;
        if (m->len % 64 == 0)
            monocs_md5_block(m, m->buf);
    }
}

// 結果を 16 進 32 桁の文字列で（hex は 33 バイト以上）
static void monocs_md5_final(MonocsMd5 *m, char *hex)
{
    uint64_t bits = m->len * 8;
    uint8_t pad[72] = {0x80};
    size_t padlen = (m->len % 64 < 56 ? 56 : 120) - m->len % 64;
    for (int i = 0; i < 8; i++)
        pad[padlen + i] = (uint8_t)(bits >> (8 * i));
    monocs_md5_update(m, pad, padlen + 8);
    uint32_t v[4] = {m->a, m->b, m->c, m->d};
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", (unsigned)(v[i / 4] >> (8 * (i % 4))) & 0xFF);
}

// 文字列を「:」でつないだものの MD5（NULL で終わり）。ダイジェスト認証の計算はどれもこの形
static void monocs_md5_join(char *hex, const char *s, ...) __attribute__((sentinel));
static void monocs_md5_join(char *hex, const char *s, ...)
{
    MonocsMd5 m;
    monocs_md5_init(&m);
    __builtin_va_list ap;
    __builtin_va_start(ap, s);
    for (bool first = true; s; s = __builtin_va_arg(ap, const char *), first = false) {
        if (!first)
            monocs_md5_update(&m, ":", 1);
        monocs_md5_update(&m, s, strlen(s));
    }
    __builtin_va_end(ap);
    monocs_md5_final(&m, hex);
}

// ---- パスワードのファイル ----

// ユーザー名とパスワードから、保存する値（HA1。ha1 は 33 バイト以上）
static void monocs_auth_ha1(const char *user, const char *pass, char *ha1)
{
    monocs_md5_join(ha1, user, MONOCS_AUTH_REALM, pass, nullptr);
}

// /etc/htdigest の最初の行を読む（user は 64 バイト以上、ha1 は 33 バイト以上）。あれば true
static bool monocs_auth_load(char *user, char *ha1)
{
    FILE *f = fopen(MONOCS_AUTH_FILE, "r");
    if (!f)
        return false;
    char line[160];
    bool ok = false;
    while (!ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
        if (!c2 || c1 == line || c1 - line >= 64 || strlen(c2 + 1) != 32 ||
            strncmp(c1 + 1, MONOCS_AUTH_REALM, (size_t)(c2 - c1 - 1)) || c2 - c1 - 1 != (int)strlen(MONOCS_AUTH_REALM))
            continue;
        memcpy(user, line, (size_t)(c1 - line));
        user[c1 - line] = 0;
        strcpy(ha1, c2 + 1);
        ok = true;
    }
    fclose(f);
    return ok;
}

// 文字列を比べる（どこで違ったかが時間に出ないように、最後まで比べる）。同じなら true
static bool monocs_auth_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la != lb;
    for (size_t i = 0; i < la && i < lb; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}
