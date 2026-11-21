// tar: .tar・.tar.gz（.tgz）を展開する・一覧する。Monocs に付いてくるアプリ（/bin）。Linux（GNU）の tar に似せた小さなもの
//
//   tar xf FILE [-C DIR] [--strip-components=N] [NAME...]    展開する（tar -xzf、tar xvf も）
//   tar tf FILE                                               一覧（tvf で大きさ・日時も）
//   FILE が - か、f がなければ標準入力（curl -sL URL | tar xz）
//   gzip かどうかは中身で決める（z は付けても付けなくてもよい）
//   NAME を書くと、その名前（かその下）のものだけ
//
// できないこと: 作る（c）・足す（r）、bzip2・xz、リンク（FAT にないので飛ばす）、所有者・権限。
// 「..」を含む名前と、先頭の「/」は安全のために受け付けない（先頭の / は取る）。
// 形式: POSIX の ustar（名前は prefix + name）、GNU の長い名前（種類 L）、pax の拡張ヘッダー（種類 x の path）。
// 出典: POSIX.1-2017 の pax（ustar Interchange Format、pax Extended Header）、GNU tar の説明（Basic Tar Format）。
// 中身（gzip の展開）は共有のライブラリ /lib/inflate。
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <monocs_inflate.h>
#if __has_include(<monocs.h>)
#include <monocs.h>     // Monocs では utime の宣言はここ（picolibc の utime.h にはない）。Linux で試すときはない
#endif

#define BLOCK 512
#define NAME_MAX_LEN 1024

static bool opt_extract, opt_list, opt_verbose;
static int opt_strip;
static char **members;
static int nmembers;
static int exit_status;

// ---- 展開した中身を受け取って、ヘッダー・データ・詰め物の順に進む ----
static unsigned char header[BLOCK];
static int header_len;
static long long data_left, pad_left;   // 今のファイルのデータの残りと、512 の倍数までの詰め物
static enum { D_SKIP, D_FILE, D_LONGNAME, D_PAX } data_kind;
static FILE *out;
static char out_path[NAME_MAX_LEN];
static time_t out_mtime;
static char long_name[NAME_MAX_LEN];    // 次のファイルの名前（GNU の L、pax の path）。空ならヘッダーの名前
static char meta[NAME_MAX_LEN * 2];     // L・x のデータ
static int meta_len;
static int zero_blocks;
static bool finished;
static long long nfiles;

static void warn(const char *name, const char *msg)
{
    fprintf(stderr, "tar: %s: %s\n", name, msg);
    exit_status = 2;
}

// 8 進（終わりは空白か NUL）。先頭のビットが立っていれば 2 進（GNU の大きな数）
static long long number(const unsigned char *p, int n)
{
    long long v = 0;
    if (p[0] & 0x80) {
        for (int i = 1; i < n; i++)
            v = v << 8 | p[i];
        return v;
    }
    for (int i = 0; i < n && p[i]; i++)
        if (p[i] >= '0' && p[i] <= '7')
            v = v * 8 + (p[i] - '0');
    return v;
}

static bool checksum_ok(void)
{
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++)
        sum += i >= 148 && i < 156 ? ' ' : header[i];
    return sum == (unsigned)number(header + 148, 8);
}

// 親のフォルダを作る（前に作ったものは覚えておく）
static void make_parents(const char *path)
{
    static char last[NAME_MAX_LEN];
    char dir[NAME_MAX_LEN];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = 0;
    if (!strcmp(dir, last))
        return;
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    mkdir(dir, 0755);
    snprintf(last, sizeof last, "%s", dir);
}

static bool wanted(const char *name);

// 名前を整える: 先頭の / と ./ を取り、--strip-components の分を取る。使えない・選ばれていなければ false
static bool clean_name(const char *raw, char *name, bool *is_dir)
{
    const char *p = raw;
    for (;;) {
        if (p[0] == '/')
            p++;
        else if (p[0] == '.' && p[1] == '/')
            p += 2;
        else
            break;
    }
    // NAME の指定は、取る前の名前と比べる（GNU tar と同じ）
    if (!wanted(p))
        return false;
    for (int i = 0; i < opt_strip; i++) {
        const char *s = strchr(p, '/');
        if (!s)
            return false;       // 取った結果、何も残らない
        p = s + 1;
        while (*p == '/')
            p++;
    }
    size_t n = strlen(p);
    *is_dir = n > 0 && p[n - 1] == '/';
    while (n > 0 && p[n - 1] == '/')
        n--;
    if (n == 0 || n >= NAME_MAX_LEN)
        return false;
    memcpy(name, p, n);
    name[n] = 0;
    // 「..」の部分があるものは受け付けない（展開する場所の外に書かせない）
    for (const char *c = name; c;) {
        const char *next = strchr(c, '/');
        size_t len = next ? (size_t)(next - c) : strlen(c);
        if (len == 2 && c[0] == '.' && c[1] == '.') {
            warn(raw, "contains '..', skipped");
            return false;
        }
        c = next ? next + 1 : nullptr;
    }
    return true;
}

static bool wanted(const char *name)
{
    if (nmembers == 0)
        return true;
    for (int i = 0; i < nmembers; i++) {
        size_t n = strlen(members[i]);
        while (n > 1 && members[i][n - 1] == '/')
            n--;
        if (!strncmp(name, members[i], n) && (name[n] == 0 || name[n] == '/'))
            return true;
    }
    return false;
}

static void list_line(const char *name, char type, long long size, time_t mtime, unsigned mode)
{
    if (!opt_verbose) {
        printf("%s%s\n", name, type == '5' ? "/" : "");
        return;
    }
    char perm[11] = "----------", when[32];
    perm[0] = type == '5' ? 'd' : type == '2' ? 'l' : type == '1' ? 'h' : '-';
    for (int i = 0; i < 9; i++)
        if (mode & (0400u >> i))
            perm[1 + i] = "rwx"[i % 3];
    struct tm tm;
    localtime_r(&mtime, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    printf("%s %9lld %s %s%s\n", perm, size, when, name, type == '5' ? "/" : "");
}

static void begin_entry(void)
{
    char type = (char)header[156];
    long long size = number(header + 124, 12);
    data_left = size;
    pad_left = (BLOCK - size % BLOCK) % BLOCK;
    data_kind = D_SKIP;
    if (type == 'L' || type == 'x') {
        data_kind = type == 'L' ? D_LONGNAME : D_PAX;
        meta_len = 0;
        return;
    }
    if (type == 'g' || type == 'K')
        return;     // pax の全体のヘッダー、GNU の長いリンク先: 使わない
    // 名前: 前の L・x があればそれ、なければ prefix（ustar のとき）+ name
    char raw[NAME_MAX_LEN];
    if (long_name[0]) {
        snprintf(raw, sizeof raw, "%s", long_name);
        long_name[0] = 0;
    } else if (!memcmp(header + 257, "ustar", 5) && header[345]) {
        snprintf(raw, sizeof raw, "%.155s/%.100s", (const char *)header + 345, (const char *)header);
    } else {
        snprintf(raw, sizeof raw, "%.100s", (const char *)header);
    }
    bool is_dir;
    char name[NAME_MAX_LEN];
    if (!clean_name(raw, name, &is_dir))
        return;
    if (type == '5' || is_dir)
        type = '5';
    else if (type == 0 || type == '7')
        type = '0';
    time_t mtime = (time_t)number(header + 136, 12);
    unsigned mode = (unsigned)number(header + 100, 8);
    nfiles++;
    if (opt_list) {
        list_line(name, type, type == '0' ? size : 0, mtime, mode);
        return;
    }
    if (opt_verbose)
        printf("%s%s\n", name, type == '5' ? "/" : "");
    if (type == '5') {
        make_parents(name);
        if (mkdir(name, 0755) != 0 && errno != EEXIST)
            warn(name, strerror(errno));
        return;
    }
    if (type != '0') {
        warn(name, type == '1' || type == '2' ? "links are not supported, skipped" : "special file, skipped");
        return;
    }
    make_parents(name);
    out = fopen(name, "wb");
    if (!out) {
        warn(name, strerror(errno));
        return;
    }
    snprintf(out_path, sizeof out_path, "%s", name);
    out_mtime = mtime;
    data_kind = D_FILE;
}

static void end_entry(void)
{
    if (data_kind == D_FILE) {
        if (fclose(out) != 0)
            warn(out_path, "write error");
        out = nullptr;
        struct utimbuf t = {out_mtime, out_mtime};
        utime(out_path, &t);
    } else if (data_kind == D_LONGNAME) {
        meta[meta_len] = 0;
        snprintf(long_name, sizeof long_name, "%.*s", NAME_MAX_LEN - 1, meta);
    } else if (data_kind == D_PAX) {
        // 「長さ 名前=値\n」の並び（長さは、その行全体のバイト数）。path だけ使う
        for (int pos = 0; pos < meta_len;) {
            int len = atoi(meta + pos);
            if (len <= 0 || pos + len > meta_len)
                break;
            char *sp = memchr(meta + pos, ' ', (size_t)len);
            if (sp && !strncmp(sp + 1, "path=", 5)) {
                int vlen = (int)(meta + pos + len - 1 - (sp + 6));
                if (vlen > 0 && vlen < NAME_MAX_LEN)
                    snprintf(long_name, sizeof long_name, "%.*s", vlen, sp + 6);
            }
            pos += len;
        }
    }
    data_kind = D_SKIP;
}

// 展開した（または、そのままの）中身を受け取る
static int feed(void *ctx, const void *buf, int len)
{
    (void)ctx;
    const unsigned char *p = buf;
    while (len > 0 && !finished) {
        if (data_left > 0) {
            int n = data_left < len ? (int)data_left : len;
            if (data_kind == D_FILE) {
                if (fwrite(p, 1, (size_t)n, out) != (size_t)n) {
                    warn(out_path, "write error");
                    fclose(out);
                    out = nullptr;
                    data_kind = D_SKIP;
                }
            } else if (data_kind == D_LONGNAME || data_kind == D_PAX) {
                int room = (int)sizeof meta - 1 - meta_len;
                memcpy(meta + meta_len, p, (size_t)(n < room ? n : room));
                meta_len += n < room ? n : room;
            }
            p += n;
            len -= n;
            data_left -= n;
            if (data_left == 0)
                end_entry();
            continue;
        }
        if (pad_left > 0) {
            int n = pad_left < len ? (int)pad_left : len;
            p += n;
            len -= n;
            pad_left -= n;
            continue;
        }
        int n = BLOCK - header_len < len ? BLOCK - header_len : len;
        memcpy(header + header_len, p, (size_t)n);
        header_len += n;
        p += n;
        len -= n;
        if (header_len < BLOCK)
            break;
        header_len = 0;
        bool zero = true;
        for (int i = 0; i < BLOCK && zero; i++)
            zero = header[i] == 0;
        if (zero) {
            if (++zero_blocks == 2)
                finished = true;    // 0 のブロックが 2 つで終わり
            continue;
        }
        zero_blocks = 0;
        if (!checksum_ok()) {
            fprintf(stderr, "tar: This does not look like a tar archive\n");
            exit_status = 2;
            finished = true;
            return -1;
        }
        begin_entry();
        if (data_left == 0)
            end_entry();
    }
    return 0;
}

// ---- 入力（始めの 2 バイトを見てから、読み直せるように）----
static FILE *in;
static unsigned char peeked[2];
static int npeeked, peek_pos;

static int rd(void *ctx, void *buf, int size)
{
    (void)ctx;
    int n = 0;
    while (peek_pos < npeeked && n < size)
        ((unsigned char *)buf)[n++] = peeked[peek_pos++];
    if (n < size) {
        size_t k = fread((char *)buf + n, 1, (size_t)(size - n), in);
        if (k == 0 && ferror(in))
            return -1;
        n += (int)k;
    }
    return n;
}

static int usage(void)
{
    fprintf(stderr, "Usage: tar x|t [z] [v] [f FILE] [-C DIR] [--strip-components=N] [NAME...]\n"
                    "  tar xf FILE.tar.gz    extract        tar tf FILE    list        curl -sL URL | tar xz\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *file = nullptr, *dir = nullptr;
    static char *names[64];
    members = names;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--strip-components=", 19)) {
            opt_strip = atoi(a + 19);
        } else if (!strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "tar: unrecognized option '%s'\n", a);
            return usage();
        } else if (i == 1 || (a[0] == '-' && a[1])) {
            // まとめた文字（最初の引数は - がなくてもよい: tar xzf FILE）
            for (const char *p = a[0] == '-' ? a + 1 : a; *p; p++) {
                if (*p == 'x')
                    opt_extract = true;
                else if (*p == 't')
                    opt_list = true;
                else if (*p == 'v')
                    opt_verbose = true;
                else if (*p == 'z') {
                    // gzip は中身で決めるので、あってもなくても同じ
                } else if (*p == 'f' || *p == 'C') {
                    if (i + 1 >= argc)
                        return usage();
                    if (*p == 'f')
                        file = argv[++i];
                    else
                        dir = argv[++i];
                } else if (*p == 'c' || *p == 'r' || *p == 'u') {
                    fprintf(stderr, "tar: creating archives is not supported (only x and t)\n");
                    return 2;
                } else {
                    fprintf(stderr, "tar: invalid option -- '%c'\n", *p);
                    return usage();
                }
            }
        } else if (nmembers < 64) {
            names[nmembers++] = argv[i];
        }
    }
    if (opt_extract == opt_list)
        return usage();
    in = stdin;
    if (file && strcmp(file, "-")) {
        in = fopen(file, "rb");
        if (!in) {
            fprintf(stderr, "tar: %s: Cannot open: %s\n", file, strerror(errno));
            return 2;
        }
    }
    if (dir && chdir(dir) != 0) {
        fprintf(stderr, "tar: %s: Cannot open: %s\n", dir, strerror(errno));
        return 2;
    }
    npeeked = (int)fread(peeked, 1, 2, in);
    if (npeeked == 2 && peeked[0] == 0x1F && peeked[1] == 0x8B) {
        const char *err = nullptr;
        int r = monocs_inflate(MONOCS_INFLATE_GZIP, rd, nullptr, feed, nullptr, &err);
        if (r != 0 && r != -3 && !finished) {   // 書く側（feed）がやめたとき・終わりの印の後の余りは、ここでは言わない
            fprintf(stderr, "tar: gzip: %s\n", err ? err : "error");
            exit_status = 2;
        }
    } else {
        static unsigned char buf[16384];
        int n;
        while (!finished && (n = rd(nullptr, buf, sizeof buf)) > 0)
            if (feed(nullptr, buf, n) != 0)
                break;
    }
    if (out) {      // 途中で終わった
        fclose(out);
        warn(out_path, "Unexpected EOF in archive");
    } else if (!finished && exit_status == 0 && (header_len || data_left || pad_left || zero_blocks == 0) && nfiles == 0) {
        fprintf(stderr, "tar: This does not look like a tar archive\n");
        exit_status = 2;
    } else if (!finished && (data_left || header_len)) {
        fprintf(stderr, "tar: Unexpected EOF in archive\n");
        exit_status = 2;
    }
    return exit_status;
}
