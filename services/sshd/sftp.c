// sftp.c: SFTP のサーバー（版 3。OpenSSH の sftp・scp（9.0 から既定で SFTP）・sshfs が話すもの）。sshd の
// subsystem "sftp" のチャンネルのデータを受け取り、答えを返す（sshd.c の pump_sftp）。2026-11-16
//
// 受けるもの: OPEN・CLOSE・READ・WRITE・LSTAT・FSTAT・STAT・SETSTAT・FSETSTAT（日時だけ。大きさは 0 にする
// ときだけ）・OPENDIR・READDIR・REMOVE・MKDIR・RMDIR・REALPATH・RENAME（行き先があれば失敗。OpenSSH と同じ）、
// 拡張の posix-rename@openssh.com（置き換える）・statvfs@openssh.com（df）・fsync@openssh.com。
// READLINK・SYMLINK・ほかの拡張は OP_UNSUPPORTED（FAT にリンクはない）。
// ファイルの操作は POSIX の関数だけ（Linux でも同じものをコンパイルして試す。tools/ssh/test.sh）。
// パスは / から（Monocs のホームは /）。権限・持ち主は見ない（ログインした人は、シェルと同じくすべてに触れる）。
//
// 出典: draft-ietf-secsh-filexfer-02（SFTP 版 3）、OpenSSH の PROTOCOL（拡張）
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#if __has_include(<monocs.h>)
#include <monocs.h>       // utime の宣言（picolibc の utime.h には struct utimbuf だけ）・monocs_disk_sync
#define SYNC() monocs_disk_sync(true)
#else
#define SYNC() sync()
#endif
#include "sftp.h"

enum {
    FXP_INIT = 1, FXP_VERSION = 2, FXP_OPEN = 3, FXP_CLOSE = 4, FXP_READ = 5, FXP_WRITE = 6, FXP_LSTAT = 7,
    FXP_FSTAT = 8, FXP_SETSTAT = 9, FXP_FSETSTAT = 10, FXP_OPENDIR = 11, FXP_READDIR = 12, FXP_REMOVE = 13,
    FXP_MKDIR = 14, FXP_RMDIR = 15, FXP_REALPATH = 16, FXP_STAT = 17, FXP_RENAME = 18, FXP_READLINK = 19,
    FXP_SYMLINK = 20, FXP_STATUS = 101, FXP_HANDLE = 102, FXP_DATA = 103, FXP_NAME = 104, FXP_ATTRS = 105,
    FXP_EXTENDED = 200, FXP_EXTENDED_REPLY = 201,
};
enum {
    FX_OK = 0, FX_EOF = 1, FX_NO_SUCH_FILE = 2, FX_PERMISSION_DENIED = 3, FX_FAILURE = 4, FX_BAD_MESSAGE = 5,
    FX_OP_UNSUPPORTED = 8,
};
#define FXF_READ    0x01
#define FXF_WRITE   0x02
#define FXF_APPEND  0x04
#define FXF_CREAT   0x08
#define FXF_TRUNC   0x10
#define FXF_EXCL    0x20
#define ATTR_SIZE           0x00000001
#define ATTR_UIDGID         0x00000002
#define ATTR_PERMISSIONS    0x00000004
#define ATTR_ACMODTIME      0x00000008
#define ATTR_EXTENDED       0x80000000

// 同時に開いておける手の上限。表は要るだけ広げる（OpenSSH の sftp-server と同じく決まった数にしない。16 では、
// sshfs の上の bfs が読んでいる途中のフォルダを開いたまま下へ進み、足りずに EPERM になった。2026-11-20）
#define MAX_HANDLES 1024
#define MAX_READ    32768           // 1 回の READ で返す上限（短く返してよい。相手が続きを頼む）
#define IN_MAX      (1024 * 1024 + 65536)
#define OUT_MAX     (256 * 1024)
#define OUT_ROOM    (MAX_READ + 1024)   // 1 つの答えに要る空き
#define PATH_MAX_   1024

typedef struct {
    int used;           // 0 = 空き、1 = ファイル、2 = ディレクトリ
    int fd;
    // ディレクトリは、開いたときに名前を全部読んで閉じる（名前を \0 で区切って並べたもの）。開いたままにすると、
    // sshfs の先読みで重なったときに、カーネルの同時に開けるフォルダの数（os/fs/vfs.h）を使い切った（2026-11-20）
    char *names;
    size_t names_len, names_pos;
    char path[PATH_MAX_];
} Handle;

struct Sftp {
    uint8_t *in;
    size_t in_len;
    uint8_t *out;
    size_t out_len;
    Handle *h;          // 手の表（nh 個。new_handle が広げる）
    int nh;
};

// ---- 読み書き ----

typedef struct {
    const uint8_t *p;
    size_t n, i;
    int err;
} R;

static uint32_t r_u32(R *r)
{
    if (r->i + 4 > r->n) {
        r->err = 1;
        return 0;
    }
    const uint8_t *p = r->p + r->i;
    r->i += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t r_u64(R *r)
{
    uint64_t hi = r_u32(r);
    return hi << 32 | r_u32(r);
}

static const uint8_t *r_str(R *r, uint32_t *len)
{
    *len = r_u32(r);
    if (r->err || r->i + *len > r->n) {
        r->err = 1;
        *len = 0;
        return (const uint8_t *)"";
    }
    const uint8_t *s = r->p + r->i;
    r->i += *len;
    return s;
}

// 文字列を C の文字列に（長すぎれば誤り）
static bool r_cstr(R *r, char *buf, size_t size)
{
    uint32_t n;
    const uint8_t *s = r_str(r, &n);
    if (r->err || n >= size)
        return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return true;
}

static void w_raw(Sftp *s, const void *p, size_t n)
{
    if (s->out_len + n <= OUT_MAX) {
        memcpy(s->out + s->out_len, p, n);
        s->out_len += n;
    }
}

static void w_u32(Sftp *s, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    w_raw(s, b, 4);
}

static void w_u64(Sftp *s, uint64_t v)
{
    w_u32(s, (uint32_t)(v >> 32));
    w_u32(s, (uint32_t)v);
}

static void w_str(Sftp *s, const void *p, size_t n)
{
    w_u32(s, (uint32_t)n);
    w_raw(s, p, n);
}

static void w_cstr(Sftp *s, const char *p)
{
    w_str(s, p, strlen(p));
}

// 答えのパケットを始める（長さは end で埋める）
static size_t begin(Sftp *s, int type)
{
    size_t at = s->out_len;
    w_u32(s, 0);
    uint8_t t = (uint8_t)type;
    w_raw(s, &t, 1);
    return at;
}

static void end(Sftp *s, size_t at)
{
    uint32_t n = (uint32_t)(s->out_len - at - 4);
    s->out[at] = (uint8_t)(n >> 24);
    s->out[at + 1] = (uint8_t)(n >> 16);
    s->out[at + 2] = (uint8_t)(n >> 8);
    s->out[at + 3] = (uint8_t)n;
}

// text: 相手に見せる理由（nullptr なら番号の名前。版 3 の番号は少ないので、FX_FAILURE の理由はここで知らせる）
static void status_text(Sftp *s, uint32_t id, int code, const char *text)
{
    static const char *const msg[] = {"Success", "End of file", "No such file", "Permission denied", "Failure",
                                      "Bad message", "No connection", "Connection lost", "Operation unsupported"};
    size_t at = begin(s, FXP_STATUS);
    w_u32(s, id);
    w_u32(s, (uint32_t)code);
    w_cstr(s, text ? text : code >= 0 && code <= 8 ? msg[code] : "Failure");
    w_cstr(s, "");      // 言語
    end(s, at);
}

static void status(Sftp *s, uint32_t id, int code)
{
    status_text(s, id, code, nullptr);
}

static int errno_code(void);

// open・opendir などが失敗したとき（理由の文も付ける）
static void status_errno(Sftp *s, uint32_t id)
{
    status_text(s, id, errno_code(), strerror(errno));
}

static int errno_code(void)
{
    switch (errno) {
    case ENOENT:
    case ENOTDIR:
        return FX_NO_SUCH_FILE;
    case EACCES:
    case EPERM:
    case EROFS:
        return FX_PERMISSION_DENIED;
    default:
        return FX_FAILURE;
    }
}

static void w_attrs(Sftp *s, const struct stat *st)
{
    w_u32(s, ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME);
    w_u64(s, (uint64_t)st->st_size);
    w_u32(s, 0);    // uid
    w_u32(s, 0);    // gid
    uint32_t perm = S_ISDIR(st->st_mode) ? (0040000 | 0755) : (0100000 | 0644);
    w_u32(s, perm);
    w_u32(s, (uint32_t)st->st_mtime);   // atime（FAT にないので mtime と同じ）
    w_u32(s, (uint32_t)st->st_mtime);
}

// 相手の属性を読む（使うのは大きさと日時だけ）
typedef struct {
    uint32_t flags;
    uint64_t size;
    uint32_t mtime;
} Attrs;

static bool r_attrs(R *r, Attrs *a)
{
    a->flags = r_u32(r);
    if (a->flags & ATTR_SIZE)
        a->size = r_u64(r);
    if (a->flags & ATTR_UIDGID) {
        r_u32(r);
        r_u32(r);
    }
    if (a->flags & ATTR_PERMISSIONS)
        r_u32(r);
    if (a->flags & ATTR_ACMODTIME) {
        r_u32(r);
        a->mtime = r_u32(r);
    }
    if (a->flags & ATTR_EXTENDED) {
        uint32_t n = r_u32(r), len;
        for (uint32_t i = 0; i < n && !r->err; i++) {
            r_str(r, &len);
            r_str(r, &len);
        }
    }
    return !r->err;
}

// ---- パス ----

// path を / からのパスに直す（相対なら / から。. と .. を文字の上で畳む）
static void normalize(const char *path, char *out, size_t size)
{
    char tmp[PATH_MAX_];
    snprintf(tmp, sizeof tmp, "%s%s", path[0] == '/' ? "" : "/", path);
    char *parts[256];
    int n = 0;
    for (char *save, *t = strtok_r(tmp, "/", &save); t; t = strtok_r(nullptr, "/", &save)) {
        if (!strcmp(t, ".") || !*t)
            continue;
        if (!strcmp(t, "..")) {
            if (n > 0)
                n--;
            continue;
        }
        if (n < 256)
            parts[n++] = t;
    }
    size_t k = 0;
    out[0] = '\0';
    for (int i = 0; i < n && k + 1 < size; i++)
        k += (size_t)snprintf(out + k, size - k, "/%s", parts[i]);
    if (k == 0)
        snprintf(out, size, "/");
}

static bool r_path(R *r, char *out, size_t size)
{
    char raw[PATH_MAX_];
    if (!r_cstr(r, raw, sizeof raw))
        return false;
    normalize(raw, out, size);
    return true;
}

// ---- 手（ハンドル） ----

static int new_handle(Sftp *s)
{
    for (int i = 0; i < s->nh; i++)
        if (!s->h[i].used)
            return i;
    if (s->nh >= MAX_HANDLES)
        return -1;
    int n = s->nh ? s->nh * 2 : 16;
    Handle *h = realloc(s->h, (size_t)n * sizeof *h);
    if (!h)
        return -1;
    memset(h + s->nh, 0, (size_t)(n - s->nh) * sizeof *h);
    int i = s->nh;
    s->h = h;
    s->nh = n;
    return i;
}

static void w_handle(Sftp *s, uint32_t id, int i)
{
    size_t at = begin(s, FXP_HANDLE);
    w_u32(s, id);
    char b[8];
    int n = snprintf(b, sizeof b, "%d", i);
    w_str(s, b, (size_t)n);
    end(s, at);
}

static Handle *r_handle(Sftp *s, R *r, int kind)
{
    uint32_t n;
    const uint8_t *p = r_str(r, &n);
    if (r->err || n == 0 || n > 4)
        return nullptr;
    int i = 0;
    for (uint32_t k = 0; k < n; k++) {
        if (p[k] < '0' || p[k] > '9')
            return nullptr;
        i = i * 10 + (p[k] - '0');
    }
    if (i >= s->nh || !s->h[i].used || (kind && s->h[i].used != kind))
        return nullptr;
    return &s->h[i];
}

static void close_handle(Handle *h)
{
    if (h->used == 1)
        close(h->fd);
    else if (h->used == 2)
        free(h->names);
    h->used = 0;
}

// ---- 1 つのパケット ----

static void longname(char *buf, size_t size, const char *name, const struct stat *st)
{
    // ls -l の形（sftp の ls -l が表示する）
    struct tm tm;
    time_t t = st->st_mtime;
    localtime_r(&t, &tm);
    char when[32];
    strftime(when, sizeof when, "%b %e %H:%M", &tm);
    snprintf(buf, size, "%s    1 root     root     %10llu %s %s", S_ISDIR(st->st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
             (unsigned long long)st->st_size, when, name);
}

static void handle_packet(Sftp *s, const uint8_t *p, size_t n)
{
    R r = {p, n, 1, 0};
    int type = p[0];
    if (type == FXP_INIT) {
        size_t at = begin(s, FXP_VERSION);
        w_u32(s, 3);
        w_cstr(s, "posix-rename@openssh.com");
        w_cstr(s, "1");
        w_cstr(s, "statvfs@openssh.com");
        w_cstr(s, "2");
        w_cstr(s, "fsync@openssh.com");
        w_cstr(s, "1");
        end(s, at);
        return;
    }
    uint32_t id = r_u32(&r);
    if (r.err)
        return;
    char path[PATH_MAX_], path2[PATH_MAX_];
    struct stat st;
    switch (type) {
    case FXP_REALPATH: {
        if (!r_path(&r, path, sizeof path))
            break;
        size_t at = begin(s, FXP_NAME);
        w_u32(s, id);
        w_u32(s, 1);
        w_cstr(s, path);
        w_cstr(s, path);
        w_u32(s, 0);    // 属性なし
        end(s, at);
        return;
    }
    case FXP_STAT:
    case FXP_LSTAT: {
        if (!r_path(&r, path, sizeof path))
            break;
        if (stat(path, &st) != 0) {
            status(s, id, errno_code());
            return;
        }
        size_t at = begin(s, FXP_ATTRS);
        w_u32(s, id);
        w_attrs(s, &st);
        end(s, at);
        return;
    }
    case FXP_FSTAT: {
        Handle *h = r_handle(s, &r, 1);
        if (!h)
            break;
        if (fstat(h->fd, &st) != 0 && stat(h->path, &st) != 0) {
            status(s, id, errno_code());
            return;
        }
        size_t at = begin(s, FXP_ATTRS);
        w_u32(s, id);
        w_attrs(s, &st);
        end(s, at);
        return;
    }
    case FXP_OPEN: {
        uint32_t pflags;
        Attrs a;
        if (!r_path(&r, path, sizeof path))
            break;
        pflags = r_u32(&r);
        if (!r_attrs(&r, &a))
            break;
        int flags = (pflags & FXF_READ) && (pflags & FXF_WRITE) ? O_RDWR : (pflags & FXF_WRITE) ? O_WRONLY : O_RDONLY;
        if (pflags & FXF_APPEND)
            flags |= O_APPEND;
        if (pflags & FXF_CREAT)
            flags |= O_CREAT;
        if (pflags & FXF_TRUNC)
            flags |= O_TRUNC;
        if (pflags & FXF_EXCL)
            flags |= O_EXCL;
        int i = new_handle(s);
        if (i < 0) {
            status_text(s, id, FX_FAILURE, "Too many open handles");
            return;
        }
        int fd = open(path, flags, 0644);
        if (fd < 0) {
            status_errno(s, id);
            return;
        }
        s->h[i] = (Handle){.used = 1, .fd = fd};
        snprintf(s->h[i].path, sizeof s->h[i].path, "%s", path);
        w_handle(s, id, i);
        return;
    }
    case FXP_OPENDIR: {
        if (!r_path(&r, path, sizeof path))
            break;
        int i = new_handle(s);
        if (i < 0) {
            status_text(s, id, FX_FAILURE, "Too many open handles");
            return;
        }
        DIR *d = opendir(path);
        if (!d) {
            status_errno(s, id);
            return;
        }
        char *names = nullptr;
        size_t len = 0, cap = 0;
        bool ok = true;
        struct dirent *e;
        while (ok && (e = readdir(d))) {
            size_t n = strlen(e->d_name) + 1;
            if (len + n > cap) {
                size_t nc = cap ? cap * 2 : 4096;
                while (nc < len + n)
                    nc *= 2;
                char *p = realloc(names, nc);
                if (!p)
                    ok = false;
                else
                    names = p, cap = nc;
            }
            if (ok) {
                memcpy(names + len, e->d_name, n);
                len += n;
            }
        }
        closedir(d);
        if (!ok) {
            free(names);
            status(s, id, FX_FAILURE);
            return;
        }
        s->h[i] = (Handle){.used = 2, .names = names, .names_len = len};
        snprintf(s->h[i].path, sizeof s->h[i].path, "%s", path);
        w_handle(s, id, i);
        return;
    }
    case FXP_READDIR: {
        Handle *h = r_handle(s, &r, 2);
        if (!h)
            break;
        // 1 回に 50 個まで。なくなったら EOF
        size_t at = begin(s, FXP_NAME);
        w_u32(s, id);
        size_t count_at = s->out_len;
        w_u32(s, 0);
        uint32_t count = 0;
        while (count < 50 && s->out_len + 2048 < OUT_MAX && h->names_pos < h->names_len) {
            const char *name = h->names + h->names_pos;
            h->names_pos += strlen(name) + 1;
            int pn = snprintf(path2, sizeof path2, "%s%s%s", h->path, strcmp(h->path, "/") ? "/" : "", name);
            if (pn >= (int)sizeof path2 || stat(path2, &st) != 0)
                memset(&st, 0, sizeof st);
            char ln[PATH_MAX_ + 80];
            longname(ln, sizeof ln, name, &st);
            w_cstr(s, name);
            w_cstr(s, ln);
            w_attrs(s, &st);
            count++;
        }
        if (count == 0) {
            s->out_len = at;
            status(s, id, FX_EOF);
            return;
        }
        s->out[count_at] = (uint8_t)(count >> 24);
        s->out[count_at + 1] = (uint8_t)(count >> 16);
        s->out[count_at + 2] = (uint8_t)(count >> 8);
        s->out[count_at + 3] = (uint8_t)count;
        end(s, at);
        return;
    }
    case FXP_CLOSE: {
        Handle *h = r_handle(s, &r, 0);
        if (!h)
            break;
        close_handle(h);
        status(s, id, FX_OK);
        return;
    }
    case FXP_READ: {
        Handle *h = r_handle(s, &r, 1);
        uint64_t off = r_u64(&r);
        uint32_t len = r_u32(&r);
        if (!h || r.err)
            break;
        if (len > MAX_READ)
            len = MAX_READ;
        if (lseek(h->fd, (off_t)off, SEEK_SET) < 0) {
            status(s, id, errno_code());
            return;
        }
        size_t at = begin(s, FXP_DATA);
        w_u32(s, id);
        size_t len_at = s->out_len;
        w_u32(s, 0);
        ssize_t k = read(h->fd, s->out + s->out_len, len);
        if (k <= 0) {
            s->out_len = at;
            status(s, id, k == 0 ? FX_EOF : errno_code());
            return;
        }
        s->out_len += (size_t)k;
        s->out[len_at] = (uint8_t)(k >> 24);
        s->out[len_at + 1] = (uint8_t)(k >> 16);
        s->out[len_at + 2] = (uint8_t)(k >> 8);
        s->out[len_at + 3] = (uint8_t)k;
        end(s, at);
        return;
    }
    case FXP_WRITE: {
        Handle *h = r_handle(s, &r, 1);
        uint64_t off = r_u64(&r);
        uint32_t len;
        const uint8_t *data = r_str(&r, &len);
        if (!h || r.err)
            break;
        if (lseek(h->fd, (off_t)off, SEEK_SET) < 0) {
            status(s, id, errno_code());
            return;
        }
        size_t done = 0;
        while (done < len) {
            ssize_t k = write(h->fd, data + done, len - done);
            if (k <= 0) {
                status(s, id, errno_code());
                return;
            }
            done += (size_t)k;
        }
        status(s, id, FX_OK);
        return;
    }
    case FXP_SETSTAT:
    case FXP_FSETSTAT: {
        Attrs a = {0};
        Handle *h = nullptr;
        if (type == FXP_SETSTAT) {
            if (!r_path(&r, path, sizeof path))
                break;
        } else {
            h = r_handle(s, &r, 0);
            if (!h)
                break;
            snprintf(path, sizeof path, "%s", h->path);
        }
        if (!r_attrs(&r, &a))
            break;
        // 大きさ: 今と同じか、0 にするときだけ（ftruncate がない）。権限・持ち主は受け流す。日時は utime
        if (a.flags & ATTR_SIZE) {
            // 開いているファイルは、その手で今の大きさを見る（Monocs の stat は、閉じる前は書く前の大きさを返す。
            // scp は書き終えた後に、書いた大きさで FSETSTAT する）
            off_t cur = -1;
            if (h && h->used == 1)
                cur = lseek(h->fd, 0, SEEK_END);
            if (cur < 0) {
                if (stat(path, &st) != 0) {
                    status(s, id, errno_code());
                    return;
                }
                cur = st.st_size;
            }
            if ((uint64_t)cur != a.size) {
                if (a.size != 0) {
                    status(s, id, FX_OP_UNSUPPORTED);
                    return;
                }
                int fd = open(path, O_WRONLY | O_TRUNC);
                if (fd < 0) {
                    status(s, id, errno_code());
                    return;
                }
                close(fd);
            }
        }
        if (a.flags & ATTR_ACMODTIME) {
            struct utimbuf ut = {.actime = (time_t)a.mtime, .modtime = (time_t)a.mtime};
            if (utime(path, &ut) != 0) {
                status(s, id, errno_code());
                return;
            }
        }
        status(s, id, FX_OK);
        return;
    }
    case FXP_REMOVE:
        if (!r_path(&r, path, sizeof path))
            break;
        status(s, id, unlink(path) == 0 ? FX_OK : errno_code());
        return;
    case FXP_MKDIR: {
        Attrs a;
        if (!r_path(&r, path, sizeof path) || !r_attrs(&r, &a))
            break;
        status(s, id, mkdir(path, 0755) == 0 ? FX_OK : errno_code());
        return;
    }
    case FXP_RMDIR:
        if (!r_path(&r, path, sizeof path))
            break;
        status(s, id, rmdir(path) == 0 ? FX_OK : errno_code());
        return;
    case FXP_RENAME:
        if (!r_path(&r, path, sizeof path) || !r_path(&r, path2, sizeof path2))
            break;
        if (stat(path2, &st) == 0) {    // 版 3 の RENAME は、行き先があれば失敗（OpenSSH と同じ）
            status(s, id, FX_FAILURE);
            return;
        }
        status(s, id, rename(path, path2) == 0 ? FX_OK : errno_code());
        return;
    case FXP_READLINK:
    case FXP_SYMLINK:
        status(s, id, FX_OP_UNSUPPORTED);
        return;
    case FXP_EXTENDED: {
        char name[64];
        if (!r_cstr(&r, name, sizeof name))
            break;
        if (!strcmp(name, "posix-rename@openssh.com")) {
            if (!r_path(&r, path, sizeof path) || !r_path(&r, path2, sizeof path2))
                break;
            status(s, id, rename(path, path2) == 0 ? FX_OK : errno_code());
            return;
        }
        if (!strcmp(name, "statvfs@openssh.com")) {
            if (!r_path(&r, path, sizeof path))
                break;
            struct statvfs v;
            if (statvfs(path, &v) != 0) {
                status(s, id, errno_code());
                return;
            }
            size_t at = begin(s, FXP_EXTENDED_REPLY);
            w_u32(s, id);
            w_u64(s, v.f_bsize);
            w_u64(s, v.f_frsize);
            w_u64(s, v.f_blocks);
            w_u64(s, v.f_bfree);
            w_u64(s, v.f_bavail);
            w_u64(s, v.f_files);
            w_u64(s, v.f_ffree);
            w_u64(s, v.f_favail);
            w_u64(s, 0);    // fsid
            w_u64(s, 0);    // flag
            w_u64(s, 255);  // namemax
            end(s, at);
            return;
        }
        if (!strcmp(name, "fsync@openssh.com")) {
            Handle *h = r_handle(s, &r, 1);
            if (!h)
                break;
            SYNC();     // SD カードに書き出す（Monocs に fsync はない）
            status(s, id, FX_OK);
            return;
        }
        status(s, id, FX_OP_UNSUPPORTED);
        return;
    }
    default:
        status(s, id, FX_OP_UNSUPPORTED);
        return;
    }
    status(s, id, FX_BAD_MESSAGE);
}

// ---- 外から ----

Sftp *sftp_new(void)
{
    Sftp *s = calloc(1, sizeof *s);
    if (!s)
        return nullptr;
    s->in = malloc(IN_MAX);
    s->out = malloc(OUT_MAX);
    if (!s->in || !s->out) {
        sftp_free(s);
        return nullptr;
    }
    return s;
}

void sftp_free(Sftp *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->nh; i++)
        if (s->h[i].used)
            close_handle(&s->h[i]);
    free(s->h);
    free(s->in);
    free(s->out);
    free(s);
}

bool sftp_feed(Sftp *s, const void *data, size_t n)
{
    if (s->in_len + n > IN_MAX)
        return false;
    memcpy(s->in + s->in_len, data, n);
    s->in_len += n;
    return true;
}

size_t sftp_run(Sftp *s, bool *bad)
{
    size_t used = 0;
    *bad = false;
    while (s->in_len - used >= 4 && OUT_MAX - s->out_len >= OUT_ROOM) {
        const uint8_t *p = s->in + used;
        uint32_t n = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        if (n == 0 || n > IN_MAX - 4) {
            *bad = true;
            break;
        }
        if (s->in_len - used < 4 + (size_t)n)
            break;
        handle_packet(s, p + 4, n);
        used += 4 + n;
    }
    memmove(s->in, s->in + used, s->in_len - used);
    s->in_len -= used;
    return used;
}

const uint8_t *sftp_output(Sftp *s, size_t *len)
{
    *len = s->out_len;
    return s->out;
}

void sftp_consumed(Sftp *s, size_t n)
{
    memmove(s->out, s->out + n, s->out_len - n);
    s->out_len -= n;
}

size_t sftp_pending_input(Sftp *s)
{
    return s->in_len;
}
