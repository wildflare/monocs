// TLS でつなぐ（monocs_tls.h）。BearSSL（third_party/bearssl、MIT。SD カードの /lib/bearssl）の上の薄い層
//
// - TCP は monocs_tcp_*。BearSSL のエンジン（br_ssl_engine_*）の 4 つの口（送るレコード・受けるレコード・
//   送るデータ・受けたデータ）を自分で回す（br_sslio は使わない。時間切れを付けるため）。
// - 信頼の起点（認証局）: PEM のファイルを br_pem_decoder で DER にし、br_x509_decoder で名前（DN）と公開鍵を
//   取り出して br_x509_trust_anchor の表にする。検証は br_x509_minimal（名前の照合、期限、署名の鎖）。
// 出典: BearSSL の inc/bearssl_ssl.h・bearssl_x509.h・bearssl_pem.h の説明。
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <monocs.h>
#include <inc/bearssl.h>
#include "monocs_tls.h"

struct MonocsTls {
    int tcp;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

static char error_text[160];
static const char *ca_file = "/etc/ssl/certs/ca-certificates.crt";

const char *monocs_tls_error(void)
{
    return error_text;
}

// ---- 信頼の起点 ----
static br_x509_trust_anchor *anchors;
static size_t nanchors, cap_anchors;

void monocs_tls_set_ca_file(const char *path)
{
    ca_file = path;
    nanchors = 0;   // 読み直す（前の分のメモリはアプリの終わりに戻る）
}

typedef struct {
    unsigned char *p;
    size_t len, cap;
    bool failed;
} Bytes;

static void bytes_append(void *ctx, const void *src, size_t len)
{
    Bytes *b = ctx;
    if (b->len + len > b->cap) {
        size_t cap = (b->len + len) * 2 + 256;
        unsigned char *p = realloc(b->p, cap);
        if (!p) {
            b->failed = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, src, len);
    b->len += len;
}

static unsigned char *dup_bytes(const unsigned char *p, size_t n)
{
    unsigned char *q = malloc(n ? n : 1);
    if (q)
        memcpy(q, p, n);
    return q;
}

// DER の証明書 1 つを信頼の起点に足す（読めないものは飛ばす）
static void add_anchor(const unsigned char *der, size_t len)
{
    static br_x509_decoder_context dc;
    Bytes dn = {0};
    br_x509_decoder_init(&dc, bytes_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || dn.failed) {
        free(dn.p);
        return;
    }
    if (nanchors == cap_anchors) {
        size_t cap = cap_anchors ? cap_anchors * 2 : 192;
        br_x509_trust_anchor *a = realloc(anchors, cap * sizeof *a);
        if (!a) {
            free(dn.p);
            return;
        }
        anchors = a;
        cap_anchors = cap;
    }
    br_x509_trust_anchor *ta = &anchors[nanchors];
    memset(ta, 0, sizeof *ta);
    ta->dn.data = dn.p;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    bool ok = true;
    if (pk->key_type == BR_KEYTYPE_RSA) {   // 鍵は decoder の中を指しているので、写しておく
        ta->pkey.key.rsa.n = dup_bytes(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = dup_bytes(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        ok = ta->pkey.key.rsa.n && ta->pkey.key.rsa.e;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = dup_bytes(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        ok = ta->pkey.key.ec.q != nullptr;
    } else {
        ok = false;
    }
    if (ok)
        nanchors++;
    else
        free(dn.p);
}

// ---- 読み解いた信頼の起点を覚えておく（/.cache/tls/anchors.bin）----
// PEM と X.509 を読み解くのは遅い（実機で 148 個に約 2 秒。BearSSL の読み解きは小さな仮想機械で動く）ので、
// 結果（名前と公開鍵）を書いておき、認証局のファイルが同じ（大きさと FNV-1a のハッシュ）なら、それを読む。
// 形: "UTA1"、ファイルの大きさ、ハッシュ、数、続いて 1 つずつ { flags, 種類, 曲線, dn・a・b の長さ, その中身 }
// （a・b は RSA なら n・e、EC なら q・なし。数はすべて 32 ビットのリトルエンディアン）
#define CACHE_DIR   "/var/cache/tls"
#define CACHE_FILE  CACHE_DIR "/anchors.bin"

static uint32_t get32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(Bytes *b, uint32_t v)
{
    unsigned char t[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    bytes_append(b, t, 4);
}

// 認証局のファイルを丸ごと読み、ハッシュを返す（data は呼んだ側が free）
static bool read_ca(unsigned char **data, size_t *size, uint32_t *hash)
{
    FILE *f = fopen(ca_file, "rb");
    if (!f) {
        snprintf(error_text, sizeof error_text, "cannot open the CA certificates: %s: %s", ca_file, strerror(errno));
        return false;
    }
    Bytes b = {0};
    static unsigned char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        bytes_append(&b, buf, n);
    fclose(f);
    if (b.failed) {
        free(b.p);
        snprintf(error_text, sizeof error_text, "out of memory");
        return false;
    }
    uint32_t h = 2166136261u;       // FNV-1a
    for (size_t i = 0; i < b.len; i++)
        h = (h ^ b.p[i]) * 16777619u;
    *data = b.p;
    *size = b.len;
    *hash = h;
    return true;
}

static bool cache_load(size_t ca_size, uint32_t ca_hash)
{
    FILE *f = fopen(CACHE_FILE, "rb");
    if (!f)
        return false;
    Bytes b = {0};
    static unsigned char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        bytes_append(&b, buf, n);
    fclose(f);
    bool ok = !b.failed && b.len >= 16 && !memcmp(b.p, "UTA1", 4) && get32(b.p + 4) == (uint32_t)ca_size &&
              get32(b.p + 8) == ca_hash;
    uint32_t count = ok ? get32(b.p + 12) : 0;
    br_x509_trust_anchor *list = ok && count > 0 && count < 4096 ? calloc(count, sizeof *list) : nullptr;
    ok = list != nullptr;
    size_t pos = 16;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (b.len - pos < 24) {
            ok = false;
            break;
        }
        const unsigned char *h = b.p + pos;
        uint32_t dl = get32(h + 12), al = get32(h + 16), bl = get32(h + 20);
        pos += 24;
        if (dl > b.len - pos || al > b.len - pos - dl || bl > b.len - pos - dl - al) {
            ok = false;
            break;
        }
        // 中身は読んだ塊（b.p。解放しない）の中を指す
        br_x509_trust_anchor *ta = &list[i];
        ta->flags = get32(h);
        ta->pkey.key_type = (unsigned char)get32(h + 4);
        ta->dn.data = b.p + pos;
        ta->dn.len = dl;
        if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
            ta->pkey.key.rsa.n = b.p + pos + dl;
            ta->pkey.key.rsa.nlen = al;
            ta->pkey.key.rsa.e = b.p + pos + dl + al;
            ta->pkey.key.rsa.elen = bl;
        } else if (ta->pkey.key_type == BR_KEYTYPE_EC) {
            ta->pkey.key.ec.curve = (int)get32(h + 8);
            ta->pkey.key.ec.q = b.p + pos + dl;
            ta->pkey.key.ec.qlen = al;
        } else {
            ok = false;
        }
        pos += dl + al + bl;
    }
    if (!ok) {
        free(list);
        free(b.p);
        return false;
    }
    anchors = list;
    nanchors = cap_anchors = count;
    return true;
}

static void cache_save(size_t ca_size, uint32_t ca_hash)
{
    Bytes b = {0};
    bytes_append(&b, "UTA1", 4);
    put32(&b, (uint32_t)ca_size);
    put32(&b, ca_hash);
    put32(&b, (uint32_t)nanchors);
    for (size_t i = 0; i < nanchors; i++) {
        const br_x509_trust_anchor *ta = &anchors[i];
        bool rsa = ta->pkey.key_type == BR_KEYTYPE_RSA;
        const unsigned char *a = rsa ? ta->pkey.key.rsa.n : ta->pkey.key.ec.q;
        size_t al = rsa ? ta->pkey.key.rsa.nlen : ta->pkey.key.ec.qlen, bl = rsa ? ta->pkey.key.rsa.elen : 0;
        put32(&b, ta->flags);
        put32(&b, ta->pkey.key_type);
        put32(&b, rsa ? 0 : (uint32_t)ta->pkey.key.ec.curve);
        put32(&b, (uint32_t)ta->dn.len);
        put32(&b, (uint32_t)al);
        put32(&b, (uint32_t)bl);
        bytes_append(&b, ta->dn.data, ta->dn.len);
        bytes_append(&b, a, al);
        if (rsa)
            bytes_append(&b, ta->pkey.key.rsa.e, bl);
    }
    if (!b.failed) {    // 書けなくてもよい（次も読み解くだけ）。途中で切れたものを残さないよう、書いてから置き換える
        mkdir("/var/cache", 0755);
        mkdir(CACHE_DIR, 0755);
        FILE *f = fopen(CACHE_FILE ".tmp", "wb");
        if (f) {
            bool ok = fwrite(b.p, 1, b.len, f) == b.len;
            ok = fclose(f) == 0 && ok;
            if (!ok || rename(CACHE_FILE ".tmp", CACHE_FILE) != 0)
                unlink(CACHE_FILE ".tmp");
        }
    }
    free(b.p);
}

// PEM のファイルの中の証明書（CERTIFICATE・X509 CERTIFICATE）をすべて読む。読めた数（0 ならだめ）
static size_t load_anchors(void)
{
    if (nanchors)
        return nanchors;
    unsigned char *ca;
    size_t ca_size;
    uint32_t ca_hash;
    if (!read_ca(&ca, &ca_size, &ca_hash))
        return 0;
    if (cache_load(ca_size, ca_hash)) {
        free(ca);
        return nanchors;
    }
    static br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    Bytes der = {0};
    bool in_cert = false;
    {
        const unsigned char *p = ca;
        size_t n = ca_size;
        while (n > 0) {
            size_t k = br_pem_decoder_push(&pc, p, n);
            p += k;
            n -= k;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ: {
                const char *name = br_pem_decoder_name(&pc);
                in_cert = !strcmp(name, "CERTIFICATE") || !strcmp(name, "X509 CERTIFICATE");
                der.len = 0;
                der.failed = false;
                br_pem_decoder_setdest(&pc, in_cert ? bytes_append : nullptr, &der);
                break;
            }
            case BR_PEM_END_OBJ:
                if (in_cert && !der.failed)
                    add_anchor(der.p, der.len);
                in_cert = false;
                break;
            case BR_PEM_ERROR:
                n = 0;      // この塊の残りは捨てる（次の BEGIN から読み直すことはできないので、ここまで）
                break;
            }
        }
    }
    free(ca);
    free(der.p);
    if (!nanchors)
        snprintf(error_text, sizeof error_text, "no CA certificate in %s", ca_file);
    else
        cache_save(ca_size, ca_hash);
    return nanchors;
}

// ---- エンジンを回す ----

static const char *engine_error(int err)
{
    switch (err) {
    case BR_ERR_X509_EXPIRED: return "the certificate has expired or is not yet valid (is the clock right?)";
    case BR_ERR_X509_NOT_TRUSTED: return "the certificate is not signed by a trusted CA";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate does not match the host name";
    case BR_ERR_X509_TIME_UNKNOWN: return "the clock is not set";
    case BR_ERR_BAD_VERSION: return "the server does not support TLS 1.2";
    case BR_ERR_NO_RANDOM: return "no random numbers";
    case BR_ERR_IO: return "connection closed";
    default: return nullptr;
    }
}

static void set_engine_error(MonocsTls *t, const char *what)
{
    int err = br_ssl_engine_last_error(&t->sc.eng);
    const char *s = engine_error(err);
    if (s)
        snprintf(error_text, sizeof error_text, "%s: %s", what, s);
    else if (err >= BR_ERR_SEND_FATAL_ALERT)
        snprintf(error_text, sizeof error_text, "%s: sent TLS alert %d", what, err - BR_ERR_SEND_FATAL_ALERT);
    else if (err >= BR_ERR_RECV_FATAL_ALERT)
        snprintf(error_text, sizeof error_text, "%s: received TLS alert %d (40 = no common cipher or version)", what,
                 err - BR_ERR_RECV_FATAL_ALERT);
    else
        snprintf(error_text, sizeof error_text, "%s: TLS error %d", what, err);
}

static int left_ms(uint64_t start, int timeout_ms)
{
    if (timeout_ms < 0)
        return -1;
    uint64_t used = (timer_now_us() - start) / 1000;
    return used >= (uint64_t)timeout_ms ? 0 : timeout_ms - (int)used;
}

// target（BR_SSL_SENDAPP・BR_SSL_RECVAPP）の口が開くまで、レコードを送り受けする。
// 0 = 開いた、1 = 閉じた（エラーなし）、-1 = 時間切れ、-2 = エラー
static int run_until(MonocsTls *t, unsigned target, int timeout_ms)
{
    uint64_t start = timer_now_us();
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&t->sc.eng);
        if (st & BR_SSL_CLOSED)
            return br_ssl_engine_last_error(&t->sc.eng) == BR_ERR_OK ? 1 : -2;
        if (st & BR_SSL_SENDREC) {      // 送るものが先（相手はこれを待っている）
            size_t len;
            unsigned char *p = br_ssl_engine_sendrec_buf(&t->sc.eng, &len);
            int ms = left_ms(start, timeout_ms);
            int n = monocs_tcp_send(t->tcp, p, (int)len, ms == 0 ? 1 : ms);
            if (n < 0) {
                snprintf(error_text, sizeof error_text, "connection lost");
                return -2;
            }
            if (n == 0 && ms == 0)
                return -1;
            br_ssl_engine_sendrec_ack(&t->sc.eng, (size_t)n);
            continue;
        }
        if (st & target)
            return 0;
        if (st & BR_SSL_RECVAPP)        // 送りたいのに、読まれていないデータがある（このライブラリの使い方では起きない）
            return -2;
        if (st & BR_SSL_RECVREC) {
            size_t len;
            unsigned char *p = br_ssl_engine_recvrec_buf(&t->sc.eng, &len);
            int ms = left_ms(start, timeout_ms);
            int n = monocs_tcp_recv(t->tcp, p, (int)len, ms);
            if (n == -1)
                return -1;
            if (n <= 0) {               // TCP が閉じた: close_notify なしの終わり
                snprintf(error_text, sizeof error_text, "connection closed");
                return n == 0 ? 1 : -2;
            }
            br_ssl_engine_recvrec_ack(&t->sc.eng, (size_t)n);
            continue;
        }
        br_ssl_engine_flush(&t->sc.eng, 0);
    }
}

MonocsTls *monocs_tls_connect(const char *host, int port, int timeout_ms)
{
    uint64_t start = timer_now_us();
    error_text[0] = 0;
    if (!load_anchors())
        return nullptr;
    time_t now = time(nullptr);
    if (!monocs_clock_known() || now < 1600000000) {
        snprintf(error_text, sizeof error_text, "the clock is not set (certificates cannot be checked)");
        return nullptr;
    }
    unsigned char seed[32];
    if (getentropy(seed, sizeof seed) != 0) {
        snprintf(error_text, sizeof error_text, "no random numbers (getentropy failed)");
        return nullptr;
    }
    MonocsTls *t = malloc(sizeof *t);
    if (!t) {
        snprintf(error_text, sizeof error_text, "out of memory");
        return nullptr;
    }
    t->tcp = monocs_tcp_connect(host, port, timeout_ms);
    if (t->tcp < 0) {
        snprintf(error_text, sizeof error_text, "cannot connect to %s:%d", host, port);
        free(t);
        return nullptr;
    }
    br_ssl_client_init_full(&t->sc, &t->xc, anchors, nanchors);
    // 日時: 0 年 1 月 1 日からの日数（1970-01-01 は 719528）と、その日の秒（bearssl_x509.h）
    br_x509_minimal_set_time(&t->xc, (uint32_t)(now / 86400) + 719528, (uint32_t)(now % 86400));
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof t->iobuf, 1);
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    if (!br_ssl_client_reset(&t->sc, host, 0)) {
        set_engine_error(t, host);
        monocs_tls_close(t);
        return nullptr;
    }
    int r = run_until(t, BR_SSL_SENDAPP, left_ms(start, timeout_ms));   // ハンドシェイクが済むと、送る口が開く
    if (r != 0) {
        if (r == -1)
            snprintf(error_text, sizeof error_text, "%s: TLS handshake timed out", host);
        else if (br_ssl_engine_last_error(&t->sc.eng) != BR_ERR_OK)
            set_engine_error(t, host);
        else if (!error_text[0])
            snprintf(error_text, sizeof error_text, "%s: connection closed during the TLS handshake", host);
        monocs_tcp_close(t->tcp);
        free(t);
        return nullptr;
    }
    return t;
}

int monocs_tls_send(MonocsTls *t, const void *buf, int len)
{
    const unsigned char *p = buf;
    int left = len;
    while (left > 0) {
        if (run_until(t, BR_SSL_SENDAPP, 30000) != 0)
            return -1;
        size_t n;
        unsigned char *q = br_ssl_engine_sendapp_buf(&t->sc.eng, &n);
        if (n > (size_t)left)
            n = (size_t)left;
        memcpy(q, p, n);
        br_ssl_engine_sendapp_ack(&t->sc.eng, n);
        p += n;
        left -= (int)n;
    }
    br_ssl_engine_flush(&t->sc.eng, 0);
    // たまった分を送り切る（送るレコードがなくなるまで）
    return run_until(t, BR_SSL_SENDAPP, 30000) == 0 ? len : -1;
}

int monocs_tls_recv(MonocsTls *t, void *buf, int len, int timeout_ms)
{
    if (len <= 0)
        return 0;
    int r = run_until(t, BR_SSL_RECVAPP, timeout_ms);
    if (r == 1)
        return 0;
    if (r < 0) {
        if (r == -2 && br_ssl_engine_last_error(&t->sc.eng) != BR_ERR_OK)
            set_engine_error(t, "receive");
        return r;
    }
    size_t n;
    unsigned char *p = br_ssl_engine_recvapp_buf(&t->sc.eng, &n);
    if (n > (size_t)len)
        n = (size_t)len;
    memcpy(buf, p, n);
    br_ssl_engine_recvapp_ack(&t->sc.eng, n);
    return (int)n;
}

void monocs_tls_close(MonocsTls *t)
{
    if (!t)
        return;
    // close_notify を送る（相手の返事は待たない）
    br_ssl_engine_close(&t->sc.eng);
    for (int i = 0; i < 4; i++) {
        unsigned st = br_ssl_engine_current_state(&t->sc.eng);
        if (!(st & BR_SSL_SENDREC))
            break;
        size_t len;
        unsigned char *p = br_ssl_engine_sendrec_buf(&t->sc.eng, &len);
        int n = monocs_tcp_send(t->tcp, p, (int)len, 1000);
        if (n <= 0)
            break;
        br_ssl_engine_sendrec_ack(&t->sc.eng, (size_t)n);
    }
    monocs_tcp_close(t->tcp);
    free(t);
}
