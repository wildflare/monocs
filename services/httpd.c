// HTTP サーバー（ポート 80）: SD カードのファイルを送り受けする。サービスとして動く
// （カーネルの外のプログラム。svc start /services/httpd.c、起動時は /services.txt。os/kernel/svc.c）。
//   GET /path      ファイルを返す（ディレクトリなら一覧の HTML）
//   PUT /path      ファイルを書く（親のディレクトリも作る）。例: curl -T a.c http://192.168.7.2/work/a.c
//   DELETE /path   ファイル（空のディレクトリ）を消す
//   GET /.status          実機の様子（"app 0|1"：アプリが動いているか、"remote 0|1"：telnet・ssh でつないでいるか、
//                         "status N"：シェルが最後に動かした行の終了の値）
//   GET /.log             カーネルの記録（シェルの dmesg と同じ。起動の表示、例外、Wi-Fi・USB・サービスのできごと）
//   GET /.sdcard/fat.img  SD カードの最初の区画（FAT）をそのまま返す（診断用。Pi 5 で fsck.vfat -n をかける）
//   PUT /.sdcard/fat.img  区画をまるごと書き換えて、すぐ再起動する（修復用。fsck.vfat で直したイメージを戻す）
// 出典: RFC 9110（HTTP の意味）、RFC 9112（HTTP/1.1 の形式）。応答のたびに接続を閉じる（Connection: close）。
//
// サービスのコア（コア 3）が service_poll を繰り返し呼び、待たない形で少しずつ進める（アプリが動いていても動く）。
// 同時に扱う接続は 1 つ。TCP はアプリと同じ monocs_tcp_* を待ち時間 0 で使う。
//
// 守り: USB から届いた接続（ケーブルの先の Pi 5）は、今までどおり認証なし。内蔵 Wi-Fi から届いた接続は、
// HTTP のダイジェスト認証（RFC 7616・2617。MD5、qop=auth）でユーザー名とパスワードを確かめる（パスワードその
// ものは流れない）。パスワードは /etc/htdigest（passwd コマンドで作る。monocs_auth.h）。なければ Wi-Fi からは断る。
//   curl --digest -u ユーザー名 http://Wi-Fi のアドレス/path      ブラウザはそのまま聞いてくる
// 続けて 5 回まちがえたら、30 秒は Wi-Fi からの要求を断る。暗号化はしない（中身は家の Wi-Fi の中をそのまま流れる。
// 同じ Wi-Fi の中で通信をのぞける相手には、中身も見え、のぞいた要求を 5 分の間は送り直せる）。

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <monocs.h>
#include <monocs_auth.h>

#define SD_SECTOR_SIZE  512

#define PORT            80
#define HEADER_MAX      4096
#define IDLE_TIMEOUT_US 10000000    // 何も進まなければ閉じる

enum { ST_IDLE, ST_HEADER, ST_SEND, ST_PUT, ST_RAW_PUT };

static int listener = -1;
static int conn = -1;
static int state;
static uint64_t last_progress;
static char header[HEADER_MAX + 1];
static int header_len;
// 送るもの: 先に out（ヘッダーや一覧）、続いてファイル（file_fd）の残り
static char *out;
static int out_len, out_pos;
static int file_fd = -1;
static long file_left;
// 区画をそのまま送るとき（GET /.sdcard/fat.img）: 次に読むセクタと、残りのセクタ数
static uint32_t raw_sector, raw_left;
static char chunk[16384];
static int chunk_len, chunk_pos;
// PUT
static char put_path[256], put_tmp[264];
static long put_left;
static unsigned long requests;

int service_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    listener = monocs_tcp_listen(PORT);   // USB と Wi-Fi のどちらからも（Wi-Fi からのものは認証する。check_auth）
    if (listener < 0) {
        printf("httpd: cannot listen on port %d\n", PORT);
        return 1;
    }
    return 0;
}

void service_stop(void)
{
    // 開いているファイルと接続は、OS が閉じる（持ち主がこのサービスのもの）
    printf("httpd: stopped (%lu requests)\n", requests);
}

static void finish(void)
{
    if (file_fd >= 0)
        close(file_fd);
    file_fd = -1;
    free(out);
    out = nullptr;
    if (conn >= 0)
        monocs_tcp_close(conn);
    conn = -1;
    raw_left = 0;
    state = ST_IDLE;
}

// out に書く（足りなければ広げる）
static void put_text(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put_text(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof(tmp))
        n = sizeof(tmp) - 1;
    char *p = realloc(out, out_len + n + 1);
    if (!p)
        return;
    out = p;
    memcpy(out + out_len, tmp, n + 1);
    out_len += n;
}

// out に n バイトを足す
static void put_bytes(const char *b, int n)
{
    char *p = realloc(out, out_len + n + 1);
    if (!p)
        return;
    out = p;
    memcpy(out + out_len, b, n);
    out_len += n;
    out[out_len] = 0;
}

static void respond(int code, const char *reason, const char *type, long length)
{
    put_text("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
             code, reason, type, length);
}

static void respond_text(int code, const char *reason, const char *text)
{
    respond(code, reason, "text/plain; charset=utf-8", (long)strlen(text));
    put_text("%s", text);
    state = ST_SEND;
}

static void respond_errno(const char *path)
{
    char msg[300];
    snprintf(msg, sizeof(msg), "%s: %s\n", path, strerror(errno));
    if (errno == ENOENT)
        respond_text(404, "Not Found", msg);
    else
        respond_text(500, "Internal Server Error", msg);
}

static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    static const char *const text[] = { ".c", ".h", ".S", ".s", ".txt", ".md", ".py", ".sh", ".def",
                                        ".inc", ".map", ".ld", ".cfg" };
    if (dot) {
        for (unsigned i = 0; i < sizeof(text) / sizeof(text[0]); i++)
            if (!strcasecmp(dot, text[i]))
                return "text/plain; charset=utf-8";
        if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))
            return "text/html; charset=utf-8";
    }
    return "application/octet-stream";
}

// %xx をもとに戻す。パスは / で始める
static bool decode_path(const char *src, int n, char *dst, int size)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        char c = src[i];
        if (c == '?' || c == '#')
            break;
        if (c == '%' && i + 2 < n) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            c = (char)strtol(hex, nullptr, 16);
            i += 2;
        }
        if (k + 1 >= size)
            return false;
        dst[k++] = c;
    }
    dst[k] = '\0';
    return k > 0 && dst[0] == '/';
}

// href に入れるパス（空白や記号を %xx にする）
static const char *href(const char *dir, const char *slash, const char *name, bool is_dir)
{
    static char buf[1024];
    char raw[512];
    snprintf(raw, sizeof(raw), "%s%s%s%s", dir, slash, name, is_dir ? "/" : "");
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && k + 4 < (int)sizeof(buf); p++) {
        if (isalnum(*p) || strchr("/-_.~", *p))
            buf[k++] = (char)*p;
        else
            k += snprintf(buf + k, sizeof(buf) - k, "%%%02X", *p);
    }
    buf[k] = '\0';
    return buf;
}

static void list_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        respond_errno(path);
        return;
    }
    // 本文を先に作り、長さがわかってからヘッダーを前に付ける
    char *saved = out;
    int saved_len = out_len;
    out = nullptr;
    out_len = 0;
    const char *slash = path[strlen(path) - 1] == '/' ? "" : "/";
    put_text("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title></head><body>\n", path);
    put_text("<h1>Monocs: %s</h1><ul>\n", path);
    if (strcmp(path, "/"))
        put_text("<li><a href=\"..\">..</a></li>\n");
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[512];
        snprintf(full, sizeof(full), "%s%s%s", path, slash, e->d_name);
        struct stat st;
        bool dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (dir)
            put_text("<li><a href=\"%s\">%s/</a></li>\n", href(path, slash, e->d_name, true), e->d_name);
        else
            put_text("<li><a href=\"%s\">%s</a> (%ld)</li>\n", href(path, slash, e->d_name, false),
                     e->d_name, (long)st.st_size);
    }
    closedir(d);
    put_text("</ul></body></html>\n");
    char *body = out;
    int body_len = out_len;
    out = saved;
    out_len = saved_len;
    respond(200, "OK", "text/html; charset=utf-8", body_len);
    char *p = realloc(out, out_len + body_len + 1);
    if (p) {
        out = p;
        memcpy(out + out_len, body, body_len + 1);
        out_len += body_len;
    }
    free(body);
    state = ST_SEND;
}

// MBR の最初の区画の位置と大きさ（セクタ）
static bool first_partition(uint32_t *start, uint32_t *count)
{
    static uint8_t mbr[SD_SECTOR_SIZE];
    if (monocs_disk_read(0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return false;
    const uint8_t *e = mbr + 446;
    *start = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
    *count = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
    return *count > 0;
}

static void do_get(const char *path, bool head)
{
    if (!strcmp(path, "/.status")) {
        // 実機の様子（Pi 5 の nos.py が、コマンドを打ち込む前に見る。使っている最中の画面に打ち込まないように）
        // status: シェルが最後に動かした行の終了の値（nos.py cmd が、自分の終了の値にする）
        char buf[96];
        snprintf(buf, sizeof buf, "app %d\nremote %d\nstatus %d\n", monocs_app_running(), monocs_console_remote(),
                 monocs_shell_status());
        respond_text(200, "OK", buf);
        return;
    }
    if (!strcmp(path, "/.log")) {
        // カーネルの記録（シェルの dmesg と同じもの。os/kernel/klog.c）
        static char log[64 * 1024];
        size_t n = monocs_klog_read(log, sizeof log);
        respond(200, "OK", "text/plain; charset=utf-8", (long)n);
        if (!head)
            put_bytes(log, (int)n);
        state = ST_SEND;
        return;
    }
    if (!strcmp(path, "/.sdcard/fat.img")) {
        monocs_disk_sync(true);  // 遅らせている書き込みを済ませてから
        uint32_t start, count;
        if (!first_partition(&start, &count)) {
            respond_text(500, "Internal Server Error", "cannot read the partition table\n");
            return;
        }
        respond(200, "OK", "application/octet-stream", (long)count * SD_SECTOR_SIZE);
        raw_sector = start;
        raw_left = head ? 0 : count;
        state = ST_SEND;
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        respond_errno(path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        list_dir(path);
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        respond_errno(path);
        return;
    }
    respond(200, "OK", content_type(path), (long)st.st_size);
    if (head) {
        close(fd);
    } else {
        file_fd = fd;
        file_left = st.st_size;
    }
    state = ST_SEND;
}

static void make_parent_dirs(const char *path)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(buf, 0777);
        *p = '/';
    }
}

// ---- 認証（Wi-Fi から届いた接続だけ）----
#define NONCE_LIFE_SEC  300     // nonce の寿命（過ぎたら stale=true で作り直させる）
#define MAX_FAILS       5       // 続けてまちがえてよい回数
#define LOCK_US         30000000ULL

static char secret[33];         // nonce に印を付けるための、起動ごとの秘密
static int fails;
static uint64_t locked_until;

static long mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

// nonce = 作った時刻（16 進 8 桁）+ MD5(時刻:秘密)。覚えておかなくても、自分が作ったものか・古くないかがわかる
static void make_nonce(char *nonce, long t)
{
    if (!secret[0]) {
        // 秘密: 保存してある値（外からは見えない）と、今の時刻（ナノ秒）・要求の数を混ぜる
        char user[64], ha1[33] = "", now[64];
        monocs_auth_load(user, ha1);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf(now, sizeof now, "%lld.%09ld.%lu.%llu", (long long)ts.tv_sec, ts.tv_nsec, requests,
                 (unsigned long long)timer_now_us());
        monocs_md5_join(secret, ha1, now, nullptr);
    }
    char ts[16], mac[33];
    snprintf(ts, sizeof ts, "%08lx", t);
    monocs_md5_join(mac, ts, secret, nullptr);
    snprintf(nonce, 41, "%s%s", ts, mac);
}

// Authorization ヘッダーの値から、名前の値を取り出す（"..." でも、そのままでも）。なければ false
static bool auth_param(const char *h, const char *end, const char *name, char *out, int size)
{
    size_t nl = strlen(name);
    for (const char *p = h; p + nl < end; p++) {
        if ((p == h || p[-1] == ' ' || p[-1] == ',') && !strncasecmp(p, name, nl) && p[nl] == '=') {
            p += nl + 1;
            int k = 0;
            if (*p == '"') {
                for (p++; p < end && *p != '"' && k + 1 < size; p++)
                    out[k++] = *p;
            } else {
                for (; p < end && *p != ',' && *p != ' ' && *p != '\r' && k + 1 < size; p++)
                    out[k++] = *p;
            }
            out[k] = 0;
            return true;
        }
    }
    return false;
}

static void respond_401(bool stale)
{
    char nonce[41];
    make_nonce(nonce, mono_sec());
    static const char body[] = "authentication required\n";
    put_text("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"" MONOCS_AUTH_REALM "\", qop=\"auth\", "
             "algorithm=MD5, nonce=\"%s\"%s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
             "Connection: close\r\n\r\n%s", nonce, stale ? ", stale=true" : "", (int)sizeof(body) - 1, body);
    state = ST_SEND;
}

// 通してよければ true。だめなら応答を用意して false。auth は Authorization ヘッダーの値（なければ NULL）、
// target は要求の行のパス（%xx を戻す前のまま）
// 公開のフォルダ（2026-11-26。C の Web: Monocs 同士でソースを渡す。docs/cweb.md）: /root/share の下は、Wi-Fi からも
// パスワードなしで読める（GET・HEAD だけ）。パスは正規化しないので、.. を含むものは公開として扱わない（外へ出さない）
#define PUBLIC_DIR "/root/share"
static bool is_public(const char *method, const char *path)
{
    if (strcmp(method, "GET") && strcmp(method, "HEAD"))
        return false;
    size_t n = strlen(PUBLIC_DIR);
    return !strncasecmp(path, PUBLIC_DIR, n) && (path[n] == '/' || path[n] == '\0') && !strstr(path, "..") &&
           !strchr(path, '\\');
}

static bool check_auth(const char *method, const char *target, const char *auth, const char *auth_end)
{
    if (monocs_tcp_netif(conn) != MONOCS_NETIF_WIFI)
        return true;    // USB から
    char user[64], ha1[33];
    if (!monocs_auth_load(user, ha1)) {
        respond_text(403, "Forbidden", "no password is set for access over Wi-Fi (run passwd on Monocs)\n");
        return false;
    }
    if (timer_now_us() < locked_until) {
        respond_text(429, "Too Many Requests", "too many failures; try again later\n");
        return false;
    }
    if (!auth || strncasecmp(auth, "Digest ", 7)) {
        respond_401(false);
        return false;
    }
    char u[64], realm[32], nonce[48], uri[300], resp[40], qop[16], nc[16], cnonce[128];
    bool have = auth_param(auth, auth_end, "username", u, sizeof u) &&
                auth_param(auth, auth_end, "realm", realm, sizeof realm) &&
                auth_param(auth, auth_end, "nonce", nonce, sizeof nonce) &&
                auth_param(auth, auth_end, "uri", uri, sizeof uri) &&
                auth_param(auth, auth_end, "response", resp, sizeof resp) &&
                auth_param(auth, auth_end, "qop", qop, sizeof qop) &&
                auth_param(auth, auth_end, "nc", nc, sizeof nc) &&
                auth_param(auth, auth_end, "cnonce", cnonce, sizeof cnonce);
    // nonce は自分が作ったものか（時刻に付けた印が合うか）
    char expect_nonce[41];
    long t = -1;
    if (have && strlen(nonce) == 40) {
        char ts[9];
        memcpy(ts, nonce, 8);
        ts[8] = 0;
        t = strtol(ts, nullptr, 16);
    }
    if (t >= 0)
        make_nonce(expect_nonce, t);
    bool ok = have && t >= 0 && monocs_auth_equal(nonce, expect_nonce) && !strcmp(qop, "auth") &&
              !strcmp(realm, MONOCS_AUTH_REALM) && !strcmp(uri, target);
    if (ok) {
        // response = MD5(HA1:nonce:nc:cnonce:qop:HA2)、HA2 = MD5(メソッド:uri)（RFC 2617 の 3.2.2）
        char ha2[33], want[33];
        monocs_md5_join(ha2, method, uri, nullptr);
        monocs_md5_join(want, ha1, nonce, nc, cnonce, qop, ha2, nullptr);
        ok = monocs_auth_equal(u, user) & monocs_auth_equal(resp, want);
    }
    if (!ok) {
        if (++fails >= MAX_FAILS) {
            fails = 0;
            locked_until = timer_now_us() + LOCK_US;
        }
        respond_401(false);
        return false;
    }
    fails = 0;
    long age = mono_sec() - t;
    if (age < 0 || age > NONCE_LIFE_SEC) {
        respond_401(true);      // パスワードは合っている。nonce が古いので、新しいもので送り直してもらう
        return false;
    }
    return true;
}

// PUT の本文を書く（header にすでに届いている分も）
static bool put_write(const char *p, int n)
{
    if (n > put_left)
        n = (int)put_left;
    if (n > 0 && write(file_fd, p, n) != n)
        return false;
    put_left -= n;
    return true;
}

static void put_finish(void)
{
    int c = close(file_fd);
    file_fd = -1;
    if (c != 0) {
        respond_errno(put_tmp);
        unlink(put_tmp);
        return;
    }
    unlink(put_path);
    if (rename(put_tmp, put_path) != 0) {
        respond_errno(put_path);
        return;
    }
    respond_text(201, "Created", "ok\n");
}

static void do_put(const char *path, long length, const char *body, int body_len, bool expect)
{
    if (length < 0) {
        respond_text(411, "Length Required", "Content-Length is required\n");
        return;
    }
    snprintf(put_path, sizeof(put_path), "%s", path);
    snprintf(put_tmp, sizeof(put_tmp), "%s.tmp", path);
    make_parent_dirs(put_path);
    file_fd = open(put_tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (file_fd < 0) {
        respond_errno(put_tmp);
        return;
    }
    put_left = length;
    if (expect) {   // curl は大きな PUT の前に「100 Continue」を待つ（RFC 9110 10.1.1）
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        monocs_tcp_send(conn, cont, sizeof(cont) - 1, 1000);
    }
    if (!put_write(body, body_len)) {
        respond_errno(put_tmp);
        return;
    }
    if (put_left == 0)
        put_finish();
    else
        state = ST_PUT;
}

// 区画をまるごと書き換える（PUT /.sdcard/fat.img）。届いた分を 16KB ずつ書く
static uint32_t raw_part_count, raw_buf_len;
static bool raw_failed;

static void raw_put_begin(long length, const char *body, int body_len, bool expect);

static void handle_request(void)
{
    char *end = strstr(header, "\r\n\r\n");
    int body_off = (int)(end - header) + 4;
    // 要求の行: METHOD SP PATH SP VERSION
    char method[16];
    char *sp1 = strchr(header, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : nullptr;
    char path[256];
    if (!sp1 || !sp2 || sp1 - header >= (int)sizeof(method) ||
        !decode_path(sp1 + 1, (int)(sp2 - sp1 - 1), path, sizeof(path))) {
        respond_text(400, "Bad Request", "bad request\n");
        return;
    }
    memcpy(method, header, sp1 - header);
    method[sp1 - header] = '\0';
    // ヘッダー（大文字・小文字を区別しない）
    long length = -1;
    bool expect = false;
    const char *auth = nullptr, *auth_end = nullptr;
    for (char *line = strstr(header, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
        char *h = line + 2;
        if (!strncasecmp(h, "Content-Length:", 15))
            length = strtol(h + 15, nullptr, 10);
        else if (!strncasecmp(h, "Authorization:", 14)) {
            auth = h + 14;
            while (*auth == ' ')
                auth++;
            auth_end = strstr(h, "\r\n");
        }
        else if (!strncasecmp(h, "Expect:", 7) && strstr(h, "100-continue") &&
                 strstr(h, "100-continue") < strstr(h, "\r\n"))
            expect = true;
    }
    requests++;
    char target[300];   // 要求の行のパス（%xx を戻す前のまま。ダイジェスト認証の uri と比べる）
    snprintf(target, sizeof target, "%.*s", (int)(sp2 - sp1 - 1), sp1 + 1);
    if (!is_public(method, path) && !check_auth(method, target, auth, auth_end))
        return;
    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
        do_get(path, method[0] == 'H');
    } else if (!strcmp(method, "PUT") && !strcmp(path, "/.sdcard/fat.img")) {
        raw_put_begin(length, header + body_off, header_len - body_off, expect);
    } else if (!strcmp(method, "PUT")) {
        do_put(path, length, header + body_off, header_len - body_off, expect);
    } else if (!strcmp(method, "DELETE")) {
        if (unlink(path) != 0)
            respond_errno(path);
        else
            respond_text(200, "OK", "deleted\n");
    } else {
        respond_text(405, "Method Not Allowed", "GET, HEAD, PUT, DELETE\n");
    }
}

// 送るものを、送れるだけ送る。すべて送ったら true
static bool send_some(void)
{
    for (;;) {
        if (out && out_pos < out_len) {
            int n = monocs_tcp_send(conn, out + out_pos, out_len - out_pos, 0);
            if (n < 0)
                return true;
            out_pos += n;
            if (n > 0)
                last_progress = timer_now_us();
            if (out_pos < out_len)
                return false;
            continue;
        }
        if (chunk_pos < chunk_len) {
            int n = monocs_tcp_send(conn, chunk + chunk_pos, chunk_len - chunk_pos, 0);
            if (n < 0)
                return true;
            chunk_pos += n;
            if (n > 0)
                last_progress = timer_now_us();
            if (chunk_pos < chunk_len)
                return false;
            continue;
        }
        if (raw_left > 0) {
            uint32_t n = sizeof(chunk) / SD_SECTOR_SIZE;
            if (n > raw_left)
                n = raw_left;
            if (monocs_disk_read(raw_sector, n, chunk) != 0)
                return true;
            raw_sector += n;
            raw_left -= n;
            chunk_len = (int)(n * SD_SECTOR_SIZE);
            chunk_pos = 0;
            continue;
        }
        if (file_fd >= 0 && file_left > 0) {
            int n = read(file_fd, chunk, file_left < (long)sizeof(chunk) ? (int)file_left : (int)sizeof(chunk));
            if (n <= 0)
                return true;
            file_left -= n;
            chunk_len = n;
            chunk_pos = 0;
            continue;
        }
        return true;
    }
}

// chunk にたまった分を書く（512 バイトの倍数になった分）
static void raw_write_chunk(bool last)
{
    uint32_t n = raw_buf_len / SD_SECTOR_SIZE;
    if (last && raw_buf_len % SD_SECTOR_SIZE)
        raw_failed = true;  // 大きさが合わない（始めに確かめているので、起きないはず）
    if (n && !raw_failed) {
        if (monocs_disk_write(raw_sector, n, chunk) != 0)
            raw_failed = true;
        raw_sector += n;
    }
    memmove(chunk, chunk + n * SD_SECTOR_SIZE, raw_buf_len - n * SD_SECTOR_SIZE);
    raw_buf_len -= n * SD_SECTOR_SIZE;
}

static void raw_feed(const char *p, int n)
{
    while (n > 0) {
        int k = (int)sizeof(chunk) - (int)raw_buf_len;
        if (k > n)
            k = n;
        if (k > put_left)
            k = (int)put_left;
        memcpy(chunk + raw_buf_len, p, k);
        raw_buf_len += k;
        put_left -= k;
        p += k;
        n -= k;
        if (raw_buf_len == sizeof(chunk) || put_left == 0)
            raw_write_chunk(put_left == 0);
        if (put_left == 0)
            break;
    }
}

static void raw_put_end(void)
{
    // 書き換えた後は、メモリ上の FAT・ディレクトリ・キャッシュは古い。何も書かずに再起動する
    monocs_disk_discard();
    static const char msg[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nrebooting\n";
    const char *ok = raw_failed ? "HTTP/1.1 500 Error\r\nContent-Length: 6\r\nConnection: close\r\n\r\nerror\n" : msg;
    monocs_tcp_send(conn, ok, (int)strlen(ok), 1000);
    monocs_tcp_close(conn);
    conn = -1;
    uint64_t end = timer_now_us() + 300000;     // 応答を送り終えるのを待つ
    while (timer_now_us() < end)
        ;
    monocs_reboot(false);
}

static void raw_put_begin(long length, const char *body, int body_len, bool expect)
{
    uint32_t start;
    if (!first_partition(&start, &raw_part_count) || length != (long)raw_part_count * SD_SECTOR_SIZE) {
        respond_text(400, "Bad Request", "the size must be the same as the partition\n");
        return;
    }
    // これまでに遅らせていた書き込みは捨てる（イメージは GET /.sdcard/fat.img で sync の後に取ったもの）
    monocs_disk_discard();
    raw_sector = start;
    raw_buf_len = 0;
    raw_failed = false;
    put_left = length;
    if (expect) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        monocs_tcp_send(conn, cont, sizeof(cont) - 1, 1000);
    }
    raw_feed(body, body_len);
    if (put_left == 0)
        raw_put_end();
    else
        state = ST_RAW_PUT;
}

// 仕事をしたら 0 以外（サービスのコアは、どのサービスも仕事をしなければ眠る）
int service_poll(void)
{
    if (listener < 0)
        return 0;
    if (state == ST_IDLE) {
        conn = monocs_tcp_accept(listener, 0);
        if (conn < 0)
            return 0;
        state = ST_HEADER;
        header_len = 0;
        out_len = out_pos = 0;
        chunk_len = chunk_pos = 0;
        last_progress = timer_now_us();
    }
    if (timer_now_us() - last_progress > IDLE_TIMEOUT_US) {
        if (state == ST_RAW_PUT) {  // 区画は書きかけ。メモリ上の状態も古いので、何も書かずに再起動する
            monocs_disk_discard();
            monocs_reboot(false);
        }
        if (state == ST_PUT)
            unlink(put_tmp);
        finish();
        return 1;
    }
    if (state == ST_HEADER) {
        int n = monocs_tcp_recv(conn, header + header_len, HEADER_MAX - header_len, 0);
        if (n == 0 || n == -2) {
            finish();
            return 1;
        }
        if (n > 0) {
            header_len += n;
            header[header_len] = '\0';
            last_progress = timer_now_us();
            if (strstr(header, "\r\n\r\n"))
                handle_request();
            else if (header_len == HEADER_MAX)
                respond_text(431, "Request Header Fields Too Large", "header too large\n");
        }
    }
    if (state == ST_PUT) {
        static char buf[16384];
        for (int i = 0; i < 16 && state == ST_PUT; i++) {
            int n = monocs_tcp_recv(conn, buf, put_left < (long)sizeof(buf) ? (int)put_left : (int)sizeof(buf), 0);
            if (n == 0 || n == -2) {    // 途中で切れた
                close(file_fd);
                file_fd = -1;
                unlink(put_tmp);
                finish();
                return 1;
            }
            if (n < 0)
                break;
            last_progress = timer_now_us();
            if (!put_write(buf, n)) {
                close(file_fd);
                file_fd = -1;
                respond_errno(put_tmp);
                unlink(put_tmp);
            } else if (put_left == 0) {
                put_finish();
            }
        }
    }
    if (state == ST_RAW_PUT) {
        static char buf[16384];
        for (int i = 0; i < 16 && state == ST_RAW_PUT; i++) {
            int n = monocs_tcp_recv(conn, buf, sizeof(buf), 0);
            if (n == 0 || n == -2) {    // 途中で切れた: 区画は書きかけ。メモリ上の状態も古いので再起動する
                monocs_disk_discard();
                monocs_reboot(false);
            }
            if (n < 0)
                break;
            last_progress = timer_now_us();
            raw_feed(buf, n);
            if (put_left == 0)
                raw_put_end();
        }
    }
    if (state == ST_SEND && send_some())
        finish();
    return 1;   // 接続を扱っている間は、仕事をしたことにする（眠らずに続ける）
}
