// curl: URL から取ってくる（http:// と https://）。Linux の curl に似せた小さなもの。Monocs に付いてくるアプリ（/bin/curl）
//
//   curl [オプション] URL
//     -o FILE   ファイルに書く（なければ画面・パイプへ）      -O   URL の最後の名前のファイルに書く
//     -L        転送（301・302・303・307・308）を追う（10 回まで）
//     -I        ヘッダーだけ（HEAD）                          -i   ヘッダーも出す
//     -s        進み具合とエラーを出さない（-S を付けるとエラーは出す）
//     -f        400 以上の応答は失敗にして、本文を出さない（終了の値 22）
//     -m 秒     全体の時間の上限（既定はなし。つなぐのは 30 秒まで）
//     -X METHOD、-H 'Name: value'、-d DATA（POST）、-A NAME（User-Agent）
//     -v        やりとりのヘッダーを stderr に出す
//     --compressed  圧縮して送ってもらい（Accept-Encoding: deflate, gzip）、展開して出す。HTML やソースが速く届く
//     --raw     圧縮されて届いても展開しない（頼まなくても圧縮して返すサーバーのものは、ふつうは展開する）
//
// HTTP/1.1 で、1 回ごとに接続を閉じる（Connection: close）。本文は Content-Length・chunked・閉じるまで、のどれでも。
// https は共有のライブラリ /lib/tls（BearSSL。証明書を /etc/ssl/certs/ca-certificates.crt で確かめる）。
// 内蔵 Wi-Fi がつながっていること（名前を引くのも Wi-Fi の先）。HTTP/2 はない。展開は共有のライブラリ /lib/inflate。
// 終了の値は curl と同じ番号（3 URL の誤り、6 名前が引けない、7 つながらない、22 -f で 400 以上、23 書けない、
// 28 時間切れ、35 TLS のやりとり、47 転送が多すぎる、52 応答がない、56 受信の失敗、60 証明書、61 展開の失敗）。
// 出典: RFC 9110（HTTP の意味）、RFC 9112（HTTP/1.1 の形式。chunked は 7.1）。
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <monocs.h>
#include <monocs_tls.h>
#include <monocs_inflate.h>

#define MAX_REDIRECTS   10
#define CONNECT_MS      30000
#define IDLE_MS         60000   // 何も届かなければあきらめる
#define MAX_HEADERS     16

static bool opt_silent, opt_show_error, opt_fail, opt_follow, opt_head, opt_include, opt_verbose, opt_remote_name;
static bool opt_compressed, opt_raw;
static const char *opt_output, *opt_method, *opt_data, *opt_agent = "curl/8 (Monocs)";
static const char *opt_headers[MAX_HEADERS];
static int nheaders;
static long opt_max_ms = -1;
static uint64_t start_us;

static void fail_msg(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail_msg(int code, const char *fmt, ...)
{
    if (opt_silent && !opt_show_error)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "curl: (%d) ", code);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// 残りの時間（ミリ秒）。idle までに切り詰める。0 なら時間切れ
static int time_left(int idle)
{
    if (opt_max_ms < 0)
        return idle;
    long used = (long)((timer_now_us() - start_us) / 1000);
    long left = opt_max_ms - used;
    return left <= 0 ? 0 : left < idle ? (int)left : idle;
}

// ---- 接続（そのままの TCP か TLS）----
typedef struct {
    int tcp;
    MonocsTls *tls;
} Conn;

static int conn_send(Conn *c, const void *p, int n)
{
    if (c->tls)
        return monocs_tls_send(c->tls, p, n);
    int sent = 0;
    while (sent < n) {
        int k = monocs_tcp_send(c->tcp, (const char *)p + sent, n - sent, 30000);
        if (k <= 0)
            return -1;
        sent += k;
    }
    return n;
}

static int conn_recv(Conn *c, void *p, int n, int ms)
{
    return c->tls ? monocs_tls_recv(c->tls, p, n, ms) : monocs_tcp_recv(c->tcp, p, n, ms);
}

static void conn_close(Conn *c)
{
    if (c->tls)
        monocs_tls_close(c->tls);
    else if (c->tcp >= 0)
        monocs_tcp_close(c->tcp);
    c->tls = nullptr;
    c->tcp = -1;
}

// ---- URL ----
typedef struct {
    bool https;
    char host[256];
    int port;
    char path[2048];
} Url;

static bool parse_url(const char *s, Url *u)
{
    memset(u, 0, sizeof *u);
    if (!strncasecmp(s, "https://", 8)) {
        u->https = true;
        s += 8;
    } else if (!strncasecmp(s, "http://", 7)) {
        s += 7;
    } else if (strstr(s, "://")) {
        return false;
    }   // 書いていなければ http（curl と同じ）
    const char *end = s + strcspn(s, "/?#");
    const char *at = memchr(s, '@', (size_t)(end - s));
    if (at)
        s = at + 1;     // user:pass@ は使わない
    const char *colon = memchr(s, ':', (size_t)(end - s));
    size_t hl = (size_t)((colon ? colon : end) - s);
    if (hl == 0 || hl >= sizeof u->host)
        return false;
    memcpy(u->host, s, hl);
    u->port = colon ? atoi(colon + 1) : u->https ? 443 : 80;
    if (u->port <= 0 || u->port > 65535)
        return false;
    const char *frag = strchr(end, '#');
    size_t pl = frag ? (size_t)(frag - end) : strlen(end);
    if (pl + 2 >= sizeof u->path)
        return false;
    if (*end != '/')
        strcpy(u->path, "/");
    strncat(u->path, end, pl);
    return true;
}

// Location（絶対の URL、// で始まるもの、/ で始まるパス、相対のパス）から次の URL
static bool redirect_url(const Url *from, const char *loc, Url *to)
{
    if (strstr(loc, "://"))
        return parse_url(loc, to);
    char buf[2400];
    const char *scheme = from->https ? "https" : "http";
    if (!strncmp(loc, "//", 2)) {
        snprintf(buf, sizeof buf, "%s:%s", scheme, loc);
    } else if (loc[0] == '/') {
        snprintf(buf, sizeof buf, "%s://%s:%d%s", scheme, from->host, from->port, loc);
    } else {
        char dir[2048];
        snprintf(dir, sizeof dir, "%s", from->path);
        dir[strcspn(dir, "?")] = 0;
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[1] = 0;
        snprintf(buf, sizeof buf, "%s://%s:%d%s%s", scheme, from->host, from->port, dir, loc);
    }
    return parse_url(buf, to);
}

// 大文字・小文字を区別しないで探す
static bool contains_nocase(const char *s, const char *word)
{
    size_t n = strlen(word);
    for (; *s; s++)
        if (!strncasecmp(s, word, n))
            return true;
    return false;
}

// ---- 応答を読む ----
static char rbuf[16384];
static int rlen, rpos;

// 次の塊を読む。0 = 相手が閉じた、負 = エラー（-1 時間切れ）
static int fill(Conn *c)
{
    int ms = time_left(IDLE_MS);
    if (ms == 0)
        return -1;
    rpos = 0;
    rlen = conn_recv(c, rbuf, sizeof rbuf, ms);
    if (rlen < 0) {
        int r = rlen;
        rlen = 0;
        return r;
    }
    return rlen;
}

// 1 行（CRLF まで。CRLF は除く）。0 で成功、負は fill と同じ・-3 は長すぎ・-4 は途中で閉じた
static int read_line(Conn *c, char *line, int size)
{
    int k = 0;
    for (;;) {
        if (rpos == rlen) {
            int r = fill(c);
            if (r <= 0)
                return r == 0 ? -4 : r;
        }
        char ch = rbuf[rpos++];
        if (ch == '\n') {
            if (k > 0 && line[k - 1] == '\r')
                k--;
            line[k] = 0;
            return 0;
        }
        if (k + 1 >= size)
            return -3;
        line[k++] = ch;
    }
}

static FILE *out;
static long long received;
static uint64_t last_meter;

static void meter(long long total, bool final)
{
    if (opt_silent || isatty(fileno(out)) || !isatty(2))
        return;     // 画面に本文を出しているときは出さない（curl と同じ）
    uint64_t now = timer_now_us();
    if (!final && now - last_meter < 500000)
        return;
    last_meter = now;
    double sec = (double)(now - start_us) / 1e6;
    if (total > 0)
        fprintf(stderr, "\r%3d%%  %lld / %lld bytes  %.0f KB/s  ", (int)(received * 100 / total), received, total,
                sec > 0 ? (double)received / 1024 / sec : 0);
    else
        fprintf(stderr, "\r%lld bytes  %.0f KB/s  ", received, sec > 0 ? (double)received / 1024 / sec : 0);
    if (final)
        fputc('\n', stderr);
}

static bool write_body(const char *p, int n, bool discard, long long total)
{
    received += n;
    if (discard)
        return true;
    if (fwrite(p, 1, (size_t)n, out) != (size_t)n)
        return false;
    meter(total, false);
    return true;
}

// ---- 本文を読む（Content-Length・chunked・閉じるまで、のどれでも同じ形で）----
typedef struct {
    Conn *c;
    bool chunked, first_chunk;
    long long left;     // 残り（chunked なら今の塊の残り）。負なら閉じるまで
    bool done;
    int err;            // 失敗したときの終了の値（18 途中で閉じた、28 時間切れ、56 受信の失敗）
} Body;

// buf に size バイトまで読む。読んだバイト数、0 = 本文の終わり、負 = 失敗（b->err）。monocs_inflate の読む関数にもなる
static int body_read(void *ctx, void *buf, int size)
{
    Body *b = ctx;
    static char line[256];
    if (b->done || b->err)
        return b->err ? -1 : 0;
    if (b->chunked && b->left == 0) {   // 次の塊: 前の塊の終わりの CRLF、大きさ（16 進）の行。大きさ 0 で終わり
        int r = b->first_chunk ? 0 : read_line(b->c, line, sizeof line);
        if (r == 0)
            r = read_line(b->c, line, sizeof line);
        if (r != 0) {
            b->err = r == -1 ? 28 : 18;
            return -1;
        }
        b->first_chunk = false;
        b->left = strtoll(line, nullptr, 16);
        if (b->left <= 0) {
            b->done = true;
            return 0;
        }
    }
    if (b->left == 0) {
        b->done = true;
        return 0;
    }
    if (rpos == rlen) {
        int r = fill(b->c);
        if (r <= 0) {
            // 閉じるまで、のときは閉じたら終わり（TLS で close_notify なしに切られたときも）
            if (b->left < 0 && (r == 0 || (r != -1 && b->c->tls))) {
                b->done = true;
                return 0;
            }
            b->err = r == 0 ? 18 : r == -1 ? 28 : 56;
            return -1;
        }
    }
    int k = rlen - rpos;
    if (k > size)
        k = size;
    if (b->left > 0 && k > b->left)
        k = (int)b->left;
    memcpy(buf, rbuf + rpos, (size_t)k);
    rpos += k;
    if (b->left > 0)
        b->left -= k;
    return k;
}

typedef struct {
    bool discard, failed;
    long long total;
} Sink;

// monocs_inflate の書く関数
static int sink_write(void *ctx, const void *buf, int len)
{
    Sink *k = ctx;
    if (!write_body(buf, len, k->discard, k->total)) {
        k->failed = true;
        return -1;
    }
    return 0;
}

// ---- 1 回の要求: つなぐ → 要求を送る → 応答のヘッダーを読む → 本文を読む ----
// どれも、失敗したらメッセージを出して curl の終了の値を返す（0 で成功）

static int open_conn(const Url *u, Conn *c)
{
    int ms = time_left(CONNECT_MS);
    if (ms == 0)
        return fail_msg(28, "Operation timed out"), 28;
    char ip[16];
    if (monocs_resolve(u->host, ip, sizeof ip, ms < 10000 ? ms : 10000) != 0)
        return fail_msg(6, "Could not resolve host: %s", u->host), 6;
    if (opt_verbose)
        fprintf(stderr, "* Connecting to %s (%s) port %d\n", u->host, ip, u->port);
    if (!u->https) {
        c->tcp = monocs_tcp_connect(ip, u->port, time_left(CONNECT_MS));
        if (c->tcp < 0)
            return fail_msg(7, "Failed to connect to %s port %d", u->host, u->port), 7;
        return 0;
    }
    c->tls = monocs_tls_connect(u->host, u->port, time_left(CONNECT_MS));
    if (!c->tls) {
        const char *e = monocs_tls_error();
        int code = strstr(e, "cannot connect") ? 7 : strstr(e, "timed out") ? 28
                   : strstr(e, "certificate") || strstr(e, "clock") ? 60 : 35;
        return fail_msg(code, "%s", e), code;
    }
    return 0;
}

static int send_request(Conn *c, const Url *u, const char *method, const char *data)
{
    static char req[8192];
    size_t cap = sizeof req;
    int n = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s", method, u->path, u->host);
    if (u->port != (u->https ? 443 : 80))
        n += snprintf(req + n, cap - (size_t)n, ":%d", u->port);
    n += snprintf(req + n, cap - (size_t)n, "\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n%s", opt_agent,
                  opt_compressed ? "Accept-Encoding: deflate, gzip\r\n" : "");
    bool has_type = false;
    for (int i = 0; i < nheaders && n < (int)cap - 2; i++) {
        n += snprintf(req + n, cap - (size_t)n, "%s\r\n", opt_headers[i]);
        if (!strncasecmp(opt_headers[i], "Content-Type:", 13))
            has_type = true;
    }
    if (data && n < (int)cap - 2)
        n += snprintf(req + n, cap - (size_t)n, "%sContent-Length: %zu\r\n",
                      has_type ? "" : "Content-Type: application/x-www-form-urlencoded\r\n", strlen(data));
    if (n >= (int)cap - 2)
        return fail_msg(3, "The request is too long"), 3;
    n += snprintf(req + n, cap - (size_t)n, "\r\n");
    if (opt_verbose)
        fprintf(stderr, "> %.*s", n, req);
    if (conn_send(c, req, n) != n || (data && conn_send(c, data, (int)strlen(data)) != (int)strlen(data)))
        return fail_msg(55, "Failed sending the request"), 55;
    return 0;
}

typedef struct {
    int status;
    long long length;       // Content-Length（なければ -1）
    bool chunked, encoded;  // Transfer-Encoding: chunked、Content-Encoding: gzip・deflate
    char location[2048];
} Response;

// 状態の行とヘッダーを読む（100 Continue などの途中の応答は読み飛ばす）
static int read_headers(Conn *c, Response *resp)
{
    static char line[8192];
    rlen = rpos = 0;
    for (;;) {
        *resp = (Response){.length = -1};
        int r = read_line(c, line, sizeof line);
        if (r != 0) {
            int code = r == -1 ? 28 : 52;
            return fail_msg(code, code == 28 ? "Operation timed out" : "Empty reply from server"), code;
        }
        if (strncmp(line, "HTTP/", 5) || !strchr(line, ' '))
            return fail_msg(1, "Not an HTTP response"), 1;
        resp->status = atoi(strchr(line, ' ') + 1);
        bool interim = resp->status >= 100 && resp->status < 200;
        bool show = !interim && (opt_include || opt_head);
        do {    // 状態の行と、続くヘッダーの行（空の行まで）
            if (opt_verbose)
                fprintf(stderr, "< %s\n", line);
            if (show)
                fprintf(out, "%s\r\n", line);
            char *v = strchr(line, ':');
            if (v) {
                *v++ = 0;
                while (*v == ' ' || *v == '\t')
                    v++;
                if (!strcasecmp(line, "Content-Length"))
                    resp->length = atoll(v);
                else if (!strcasecmp(line, "Transfer-Encoding") && contains_nocase(v, "chunked"))
                    resp->chunked = true;
                else if (!strcasecmp(line, "Content-Encoding") &&
                         (contains_nocase(v, "gzip") || contains_nocase(v, "deflate")))
                    resp->encoded = true;   // 頼んでいなくても、圧縮して返すサーバーがある
                else if (!strcasecmp(line, "Location"))
                    snprintf(resp->location, sizeof resp->location, "%s", v);
            }
            r = read_line(c, line, sizeof line);
        } while (r == 0 && line[0]);
        if (r != 0)
            return fail_msg(56, "Failure when receiving the headers"), 56;
        if (show)
            fprintf(out, "\r\n");
        if (!interim)
            return 0;
    }
}

// 本文を読んで書く（discard なら捨てる）。圧縮されていれば展開しながら（共有のライブラリ /lib/inflate）
static int read_body(Conn *c, const Response *resp, bool discard)
{
    Body body = {.c = c, .chunked = resp->chunked, .first_chunk = true, .left = resp->chunked ? 0 : resp->length};
    bool write_failed = false;
    const char *inflate_error = nullptr;
    if (resp->encoded && !opt_raw) {
        Sink sink = {.discard = discard, .total = -1};
        if (monocs_inflate(MONOCS_INFLATE_AUTO, body_read, &body, sink_write, &sink, &inflate_error) == 0)
            inflate_error = nullptr;
        else if (!inflate_error)
            inflate_error = "error";
        write_failed = sink.failed;
    } else {
        static char buf[16384];
        int n;
        while (!write_failed && (n = body_read(&body, buf, sizeof buf)) > 0)
            write_failed = !write_body(buf, n, discard, resp->length);
    }
    // 原因の順: 書けなかった、受け取れなかった、展開できなかった
    if (write_failed)
        return fail_msg(23, "Failed writing the output"), 23;
    if (body.err == 28)
        return fail_msg(28, "Operation timed out after %lld bytes", received), 28;
    if (body.err == 18)
        return fail_msg(18, "Transfer closed with outstanding data remaining"), 18;
    if (body.err)
        return fail_msg(body.err, "Failure when receiving data"), body.err;
    if (inflate_error)
        return fail_msg(61, "Error while processing content unencoding: %s", inflate_error), 61;
    return 0;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// 応答（番号と Location）を resp に。本文は、転送を追うとき・-f で 400 以上のときは捨てる
static int request(const Url *u, const char *method, const char *data, Response *resp)
{
    Conn c = {.tcp = -1};
    int rc = open_conn(u, &c);
    if (!rc)
        rc = send_request(&c, u, method, data);
    if (!rc)
        rc = read_headers(&c, resp);
    bool has_body = strcmp(method, "HEAD") && resp->status != 204 && resp->status != 304;
    if (!rc && has_body) {
        bool follow = opt_follow && resp->location[0] && is_redirect(resp->status);
        rc = read_body(&c, resp, follow || (opt_fail && resp->status >= 400));
    }
    conn_close(&c);
    return rc;
}

static void usage(void)
{
    fprintf(stderr, "Usage: curl [-o FILE] [-O] [-L] [-I] [-i] [-s] [-S] [-f] [-v] [-m SEC] [-X METHOD] [-H HEADER] [-d DATA] [-A AGENT] [--compressed] URL\n");
}

// オプションを読む。0 = 成功（URL を *url に）、-1 = --help を出した、ほかは終了の値
static int parse_options(int argc, char **argv, const char **url)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            *url = a;
        } else if (!strcmp(a, "--help")) {
            usage();
            return -1;
        } else if (!strcmp(a, "--compressed")) {
            opt_compressed = true;
        } else if (!strcmp(a, "--raw")) {
            opt_raw = true;
        } else {
            // 1 文字のオプション（-sL のように続けて書ける）
            for (const char *p = a + 1; *p; p++) {
                if (strchr("oXdAHm", *p)) {
                    // 値を取るもの: 値は続き（-ofile）か、次の引数（-o file）。この引数はここで終わり
                    const char *v = p[1] ? p + 1 : i + 1 < argc ? argv[++i] : nullptr;
                    if (!v)
                        return usage(), 2;
                    switch (*p) {
                    case 'o': opt_output = v; break;
                    case 'X': opt_method = v; break;
                    case 'd': opt_data = v; break;
                    case 'A': opt_agent = v; break;
                    case 'm': opt_max_ms = (long)(atof(v) * 1000); break;
                    case 'H':
                        if (nheaders < MAX_HEADERS)
                            opt_headers[nheaders++] = v;
                        break;
                    }
                    break;
                }
                switch (*p) {
                case 's': opt_silent = true; break;
                case 'S': opt_show_error = true; break;
                case 'f': opt_fail = true; break;
                case 'L': opt_follow = true; break;
                case 'I': opt_head = true; break;
                case 'i': opt_include = true; break;
                case 'v': opt_verbose = true; break;
                case 'O': opt_remote_name = true; break;
                default:
                    fprintf(stderr, "curl: option -%c: is unknown\n", *p);
                    return usage(), 2;
                }
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *url = nullptr;
    int r = parse_options(argc, argv, &url);
    if (r)
        return r < 0 ? 0 : r;
    if (!url)
        return usage(), 2;
    start_us = timer_now_us();
    Url u;
    if (!parse_url(url, &u))
        return fail_msg(3, "URL rejected: %s (http:// and https:// only)", url), 3;
    static char name[256];
    if (opt_remote_name) {
        char p[2048];
        snprintf(p, sizeof p, "%s", u.path);
        p[strcspn(p, "?")] = 0;
        const char *base = strrchr(p, '/');
        if (!base || !base[1])
            return fail_msg(23, "Remote file name has no length"), 23;
        snprintf(name, sizeof name, "%s", base + 1);
        opt_output = name;
    }
    out = stdout;
    if (opt_output && strcmp(opt_output, "-")) {
        out = fopen(opt_output, "wb");
        if (!out)
            return fail_msg(23, "Failed to open %s", opt_output), 23;
    }
    const char *method = opt_method ? opt_method : opt_head ? "HEAD" : opt_data ? "POST" : "GET";
    const char *data = opt_data;
    int rc = 0;
    static Response resp;
    for (int hop = 0;; hop++) {
        rc = request(&u, method, data, &resp);
        if (rc || !opt_follow || !resp.location[0] || !is_redirect(resp.status))
            break;
        if (hop == MAX_REDIRECTS) {
            rc = 47;
            fail_msg(47, "Maximum (%d) redirects followed", MAX_REDIRECTS);
            break;
        }
        Url next;
        if (!redirect_url(&u, resp.location, &next)) {
            rc = 3;
            fail_msg(3, "Bad redirect: %s", resp.location);
            break;
        }
        u = next;
        if (resp.status == 303 || ((resp.status == 301 || resp.status == 302) && !strcmp(method, "POST"))) {
            method = "GET";     // POST の転送は GET にする（curl・ブラウザと同じ）
            data = nullptr;
        }
        received = 0;
    }
    if (!rc)
        meter(-1, true);
    if (out != stdout && fclose(out) != 0 && !rc) {
        rc = 23;
        fail_msg(23, "Failed writing the output");
    }
    if (!rc && opt_fail && resp.status >= 400) {
        rc = 22;
        fail_msg(22, "The requested URL returned error: %d", resp.status);
    }
    return rc;
}
