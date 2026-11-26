// sandbox URL（2026-11-26。docs/sandbox.md の 5、docs/cweb.md）: http:// の C のソースを取ってきて、サンドボックスで動かす。
//   sandbox http://HOST[:PORT]/PATH/prog.c[#sha256=64 桁] [引数...]
// 取ってくる道は HTTP（暗号なし）。すり替えには中身の SHA-256 で気づく:
//   #sha256= があれば、合わなければ動かさない（ハッシュは作った人から別の道で受け取る）。
//   なければ、前に許したときのハッシュ（/var/cache/sandbox/…/名前.sha256）と比べ、変わっていたら聞く。初めてならハッシュを
//   出して覚える（1 回目は守れない。動くのは標準のサンドボックス）。
// 置き場所: /var/cache/sandbox/<URL のフォルダの部分のハッシュ>/ に、名前（使えない文字は %XX に）と url.txt。
// ハッシュのフォルダにするのは、FAT32 が大文字・小文字を区別せず、: ? * などを名前に使えないため。
// まだないもの: https、転送（3xx）、chunked、ほかのファイル（#include "x.h" は同じ置き場所にないと見つからない）
#define _GNU_SOURCE     // memmem（カーネルは ISO C23 なので宣言を見せる）
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../kernel/sha256.h"
#include "../net/tcp_api.h"
#include "../net/dns_api.h"
#include "../drivers/uart.h"

#define CACHE_ROOT  "/var/cache/sandbox"
#define MAX_BODY    (4u << 20)

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 0x100000001b3ULL;
    return h;
}

// 「y」が押されるか（30 秒で「いいえ」。ssh のコマンドからはキーが来ないので、そのまま断る）
static bool ask_yes(void)
{
    char c = 0;
    if (uart_read(&c, 1, 30000000) != 1)
        return false;
    printf("%c\n", c);
    return c == 'y' || c == 'Y';
}

static bool write_file(const char *path, const void *p, size_t n)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = fwrite(p, 1, n, f) == n;
    ok &= fclose(f) == 0;
    return ok && rename(tmp, path) == 0;
}

// HTTP/1.0 の GET。本文を malloc して返す（呼んだ側が free）。だめなら NULL（理由を表示する）
static char *http_get(const char *host, int port, const char *path, size_t *len)
{
    char ip[48];
    if (dns_api_resolve(host, ip, sizeof ip, 10000) != 0) {
        fprintf(stderr, "sandbox: cannot resolve %s\n", host);
        return nullptr;
    }
    int h = tcp_api_connect(ip, port, 10000);
    if (h < 0) {
        fprintf(stderr, "sandbox: cannot connect to %s:%d\n", host, port);
        return nullptr;
    }
    char req[600];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: monocs-sandbox\r\nConnection: close\r\n\r\n",
                     path, host);
    char *buf = n > 0 && n < (int)sizeof req && tcp_api_send(h, req, n, 10000) == n ? malloc(MAX_BODY + 1) : nullptr;
    size_t got = 0;
    for (;;) {
        if (!buf)
            break;
        if (got == MAX_BODY) {
            fprintf(stderr, "sandbox: too large (more than %u bytes)\n", MAX_BODY);
            free(buf);
            buf = nullptr;
            break;
        }
        int r = tcp_api_recv(h, buf + got, (int)(MAX_BODY - got), 15000);
        if (r == 0)
            break;
        if (r < 0) {
            fprintf(stderr, "sandbox: %s while reading from %s\n", r == -1 ? "timed out" : "error", host);
            free(buf);
            buf = nullptr;
            break;
        }
        got += (size_t)r;
    }
    tcp_api_close(h);
    if (!buf)
        return nullptr;
    buf[got] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    int status = 0;
    if (strncmp(buf, "HTTP/1.", 7) || !body || sscanf(buf + 8, "%d", &status) != 1 || status != 200) {
        char *eol = strstr(buf, "\r\n");
        fprintf(stderr, "sandbox: %s:%d%s: %.*s\n", host, port, path, eol ? (int)(eol - buf) : 40, buf);
        free(buf);
        return nullptr;
    }
    body += 4;
    *len = got - (size_t)(body - buf);
    memmove(buf, body, *len);
    buf[*len] = 0;
    return buf;
}

// URL を取ってきて確かめ、置いたファイルのパスを out に。だめなら false
bool sandbox_fetch(const char *url, char *out, size_t out_size)
{
    if (!strncmp(url, "https://", 8)) {
        fprintf(stderr, "sandbox: https is not supported yet (use http:// with #sha256=...)\n");
        return false;
    }
    const char *p = url + 7;    // "http://"
    char host[128], pin[65] = "";
    int port = 80;
    size_t hl = strcspn(p, ":/#");
    if (hl == 0 || hl >= sizeof host)
        return fprintf(stderr, "sandbox: bad URL: %s\n", url), false;
    memcpy(host, p, hl);
    host[hl] = 0;
    p += hl;
    if (*p == ':') {
        port = atoi(p + 1);
        p += 1 + strspn(p + 1, "0123456789");
    }
    const char *hash = strchr(p, '#');
    size_t pl = hash ? (size_t)(hash - p) : strlen(p);
    if (hash) {
        if (strncmp(hash, "#sha256=", 8) || strlen(hash + 8) != 64)
            return fprintf(stderr, "sandbox: write the hash as #sha256= and 64 hex digits\n"), false;
        for (int i = 0; i < 64; i++)
            pin[i] = (char)tolower((unsigned char)hash[8 + i]);
        pin[64] = 0;
    }
    char path[400];
    if (pl == 0 || pl >= sizeof path || p[0] != '/' || memmem(p, pl, "..", 2))
        return fprintf(stderr, "sandbox: bad path in URL (needs /…, no ..)\n"), false;
    memcpy(path, p, pl);
    path[pl] = 0;
    const char *slash = strrchr(path, '/');
    const char *name = slash + 1;
    if (!*name)
        return fprintf(stderr, "sandbox: the URL must name a .c file\n"), false;

    // 置き場所: URL の、ファイルの名前の前までのハッシュ
    char dir[200], file[300], sumf[320], urlf[220];
    size_t base_len = (size_t)(p - url) + (size_t)(name - path);   // url の、ファイルの名前の手前までの長さ
    snprintf(dir, sizeof dir, CACHE_ROOT "/%016llx", (unsigned long long)fnv(url, base_len));
    char safe[120];
    size_t k = 0;
    for (const char *q = name; *q && k + 4 < sizeof safe; q++)
        k += (size_t)(isalnum((unsigned char)*q) || strchr("._-", *q) ? snprintf(safe + k, 2, "%c", *q)
                                                                         : snprintf(safe + k, 4, "%%%02X", (unsigned char)*q));
    snprintf(file, sizeof file, "%s/%s", dir, safe);
    snprintf(sumf, sizeof sumf, "%s.sha256", file);
    snprintf(urlf, sizeof urlf, "%s/url.txt", dir);

    size_t len = 0;
    char *body = http_get(host, port, path, &len);
    if (!body)
        return false;
    char got[65];
    sha256_hex(body, len, got);
    char old[65] = "";
    FILE *f = fopen(sumf, "r");
    if (f) {
        if (!fgets(old, sizeof old, f))
            old[0] = 0;
        fclose(f);
    }
    bool ok = true;
    if (pin[0]) {
        if (strcmp(pin, got)) {
            fprintf(stderr, "sandbox: hash mismatch: expected %s\n                        got      %s\n", pin, got);
            ok = false;
        }
    } else if (old[0] && strcmp(old, got)) {
        printf("sandbox: %s changed since you allowed it\n  before %s\n  now    %s\nRun it? [y/N] ", url, old, got);
        fflush(stdout);
        ok = ask_yes();
    } else if (!old[0]) {
        printf("sandbox: first run of %s\n  sha256 %s (check it with the author; add #sha256=... next time)\n", url, got);
    }
    if (ok) {
        mkdir("/var/cache", 0755);
        mkdir(CACHE_ROOT, 0755);
        mkdir(dir, 0755);
        char u[260];
        int un = snprintf(u, sizeof u, "%.*s\n", (int)base_len, url);
        ok = write_file(file, body, len) && write_file(sumf, got, 64) && write_file(urlf, u, (size_t)un);
        if (!ok)
            fprintf(stderr, "sandbox: cannot write %s\n", file);
    }
    free(body);
    if (ok)
        snprintf(out, out_size, "%s", file);
    return ok;
}

// ---- ページの指定から、コンパイルするファイルの一覧（sandbox_page.h。2026-11-26） ----
#include <dirent.h>
#include "sandbox_page.h"

#define MANIFEST    "cweb.txt"

static bool is_http(const char *u)
{
    return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8);
}

// 目録の中のファイルの名前: 英数字と ._- だけ（フォルダの中だけ。/ と .. は使えない）
static bool safe_name(const char *n)
{
    if (!*n || n[0] == '.')
        return false;
    for (const char *p = n; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("._-", *p))
            return false;
    return true;
}

static bool ends_with_c(const char *n)
{
    size_t l = strlen(n);
    return l > 2 && !strcmp(n + l - 2, ".c");
}

// 手元のファイルの中身の SHA-256
static bool file_sha(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char *buf = malloc(MAX_BODY);
    size_t n = buf ? fread(buf, 1, MAX_BODY, f) : 0;
    bool ok = buf && !ferror(f) && !(n == MAX_BODY && fgetc(f) != EOF);
    fclose(f);
    if (ok)
        sha256_hex(buf, n, out);
    free(buf);
    return ok;
}

// 目録（sha256sum の形: 「64 桁  名前」か「64 桁 *名前」）を読み、1 つずつ fn に渡す。fn が false なら止める
static bool each_entry(const char *manifest, bool (*fn)(const char *hash, const char *name, void *ctx), void *ctx)
{
    FILE *f = fopen(manifest, "r");
    if (!f)
        return fprintf(stderr, "sandbox: cannot read %s\n", manifest), false;
    char line[400];
    bool ok = true;
    int lines = 0;
    while (ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        char *name = line + 64;
        bool hex = strlen(line) > 66;
        for (int i = 0; i < 64 && hex; i++)
            hex = isxdigit((unsigned char)line[i]);
        if (!hex || name[0] != ' ' || (name[1] != ' ' && name[1] != '*')) {
            fprintf(stderr, "sandbox: %s: bad line (write it as sha256sum does): %s\n", manifest, line);
            ok = false;
            break;
        }
        line[64] = 0;
        for (int i = 0; i < 64; i++)
            line[i] = (char)tolower((unsigned char)line[i]);
        name += 2;
        if (!safe_name(name)) {
            fprintf(stderr, "sandbox: %s: bad file name (letters, digits, . _ - only): %s\n", manifest, name);
            ok = false;
            break;
        }
        ok = fn(line, name, ctx);
        lines++;
    }
    fclose(f);
    if (ok && !lines)
        return fprintf(stderr, "sandbox: %s lists no files\n", manifest), false;
    return ok;
}

struct collect {
    const char *base;       // http なら URL のフォルダ、手元ならフォルダのパス（どちらも / で終わる）
    bool http;
    char (*files)[SANDBOX_PATH_MAX];
    int max, n;
};

static bool add_c(struct collect *c, const char *path)
{
    if (!ends_with_c(path))
        return true;
    if (c->n == c->max)
        return fprintf(stderr, "sandbox: too many .c files (up to %d)\n", c->max), false;
    snprintf(c->files[c->n++], SANDBOX_PATH_MAX, "%s", path);
    return true;
}

static bool collect_entry(const char *hash, const char *name, void *ctx)
{
    struct collect *c = ctx;
    char path[SANDBOX_PATH_MAX];
    if (c->http) {          // 目録のハッシュを付けて取ってくる（合わなければ置かない）
        char url[600];
        snprintf(url, sizeof url, "%s%s#sha256=%s", c->base, name, hash);
        if (!sandbox_fetch(url, path, sizeof path))
            return false;
    } else {                // 手元: 読んで確かめる
        snprintf(path, sizeof path, "%s%s", c->base, name);
        char got[65];
        if (!file_sha(path, got))
            return fprintf(stderr, "sandbox: cannot read %s\n", path), false;
        if (strcmp(got, hash))
            return fprintf(stderr, "sandbox: hash mismatch: %s\n  listed %s\n  got    %s\n", path, hash, got), false;
    }
    return add_c(c, path);
}

int sandbox_prepare(const char *spec, char (*files)[SANDBOX_PATH_MAX], int max)
{
    const char *hash = strchr(spec, '#');
    char pin[65] = "";
    if (hash) {
        if (strncmp(hash, "#sha256=", 8) || strlen(hash + 8) != 64)
            return fprintf(stderr, "sandbox: write the hash as #sha256= and 64 hex digits\n"), 0;
        for (int i = 0; i < 64; i++)
            pin[i] = (char)tolower((unsigned char)hash[8 + i]);
    }
    char base[SANDBOX_PATH_MAX];
    snprintf(base, sizeof base, "%.*s", hash ? (int)(hash - spec) : (int)strlen(spec), spec);
    size_t bl = strlen(base);
    struct stat st;
    bool local_dir = !is_http(base) && stat(base, &st) == 0 && S_ISDIR(st.st_mode);
    bool dir = (bl && base[bl - 1] == '/') || local_dir;
    if (dir && bl && base[bl - 1] != '/' && bl + 1 < sizeof base)
        strcat(base, "/");
    struct collect c = { base, is_http(base), files, max, 0 };

    if (!dir) {                                     // 1 つのファイル
        if (c.http)
            return sandbox_fetch(spec, files[0], SANDBOX_PATH_MAX) ? 1 : 0;
        if (pin[0]) {
            char got[65];
            if (!file_sha(base, got))
                return fprintf(stderr, "sandbox: cannot read %s\n", base), 0;
            if (strcmp(got, pin))
                return fprintf(stderr, "sandbox: hash mismatch: %s\n  expected %s\n  got      %s\n", base, pin, got), 0;
        }
        snprintf(files[0], SANDBOX_PATH_MAX, "%s", base);
        return 1;
    }
    char manifest[SANDBOX_PATH_MAX];
    if (c.http) {                                   // 目録を取ってくる（#sha256= は目録のハッシュ）
        char url[600];
        snprintf(url, sizeof url, "%s" MANIFEST "%s%s", base, pin[0] ? "#sha256=" : "", pin);
        if (!sandbox_fetch(url, manifest, sizeof manifest))
            return 0;
    } else {
        snprintf(manifest, sizeof manifest, "%s" MANIFEST, base);
        if (stat(manifest, &st) != 0) {             // 目録のない手元のフォルダ: 中の .c を全部
            if (pin[0])
                return fprintf(stderr, "sandbox: %s: no %s to check the hash against\n", base, MANIFEST), 0;
            DIR *d = opendir(base);
            struct dirent *e;
            while (d && (e = readdir(d))) {
                char path[SANDBOX_PATH_MAX];
                snprintf(path, sizeof path, "%s%s", base, e->d_name);
                if (e->d_name[0] != '.' && !add_c(&c, path)) {
                    closedir(d);
                    return 0;
                }
            }
            if (d)
                closedir(d);
            if (!c.n)
                fprintf(stderr, "sandbox: %s has no .c files\n", base);
            return c.n;
        }
        if (pin[0]) {
            char got[65];
            if (!file_sha(manifest, got) || strcmp(got, pin))
                return fprintf(stderr, "sandbox: hash mismatch: %s\n  expected %s\n  got      %s\n", manifest, pin,
                               file_sha(manifest, got) ? got : "(cannot read)"), 0;
        }
    }
    if (!each_entry(manifest, collect_entry, &c))
        return 0;
    if (!c.n)
        fprintf(stderr, "sandbox: %s lists no .c files\n", manifest);
    return c.n;
}
