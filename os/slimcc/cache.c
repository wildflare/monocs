// コンパイルのキャッシュ: run でコンパイルした結果を SD カードに保存し、ソースとヘッダーが
// 変わっていなければ、次からはコンパイルせずに読み込む（電源を切っても残る）。
//
// 置き場所: /var/cache/slimcc/<コンパイラの識別>/<キー>（2026-11-19 から。run のときだけ使う）
//   コンパイラの識別: 中のコンパイラのソースなど、コンパイラの出力に効くもののハッシュ（ビルドのときに
//     tools/compiler_id.sh が作る SLIMCC_COMPILER_ID）。コンパイラが変われば別の場所になる。今のもの以外は、
//     最初に使うときに消す。2026-11-16 まではカーネル全体のハッシュで、シェルやドライバを直しただけでも
//     アプリをすべてコンパイルし直していた。結果にカーネルの番地は入らない（つなぐのは動かすたび）
//   キー: ソースの絶対パスとコンパイルのオプションのハッシュ（同じファイルを直せば、同じ場所に書き直す）
// 中身（リトルエンディアン。文字列は長さ u32 と中身）:
//   "NOSC" u32 版、u64 コンパイラの識別、ソースの絶対パス、オプション、
//   u32 読んだファイルの数、各: 絶対パス、u64 大きさ、u64 中身のハッシュ、u32 FAT の日時（0 なら使わない）、
//   u32 調べてなかったパスの数、各: 絶対パス、
//   コンパイルした結果（slimcc_obj_save）、u64 ここまでのハッシュ
// 使えるのは、全体のハッシュ（書きかけ・壊れたものを除く）と、読んだファイルの中身のハッシュがすべて
// 一致し、調べてなかったパス（ヘッダーの検索で先に探した場所など）が今もないときだけ。
// ファイルの日時（2026-11-21 から。ユーザーと合意）: 大きさと FAT の日時が保存したときと同じなら、中身を読まずに
// 同じとみなす（再起動の後、vim の起動でソース・ヘッダー約 18MB を読み直していた）。git の「racy」の扱いと同じく、
// 保存したときに日時が今から STAMP_MARGIN 秒の内（か先）のファイルは日時を残さない（0。次も中身で確かめる）。FAT の
// 日時は 2 秒刻みなので、同じ刻みの内に同じ大きさで書き換えられると見分けられないため。残した日時より後の書き換えは、
// 時計が戻らない限り違う日時になる。時計が戻るのは、電源を切った後の起動（/etc/fake-hwclock.data の時刻は最大
// 60 秒古い）と、手で日時を戻したとき。日時が違えば今までどおり中身のハッシュで確かめる（日時だけ変わったものも使える）。
// __DATE__・__TIME__・__TIMESTAMP__ を使ったもの（slimcc_cacheable が false）は保存しない。

// カーネルは -std=c23（ISO）でビルドするので、POSIX の宣言（PATH_MAX、realpath、open_memstream など）を見えるようにする
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cache.h"
#include "../libc/heap.h"
#include "../fs/vfs.h"
#include "../../build/gen/compiler_id.h"     // 中のコンパイラの識別（tools/compiler_id.sh）

#define CACHE_ROOT      "/var/cache"
#define CACHE_DIR       "/var/cache/slimcc"
#define CACHE_VERSION   4       // 4: 読んだファイルに FAT の日時（2026-11-21）
#define FNV_OFFSET      0xcbf29ce484222325ULL
#define FNV_PRIME       0x100000001b3ULL

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * FNV_PRIME;
    return h;
}

static uint64_t fnv64_str(uint64_t h, const char *s)
{
    return fnv64(h, s, strlen(s) + 1);
}

static uint64_t get_compiler_id(void)
{
    return SLIMCC_COMPILER_ID | 1;
}

// ---- 読んだファイルのハッシュ（どのコアからも使う） ----
// 2026-11-20 から、アプリの実行をまたいで覚え、VFS がそのパスのファイルを書き換える・消す・名前を変えるときに忘れる
// （cache_file_changed。os/fs/vfs.c の fc_invalidate から）。前は cache_begin〜cache_end の間だけ覚え、vim を起動する
// たびに 134 個のソースとヘッダー（約 13MB）を読み直してハッシュしていた（起動 2.1 秒の大部分）。ファイルの書き換えは
// すべて VFS を通る（SD カードを PC で書き換えたら再起動する。ファイルのキャッシュと同じ前提）。パスの大文字・小文字は
// 区別しない（FAT と同じ）

typedef struct DepHash DepHash;
struct DepHash {
    DepHash *next;
    uint64_t size, hash;
    bool ok;
    char path[];
};

// パスのハッシュで引く表（2026-11-16。前は 1 本のリストを頭から比べ、その間ロックを持ったままだったので、
// mi（38 ファイル、それぞれ百を超えるヘッダー）の起動で 3 つのコアがここで順番待ちになり、0.7〜13 秒かかった）
#define DEP_BUCKETS 1024
#define DEP_MAX     20000   // 覚える数の上限（超えたら全部忘れる）
static DepHash *dep_hashes[DEP_BUCKETS];
static int dep_count;
static atomic_uint dep_gen;     // 書き換えを見るたびに進める（読んでいる間に書き換えられたものを覚えない）
static atomic_uint src_gen;     // 覚えていたファイルが書き換えられた・忘れたときに進める（cache_src_gen）
static atomic_int dep_lock;
static int users;
static uint64_t hits, misses;   // dep_lock で守る

static void lock(void)
{
    // 待つ間は読むだけ（書き込みを試し続けると、持っているコアの解放が遅れる）
    while (atomic_exchange_explicit(&dep_lock, 1, memory_order_acquire))
        while (atomic_load_explicit(&dep_lock, memory_order_relaxed))
            __asm__ volatile("yield");
}

static void unlock(void)
{
    atomic_store_explicit(&dep_lock, 0, memory_order_release);
}

// ファイルの中身を読む（全体。呼んだ側が free）。なければ NULL
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    char *buf = nullptr;
    long size;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) &&
        (buf = malloc((size_t)size + 1))) {
        if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
            free(buf);
            buf = nullptr;
        } else {
            *len = (size_t)size;
        }
    }
    fclose(f);
    return buf;
}

// 大文字・小文字をそろえたパスのハッシュ（表の枠）
static unsigned dep_bucket(const char *path)
{
    uint64_t h = FNV_OFFSET;
    for (const char *p = path; *p; p++)
        h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 0x100000001b3ULL;
    return (unsigned)(h % DEP_BUCKETS);
}

// 全部忘れる（dep_lock の中で）
static void dep_clear_locked(void)
{
    atomic_fetch_add(&src_gen, 1);  // 覚えていたものを忘れる: それを使ったつないだ結果も使えない
    for (int b = 0; b < DEP_BUCKETS; b++)
        while (dep_hashes[b]) {
            DepHash *next = dep_hashes[b]->next;
            free(dep_hashes[b]);
            dep_hashes[b] = next;
        }
    dep_count = 0;
}

// VFS がファイルを書き換える・消す・名前を変える（key は絶対パス、小文字。NULL ならすべて）: そのパスと、その下の
// パス（フォルダの名前を変えたとき）を忘れる
void cache_file_changed(const char *key)
{
    atomic_fetch_add(&dep_gen, 1);
    image_cache_changed(key);   // つないだ結果の覚え（os/slimcc/run.c）: そのプログラムのフォルダの中が変わった
    lock();
    if (!key) {
        dep_clear_locked();
        unlock();
        return;
    }
    size_t kl = strlen(key);
    for (int b = 0; b < DEP_BUCKETS; b++)
        for (DepHash **pp = &dep_hashes[b]; *pp;) {
            DepHash *d = *pp;
            if (!strncasecmp(d->path, key, kl) && (d->path[kl] == '\0' || d->path[kl] == '/' || kl == 1)) {
                *pp = d->next;
                free(d);
                dep_count--;
                atomic_fetch_add(&src_gen, 1);
            } else {
                pp = &d->next;
            }
        }
    unlock();
}

unsigned cache_src_gen(void)
{
    return atomic_load(&src_gen);
}

// 覚えたハッシュを引く。覚えていれば true（*ok にあるか）
static bool dep_known(const char *path, uint64_t *size, uint64_t *hash, bool *ok)
{
    unsigned b = dep_bucket(path);
    lock();
    for (DepHash *d = dep_hashes[b]; d; d = d->next)
        if (!strcasecmp(d->path, path)) {
            unlock();
            *size = d->size;
            *hash = d->hash;
            *ok = d->ok;
            return true;
        }
    unlock();
    return false;
}

// 覚える（gen は調べ始める前の dep_gen。その間にどこかのファイルが書き換えられたら覚えない）
static void dep_remember(const char *path, unsigned gen, bool ok, uint64_t len, uint64_t h)
{
    unsigned b = dep_bucket(path);
    // アプリの実行をまたいで持つので、カーネル用のヒープから（アプリ用はアプリの終わりに捨てる）
    malloc_kernel_begin();
    DepHash *d = malloc(sizeof(DepHash) + strlen(path) + 1);
    malloc_kernel_end();
    if (!d)
        return;
    strcpy(d->path, path);
    d->ok = ok;
    d->size = len;
    d->hash = h;
    lock();
    if (atomic_load(&dep_gen) != gen) {
        free(d);            // 読んでいる間に、どこかのファイルが書き換えられた（覚えない）
    } else {
        if (dep_count >= DEP_MAX)
            dep_clear_locked();
        d->next = dep_hashes[b];
        dep_hashes[b] = d;
        dep_count++;
    }
    unlock();
}

static bool dep_hash(const char *path, uint64_t *size, uint64_t *hash)
{
    bool ok;
    if (dep_known(path, size, hash, &ok))
        return ok;
    unsigned gen = atomic_load(&dep_gen);
    size_t len = 0;
    char *buf = read_all(path, &len);
    ok = buf != nullptr;
    uint64_t h = buf ? fnv64(FNV_OFFSET, buf, len) : 0;
    free(buf);
    *size = len;
    *hash = h;
    dep_remember(path, gen, ok, len, h);
    return ok;
}

// ---- 古いカーネルのキャッシュを消す（起動して最初に使うとき） ----

static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    char path[PATH_MAX];
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(path);
        else
            unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static bool cleaned;

static void clean_old(void)
{
    if (cleaned)
        return;
    cleaned = true;
    char mine[24];
    snprintf(mine, sizeof(mine), "%016llx", (unsigned long long)get_compiler_id());
    DIR *d = opendir(CACHE_DIR);
    if (!d)
        return;
    char names[16][64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 16)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcasecmp(e->d_name, mine))
            snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    closedir(d);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", CACHE_DIR, names[i]);
        remove_tree(path);
    }
}

void cache_begin(void)
{
    lock();
    bool first = users++ == 0;
    unlock();
    if (first)
        clean_old();
}

void cache_end(void)
{
    lock();
    --users;    // 読んだファイルのハッシュは忘れない（cache_file_changed が書き換えを見て忘れる）
    unlock();
}

void cache_stats(uint64_t *h, uint64_t *m)
{
    *h = hits;
    *m = misses;
}

// 今のカーネルのキャッシュの数と大きさ
void cache_usage(int *files, uint64_t *bytes)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%016llx", CACHE_DIR, (unsigned long long)get_compiler_id());
    *files = 0;
    *bytes = 0;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            (*files)++;
            *bytes += (uint64_t)st.st_size;
        }
    }
    closedir(d);
}

void cache_clear(void)
{
    remove_tree(CACHE_DIR);
    image_cache_changed(nullptr);   // メモリに覚えたつないだ結果も捨てる（os/slimcc/run.c）
}


// ---- 読み込み ----

typedef struct {
    const unsigned char *p, *end;
} Reader;

static bool rd(Reader *r, void *out, size_t n)
{
    if ((size_t)(r->end - r->p) < n)
        return false;
    memcpy(out, r->p, n);
    r->p += n;
    return true;
}

static bool rd_str(Reader *r, char *out, size_t cap)
{
    uint32_t n;
    if (!rd(r, &n, 4) || n >= cap || (size_t)(r->end - r->p) < n)
        return false;
    memcpy(out, r->p, n);
    out[n] = '\0';
    r->p += n;
    return true;
}

// 1 つのプログラムのキャッシュを消す（2026-11-21。ユーザーの「vim のキャッシュだけ削除」）: abs（絶対パス）がファイルなら
// そのファイルの、フォルダならその下のソースのもの。中身の頭にあるソースの絶対パスで決める（場所の名前はハッシュ）。
// 消した数を返す
int cache_clear_program(const char *abs, bool dir)
{
    char cdir[64];
    snprintf(cdir, sizeof(cdir), "%s/%016llx", CACHE_DIR, (unsigned long long)get_compiler_id());
    size_t al = strlen(abs);
    while (al > 1 && abs[al - 1] == '/')
        al--;
    int removed = 0;
    DIR *d = opendir(cdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", cdir, e->d_name);
            FILE *f = fopen(path, "rb");
            if (!f)
                continue;
            unsigned char head[4 + 4 + 8 + 4 + PATH_MAX];
            size_t n = fread(head, 1, sizeof(head), f);
            fclose(f);
            Reader r = {head, head + n};
            char magic[4], src[PATH_MAX];
            uint32_t ver;
            uint64_t kid;
            if (!rd(&r, magic, 4) || memcmp(magic, "NOSC", 4) || !rd(&r, &ver, 4) || !rd(&r, &kid, 8) ||
                !rd_str(&r, src, sizeof(src)))
                continue;
            bool mine = dir ? !strncasecmp(src, abs, al) && (src[al] == '/' || al == 1) : !strcasecmp(src, abs);
            if (mine && remove(path) == 0)
                removed++;
        }
        closedir(d);
    }
    char key[PATH_MAX];
    size_t i = 0;
    for (; i < al && i + 1 < sizeof(key); i++)
        key[i] = (char)tolower((unsigned char)abs[i]);
    key[i] = '\0';
    image_cache_changed(key);
    return removed;
}

static void entry_path(char *out, size_t cap, const char *abs, const char *optkey)
{
    uint64_t key = fnv64_str(fnv64_str(FNV_OFFSET, abs), optkey);
    snprintf(out, cap, "%s/%016llx/%016llx", CACHE_DIR, (unsigned long long)get_compiler_id(),
             (unsigned long long)key);
}

// FAT の日時（地方時の 2 秒刻み）を、比べるための秒に（1980 年からの通しの秒。TZ は使わない）
static int64_t stamp_seconds(uint32_t stamp)
{
    int y = (int)(stamp >> 25) + 1980, m = (int)(stamp >> 21 & 15), d = (int)(stamp >> 16 & 31);
    int hh = (int)(stamp >> 11 & 31), mm = (int)(stamp >> 5 & 63), ss = (int)(stamp & 31) * 2;
    // 日付 → 通しの日（Howard Hinnant の days_from_civil）
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

#define STAMP_MARGIN 4      // 秒（FAT の 2 秒刻みと、時刻の読み違いの分）
#define STAMP_UNSET  ((uint32_t)(2026 - 1980) << 25 | 1 << 21 | 1 << 16)   // 時計が合う前の日時（vfs.c の get_fattime）

// 保存するときの日時: 今の内容と同じ大きさで、今より STAMP_MARGIN 秒以上前の日時なら、その日時。ほかは 0（使わない）
static uint32_t stamp_to_keep(const char *path, uint64_t size)
{
    uint64_t sz;
    uint32_t st;
    if (!vfs_file_stamp(path, &sz, &st) || sz != size || !st)
        return 0;
    if (st == STAMP_UNSET)
        return 0;   // 時計が合う前に書いたもの（どれも同じ日時になるので、書き換えを見分けられない）
    if (stamp_seconds(st) + STAMP_MARGIN > stamp_seconds(vfs_now_stamp()))
        return 0;   // 書き換えたばかり（か、時計より先の日時）
    return st;
}

static SlimccObj *try_load(const char *file, const char *abs, const char *optkey)
{
    size_t len;
    char *buf = read_all(file, &len);
    if (!buf)
        return nullptr;
    SlimccObj *obj = nullptr;
    uint64_t sum;
    if (len < 8 || (memcpy(&sum, buf + len - 8, 8), sum != fnv64(FNV_OFFSET, buf, len - 8)))
        goto out;   // 書きかけ・壊れている
    Reader r = {(const unsigned char *)buf, (const unsigned char *)buf + len - 8};
    char magic[4], s[PATH_MAX];
    uint32_t ver, ndeps;
    uint64_t kid;
    if (!rd(&r, magic, 4) || memcmp(magic, "NOSC", 4) || !rd(&r, &ver, 4) || ver != CACHE_VERSION ||
        !rd(&r, &kid, 8) || kid != get_compiler_id() || !rd_str(&r, s, sizeof(s)) || strcmp(s, abs) ||
        !rd_str(&r, s, sizeof(s)) || strcmp(s, optkey) || !rd(&r, &ndeps, 4))
        goto out;
    for (uint32_t i = 0; i < ndeps; i++) {
        uint64_t size, hash, size2, hash2;
        uint32_t stamp, stamp2;
        if (!rd_str(&r, s, sizeof(s)) || !rd(&r, &size, 8) || !rd(&r, &hash, 8) || !rd(&r, &stamp, 4))
            goto out;
        bool ok2;
        if (dep_known(s, &size2, &hash2, &ok2)) {
            if (!ok2 || size != size2 || hash != hash2)
                goto out;
            continue;
        }
        if (stamp) {
            // 大きさと日時が同じなら、中身を読まずに同じとみなし、覚える（ほかの .c も同じヘッダーを読むので、
            // 2 回目からは stat もしない。最初の版は毎回 stat して、vim の 133 個の部品で約 2 万回 VFS のロックを取り、
            // 前より遅くなった）
            unsigned gen = atomic_load(&dep_gen);
            if (vfs_file_stamp(s, &size2, &stamp2) && size2 == size && stamp2 == stamp) {
                dep_remember(s, gen, true, size, hash);
                continue;
            }
        }
        if (!dep_hash(s, &size2, &hash2) || size != size2 || hash != hash2)
            goto out;   // ソースかヘッダーが変わった（なくなった）
    }
    uint32_t nabsent;
    if (!rd(&r, &nabsent, 4))
        goto out;
    for (uint32_t i = 0; i < nabsent; i++) {
        uint64_t size, hash;
        if (!rd_str(&r, s, sizeof(s)) || dep_hash(s, &size, &hash))
            goto out;   // 前はなかったファイルが増えた（先に見つかるヘッダーが変わる）
    }
    obj = slimcc_obj_load_mem(r.p, (size_t)(r.end - r.p));
out:
    free(buf);
    return obj;
}

// ---- 書き込み ----

static void put_str(FILE *f, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    fwrite(&n, 4, 1, f);
    fwrite(s, 1, n, f);
}

static void make_dirs(const char *file)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    mkdir(CACHE_ROOT, 0755);
    mkdir(CACHE_DIR, 0755);
    mkdir(dir, 0755);
}

static void save(const char *file, const char *abs, const char *optkey, SlimccObj *obj)
{
    int n;
    const char *const *deps = slimcc_deps(&n);
    char *buf = nullptr;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    if (!m)
        return;
    fwrite("NOSC", 1, 4, m);
    uint32_t ver = CACHE_VERSION;
    fwrite(&ver, 4, 1, m);
    uint64_t kid = get_compiler_id();
    fwrite(&kid, 8, 1, m);
    put_str(m, abs);
    put_str(m, optkey);
    uint32_t nd = (uint32_t)n;
    fwrite(&nd, 4, 1, m);
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        char dabs[PATH_MAX];
        uint64_t size, hash;
        ok = realpath(deps[i], dabs) && dep_hash(dabs, &size, &hash);
        if (ok) {
            uint32_t stamp = stamp_to_keep(dabs, size);
            put_str(m, dabs);
            fwrite(&size, 8, 1, m);
            fwrite(&hash, 8, 1, m);
            fwrite(&stamp, 4, 1, m);
        }
    }
    int na;
    const char *const *absent = slimcc_absent(&na);
    uint32_t nab = (uint32_t)na;
    fwrite(&nab, 4, 1, m);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        ok = false;
    for (int i = 0; i < na && ok; i++) {
        char p[PATH_MAX];
        if (absent[i][0] == '/')
            snprintf(p, sizeof(p), "%s", absent[i]);
        else
            snprintf(p, sizeof(p), "%s/%s", strcmp(cwd, "/") ? cwd : "", absent[i]);
        put_str(m, p);
    }
    ok = ok && slimcc_obj_save(obj, m);
    fclose(m);
    if (ok) {
        uint64_t sum = fnv64(FNV_OFFSET, buf, len);
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", file);
        make_dirs(file);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool w = fwrite(buf, 1, len, f) == len && fwrite(&sum, 8, 1, f) == 1;
            if (fclose(f) != 0 || !w || rename(tmp, file) != 0)
                unlink(tmp);
        }
    }
    free(buf);
}

SlimccObj *cache_compile(const char *path, const SlimccOptions *opt, const char *optkey)
{
    char abs[PATH_MAX], file[PATH_MAX];
    if (!realpath(path, abs))
        return slimcc_compile(path, nullptr, opt);
    entry_path(file, sizeof(file), abs, optkey);
    SlimccObj *obj = try_load(file, abs, optkey);
    lock();
    if (obj)
        hits++;
    else
        misses++;
    unlock();
    if (obj)
        return obj;
    obj = slimcc_compile(path, nullptr, opt);
    if (obj && slimcc_cacheable())
        save(file, abs, optkey, obj);
    return obj;
}
