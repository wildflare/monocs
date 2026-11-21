// VFS: POSIX のファイル API を FatFs とコンソールに振り分ける
// 出典: picolibc doc/os.md（picolibc が OS に求める関数）、FatFs documents/doc/*.html

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/utime.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../libc/console.h"
#include "../kernel/clock.h"
#include "ff.h"
#include "vfs.h"
#include "../libc/socket_os.h"
#include "../libc/pipe_os.h"
#include "../libc/heap.h"
#include "../drivers/uart.h"
#include "../kernel/lock.h"
#include "writeback.h"
#include "../drivers/sd.h"
#include "../kernel/svc.h"
// コンパイルのキャッシュが覚えたハッシュを忘れさせる（os/slimcc/cache.c。cache.h はコンパイラのヘッダーを読むので宣言だけ）
void cache_file_changed(const char *key);

#define FD_CONSOLE_COUNT    3   // stdin、stdout、stderr

// picolibc の FILE のバッファの大きさ（picolibc は fstat-bufsiz で st_blksize を使う）。
// 大きくすると、FatFs が複数のセクタをまとめて読み書きでき、VFS の呼び出しの回数も減る
// （複数のコアが同時にファイルを読むとき、VFS のロックを待つ時間が減る）
#define VFS_BLKSIZE         16384

// ファイルの表と FatFs（作業用の領域、カレントディレクトリなど）を守るロック。
// 公開する関数は、このロックを取ってから *_unlocked を呼ぶ（ファイルの末尾）。
// FatFs の FF_FS_REENTRANT は使わない（FatFs を呼ぶのは VFS だけで、ファイルの表も同じロックで守るため）
struct klock vfs_lock = KLOCK_INIT;

// SD カードを使わない部分を守るロック（2026-11-12）: ファイルのキャッシュ、ないとわかっているパスの覚え、
// ファイルの表の空き（used・fc・owner）、カレントディレクトリの写し（cwd）。キャッシュに当たる open・stat と、
// キャッシュから開いたファイルの read・lseek・fstat・close は vfs_lock を取らない（ほかのコアが vfs_lock を
// 持ったまま SD カードを読み書きしている間も待たない。perf record で、ビルドの時間の約 2 割が vfs_lock の待ちだった）。
// vfs_lock を持ったまま取ってよいが、逆（vfs_fc_lock を持ったまま vfs_lock）はしない。
// キャッシュから開いたファイルの read などはロックを取らない（fc の中身は変わらず、開いている間は解放されない。
// 同じ fd を別のコアが同時に閉じるのはアプリの誤り）
struct klock vfs_fc_lock = KLOCK_INIT;
#define FC_LOCK()   klock_acquire(&vfs_fc_lock)
#define FC_UNLOCK() klock_release(&vfs_fc_lock)

// ファイルのキャッシュ: 読み出し専用で開いたファイル（FCACHE_MAX_FILE まで）は、最初に全体を読んで
// メモリに置き、以後は SD カードを読まずにメモリから返す（セルフホストのビルドで、同じヘッダーを何十回も読むため）。
// 鍵は絶対パス（FAT は大文字と小文字を区別しないので、小文字にそろえる）。書き込みで開いたとき・削除・名前の変更で、
// そのパスのものを捨てる（開いている間は古い内容のまま読める）。合計が FCACHE_BUDGET を超えたら、使われていない
// 古いものから捨てる。SD カードをほかの機械で書き換えたときのことは考えない（差し替えたら再起動する）
#define FCACHE_MAX_FILE     (4u << 20)
#define FCACHE_BUDGET       (64u << 20)
#define FCACHE_PATH         256

static time_t fat_time(WORD fdate, WORD ftime);
// picolibc の time.h は POSIX のときだけ宣言する（カーネルは -std=c23）
struct tm *localtime_r(const time_t *restrict t, struct tm *restrict tm);

struct fcache {
    struct fcache *next, *prev; // 新しく使った順のリスト（双方向。捨てたものは外す）
    struct fcache *hnext;   // ハッシュ表の同じ枠のリスト
    uint32_t hash;
    char key[FCACHE_PATH];
    char *data;
    size_t size;
    time_t mtime;           // ファイルの日時（stat が返す）
    uint32_t stamp;         // FAT の生の日時（fdate << 16 | ftime。vfs_file_stamp）
    int refs;               // 開いている fd の数
    bool dropped;           // リストから外した（refs が 0 になったら解放する）
};

static struct fcache *fc_head, *fc_tail;
// 鍵 → キャッシュ（ハッシュ表）。リストを先頭から比べていたときは、キャッシュに入っているファイルが
// 多いほど stat・open が遅く（2,559 ファイルで、picolibc のビルドの 2 回目以降が 63 → 104 秒）
#define FC_HASH 4096                // 2 の累乗
static struct fcache *fc_table[FC_HASH];
static size_t fc_total;
static uint64_t fc_hits, fc_misses;

struct vfs_file {
    bool used;
    int owner;              // 開いたもの（0 = アプリ・カーネル、1〜 = サービス。os/kernel/svc.c）
    struct fcache *fc;      // キャッシュから読むとき（FIL は使わない）
    bool null;              // /dev/null（書いたものを捨て、読むとすぐ終わり。FIL は使わない）
    uint32_t pos;
    uint32_t ino;           // stat の st_ino（ino_of。開いたパスから）
    // ファイルの終わりより先への lseek（POSIX では大きさを変えない）。FatFs の f_lseek は書き込みで開いたファイルを
    // その位置まで広げ（中身は SD カードに残っていたもの）、読み出しで開いたものは終わりで止める。そこで、位置だけを
    // past_pos に覚え、次の write で 0 を書き足してから書く（read は終わり）。2026-11-20、SQLite が空のファイルの
    // 24 バイト目を読みに lseek して、ファイルが 24 バイトのごみになり「file is not a database」になった
    bool past;
    uint32_t past_pos;
    // ほかの fd で書いたものが見えるように（2026-11-20、Lua の files.lua）: 書き込みで開いた fd を閉じた・fsync・
    // ftruncate したら、同じファイルを読み出し専用で開いているほかの fd に stale を立て、その fd の次の read・lseek・
    // fstat で開き直す（位置はそのまま）。キャッシュから開いた fd は開いたときの中身を持ち、FatFs で開いた fd も
    // 開いたときの大きさを持つので、そのままでは書いたものが見えなかった（Linux は見える）
    bool stale;
    bool append;            // O_APPEND（write のたびに終わりへ。POSIX と同じ）
    char key[FCACHE_PATH];  // 開いたパス（make_key の形。長すぎたら空）
    FIL fil;
};

static FATFS fatfs;
static bool mounted;
// FatFs のカレントディレクトリの写し（make_key が使う。f_getcwd は FatFs なので vfs_lock が要るため）。
// 書き換えるのは vfs_lock と vfs_fc_lock の両方を持つときだけなので、どちらかを持てば読める
static char cwd[FCACHE_PATH * 2] = "/";
static struct vfs_file files[VFS_MAX_FILES];
static bool fd_sock[VFS_MAX_FILES];     // ソケットが使っている番号（files[].used は偽のまま）
static struct {
    bool used;
    int owner;              // 開いたもの（0 = アプリ・カーネル、1〜 = サービス。os/kernel/svc.c）
    DIR dir;
} dirs[VFS_MAX_DIRS];

// FRESULT を errno に変換する
static int fresult_to_errno(FRESULT r)
{
    switch (r) {
    case FR_OK:                 return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:            return ENOENT;
    case FR_INVALID_NAME:       return EINVAL;
    case FR_DENIED:             return EACCES;
    case FR_EXIST:              return EEXIST;
    case FR_WRITE_PROTECTED:    return EROFS;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:      return ENODEV;
    case FR_TOO_MANY_OPEN_FILES: return EMFILE;
    case FR_NOT_ENOUGH_CORE:    return ENOMEM;
    case FR_INVALID_PARAMETER:  return EINVAL;
    case FR_LOCKED:             return EBUSY;
    default:                    return EIO;
    }
}

static int set_errno(FRESULT r)
{
    errno = fresult_to_errno(r);
    return -1;
}

static struct vfs_file *get_file(int fd)
{
    if (fd < FD_CONSOLE_COUNT || fd >= VFS_MAX_FILES || !files[fd].used) {
        errno = EBADF;
        return nullptr;
    }
    return &files[fd];
}

// path を絶対パスにし、"." と ".." を除き、小文字にそろえる。長すぎたら false
static bool make_key(const char *path, char *key)
{
    char buf[FCACHE_PATH * 2];
    size_t n = 0;
    if (path[0] != '/') {
        n = strlen(cwd);
        memcpy(buf, cwd, n);
    }
    int w = snprintf(buf + n, sizeof(buf) - n, "/%s", path);
    if (w < 0 || (size_t)w >= sizeof(buf) - n)
        return false;

    size_t k = 0;
    for (char *p = buf; *p;) {
        while (*p == '/')
            p++;
        char *e = p;
        while (*e && *e != '/')
            e++;
        size_t len = e - p;
        if (len == 0 || (len == 1 && p[0] == '.')) {
            // 何もしない
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (k > 0 && key[k - 1] != '/')
                k--;
            if (k > 0)
                k--;
        } else {
            if (k + 1 + len >= FCACHE_PATH)
                return false;
            key[k++] = '/';
            for (size_t i = 0; i < len; i++)
                key[k++] = (char)tolower((unsigned char)p[i]);
        }
        p = e;
    }
    if (k == 0)
        key[k++] = '/';
    key[k] = '\0';
    return true;
}

static uint32_t fc_hash(const char *key)
{
    uint32_t h = 2166136261u;
    for (; *key; key++)
        h = (h ^ (uint8_t)*key) * 16777619u;
    return h;
}

// stat の st_ino: FAT には i ノードの番号がないので、鍵（絶対パス、小文字）のハッシュにする（0 にはしない）。
// SQLite は (st_dev, st_ino) で同じファイルかを決める（前はすべて 0 で、どのファイルも同じに見えた。2026-11-20）。
// 名前を変えると番号も変わる（Linux と違う）
static uint32_t ino_of(const char *key)
{
    return fc_hash(key) | 1;
}

static void lru_remove(struct fcache *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        fc_head = c->next;
    if (c->next)
        c->next->prev = c->prev;
    else
        fc_tail = c->prev;
    c->next = c->prev = nullptr;
}

static void lru_push_front(struct fcache *c)
{
    c->prev = nullptr;
    c->next = fc_head;
    if (fc_head)
        fc_head->prev = c;
    fc_head = c;
    if (!fc_tail)
        fc_tail = c;
}

static void fc_unlink(struct fcache *c)
{
    lru_remove(c);
    for (struct fcache **pp = &fc_table[c->hash & (FC_HASH - 1)]; *pp; pp = &(*pp)->hnext)
        if (*pp == c) {
            *pp = c->hnext;
            break;
        }
    c->dropped = true;
    fc_total -= c->size;
    if (c->refs == 0) {
        free(c->data);
        free(c);
    }
}

static void fc_release(struct fcache *c)
{
    if (--c->refs == 0 && c->dropped) {
        free(c->data);
        free(c);
    }
}

static struct fcache *fc_find(const char *key)
{
    uint32_t h = fc_hash(key);
    for (struct fcache *c = fc_table[h & (FC_HASH - 1)]; c; c = c->hnext)
        if (c->hash == h && strcmp(c->key, key) == 0) {
            lru_remove(c);      // 先頭に移す
            lru_push_front(c);
            return c;
        }
    return nullptr;
}

static void fc_invalidate(const char *path)
{
    char key[FCACHE_PATH];
    // コンパイルのキャッシュが覚えている、ファイルの中身のハッシュも忘れさせる（os/slimcc/cache.c。2026-11-20）
    cache_file_changed(make_key(path, key) ? key : nullptr);
    FC_LOCK();
    if (!make_key(path, key)) {
        // 鍵を作れない（長すぎる）パスはキャッシュにないが、念のため全部捨てる
        while (fc_head)
            fc_unlink(fc_head);
    } else {
        struct fcache *c = fc_find(key);
        if (c)
            fc_unlink(c);
    }
    FC_UNLOCK();
}

// size バイトを足せるよう、使われていない古いものから捨てる
static void fc_make_room(size_t size)
{
    while (fc_total + size > FCACHE_BUDGET) {
        struct fcache *victim = fc_tail;    // 末尾（いちばん前に使ったもの）から、開かれていないもの
        while (victim && victim->refs != 0)
            victim = victim->prev;
        if (!victim)
            return;
        fc_unlink(victim);
    }
}

// 開いた fil の中身を全部読み、キャッシュに入れる。できなければ nullptr（fil はそのまま）
// （vfs_lock の中で呼ぶ。読む間は vfs_fc_lock を持たず、入れるときに取る）
static struct fcache *fc_load(const char *key, FIL *fil)
{
    FSIZE_t size = f_size(fil);
    if (size > FCACHE_MAX_FILE)
        return nullptr;
    FC_LOCK();
    fc_make_room(size);
    bool room = fc_total + size <= FCACHE_BUDGET;
    FC_UNLOCK();
    if (!room)
        return nullptr;
    // キャッシュはアプリが終わっても残すので、アプリの実行中でもカーネル用のヒープから取る
    malloc_kernel_begin();
    struct fcache *c = calloc(1, sizeof(*c));
    char *data = malloc(size ? size : 1);
    malloc_kernel_end();
    if (!c || !data) {
        free(c);
        free(data);
        return nullptr;
    }
    UINT n;
    if (f_read(fil, data, size, &n) != FR_OK || n != size) {
        free(c);
        free(data);
        f_lseek(fil, 0);
        return nullptr;
    }
    strcpy(c->key, key);
    c->data = data;
    c->size = size;
    FILINFO info;
    bool have_info = f_stat(key, &info) == FR_OK;
    c->mtime = have_info ? fat_time(info.fdate, info.ftime) : 0;
    c->stamp = have_info ? (uint32_t)info.fdate << 16 | info.ftime : 0;
    c->hash = fc_hash(key);
    FC_LOCK();
    c->hnext = fc_table[c->hash & (FC_HASH - 1)];
    fc_table[c->hash & (FC_HASH - 1)] = c;
    lru_push_front(c);
    fc_total += size;
    c->refs++;              // 開く fd の分（呼び出し元が使う。vfs_fc_lock を外す前に数える）
    FC_UNLOCK();
    return c;
}

static bool vfs_init_unlocked(void)
{
    FRESULT r = f_mount(&fatfs, "", 1);
    mounted = (r == FR_OK);
    return mounted;
}

// ないとわかっているパス（負のキャッシュ）。コンパイラはヘッダーを -I のディレクトリの順に探すので、
// ないファイルを何度も開こうとする。FatFs はそのたびにディレクトリを端から読む（picolibc のビルドで
// SD カードから 760MB 読んでいた）。開けなかった・stat できなかったパスを覚えておき、そのパスにファイルや
// ディレクトリを作ったら（O_CREAT・mkdir・rename の行き先）忘れる。いっぱいになったら全部忘れる
#define NEG_SLOTS   8192                // 2 の累乗
static struct {
    uint32_t hash;                      // 0 は空き
    char key[FCACHE_PATH];
} neg[NEG_SLOTS];
static unsigned neg_count;
static uint64_t neg_hits;

static uint32_t neg_hash(const char *key)
{
    uint32_t h = 2166136261u;
    for (; *key; key++)
        h = (h ^ (uint8_t)*key) * 16777619u;
    return h ? h : 1;
}

static int neg_find(const char *key, uint32_t h)
{
    for (uint32_t i = h & (NEG_SLOTS - 1);; i = (i + 1) & (NEG_SLOTS - 1)) {
        if (!neg[i].hash)
            return -1;
        if (neg[i].hash == h && !strcmp(neg[i].key, key))
            return (int)i;
    }
}

static bool neg_has(const char *key)
{
    if (!neg_count || neg_find(key, neg_hash(key)) < 0)
        return false;
    neg_hits++;
    return true;
}

static void neg_add_locked(const char *key);

static void neg_add(const char *key)
{
    FC_LOCK();
    neg_add_locked(key);
    FC_UNLOCK();
}

static void neg_add_locked(const char *key)
{
    uint32_t h = neg_hash(key);
    if (neg_find(key, h) >= 0)
        return;
    if (neg_count >= NEG_SLOTS / 2) {   // 半分を超えたら全部忘れる（探す手間が増えないように）
        memset(neg, 0, sizeof(neg));
        neg_count = 0;
    }
    uint32_t i = h & (NEG_SLOTS - 1);
    while (neg[i].hash)
        i = (i + 1) & (NEG_SLOTS - 1);
    neg[i].hash = h;
    strcpy(neg[i].key, key);
    neg_count++;
}

// path にファイルかディレクトリができる（覚えていたら、全部忘れる。まれなので簡単にする）
static void neg_forget(const char *path)
{
    char key[FCACHE_PATH];
    FC_LOCK();
    if (neg_count && (!make_key(path, key) || neg_find(key, neg_hash(key)) >= 0)) {
        memset(neg, 0, sizeof(neg));
        neg_count = 0;
    }
    FC_UNLOCK();
}

// ファイルの表の空きを取り、使用中にする（vfs_fc_lock の中で）。いっぱいなら -1
static int slot_alloc_locked(void)
{
    for (int fd = FD_CONSOLE_COUNT; fd < VFS_MAX_FILES; fd++)
        if (!files[fd].used && !fd_sock[fd]) {
            files[fd].used = true;
            files[fd].owner = svc_owner();
            files[fd].fc = nullptr;
            files[fd].null = false;
            files[fd].pos = 0;
            files[fd].past = false;
            files[fd].stale = false;
            files[fd].append = false;
            files[fd].key[0] = '\0';
            return fd;
        }
    return -1;
}

int vfs_fd_take_socket(void)
{
    FC_LOCK();
    int fd = slot_alloc_locked();
    if (fd >= 0) {
        files[fd].used = false;
        fd_sock[fd] = true;
    }
    FC_UNLOCK();
    return fd;
}

void vfs_fd_put_socket(int fd)
{
    FC_LOCK();
    if (fd >= FD_CONSOLE_COUNT && fd < VFS_MAX_FILES)
        fd_sock[fd] = false;
    FC_UNLOCK();
}

static void slot_free(int fd)
{
    FC_LOCK();
    files[fd].used = false;
    files[fd].fc = nullptr;
    FC_UNLOCK();
}

// キャッシュだけで答えられる open（vfs_fc_lock の中で）。fd、-1（errno）、-2（キャッシュにない）
static int open_cached_locked(const char *key)
{
    if (neg_has(key)) {
        errno = ENOENT;
        return -1;
    }
    struct fcache *c = fc_find(key);
    if (!c)
        return -2;
    int fd = slot_alloc_locked();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    fc_hits++;
    c->refs++;
    files[fd].fc = c;
    files[fd].ino = ino_of(key);    // open の速い道（vfs_lock なし）もここを通る
    strcpy(files[fd].key, key);
    return fd;
}

// 読み出し専用で、キャッシュに使えるか（key に鍵）
static bool open_cacheable(const char *path, int flags, char *key)
{
    return (flags & O_ACCMODE) == O_RDONLY && !(flags & (O_CREAT | O_TRUNC)) && make_key(path, key);
}

// vfs_lock の中で（キャッシュに当たらなかったとき。open がキャッシュを先に調べる）
static int open_file_unlocked(const char *path, int flags);

static int open_unlocked(const char *path, int flags)
{
    int fd = open_file_unlocked(path, flags);
    if (fd >= 0) {
        char key[FCACHE_PATH];
        bool ok = make_key(path, key);
        files[fd].ino = ok ? ino_of(key) : 0;
        strcpy(files[fd].key, ok ? key : "");
        files[fd].append = (flags & O_APPEND) != 0;
    }
    return fd;
}

// 読み出し専用の fd か（キャッシュからか、FatFs の FA_READ だけ）
static bool read_only_fd(const struct vfs_file *f)
{
    return f->fc || !(f->fil.flag & FA_WRITE);
}

// 同じファイルを読み出し専用で開いているほかの fd があるか（vfs_lock の中で）
static bool others_reading(int fd)
{
    const struct vfs_file *w = &files[fd];
    if (!w->key[0])
        return false;
    for (int i = FD_CONSOLE_COUNT; i < VFS_MAX_FILES; i++) {
        const struct vfs_file *f = &files[i];
        if (i != fd && f->used && !f->null && f->ino == w->ino && read_only_fd(f) && !strcmp(f->key, w->key))
            return true;
    }
    return false;
}

// fd で書いたものを、同じファイルを読み出し専用で開いているほかの fd に見せる（vfs_lock の中で）
static void mark_others_stale(int fd)
{
    const struct vfs_file *w = &files[fd];
    if (!w->key[0])
        return;
    // 書いている間に、ほかが読み出し専用で開いてキャッシュに入れたかもしれない（古い中身）。次の open が読み直すように
    fc_invalidate(w->key);
    FC_LOCK();
    for (int i = FD_CONSOLE_COUNT; i < VFS_MAX_FILES; i++) {
        struct vfs_file *f = &files[i];
        if (i != fd && f->used && !f->null && f->ino == w->ino && read_only_fd(f) && !strcmp(f->key, w->key))
            f->stale = true;
    }
    FC_UNLOCK();
}

// stale の fd を開き直す（vfs_lock の中で）。開き直せなければ前のまま
static void refresh_unlocked(int fd)
{
    struct vfs_file *f = &files[fd];
    if (!f->used || !f->stale)
        return;
    f->stale = false;
    FSIZE_t pos = f->fc ? f->pos : f->past ? f->past_pos : f_tell(&f->fil);
    FIL fil;
    if (f_open(&fil, f->key, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return;
    struct fcache *old = f->fc;
    if (!old)
        f_close(&f->fil);
    // 書き込みで開いたときにキャッシュは捨ててあるので、新しい中身で入れ直す（入らなければ FatFs のまま）
    struct fcache *c = fc_load(f->key, &fil);
    FC_LOCK();
    if (old)
        fc_release(old);
    f->past = false;
    if (c) {
        f->fc = c;
        f->pos = (uint32_t)pos;
    } else {
        f->fc = nullptr;
        f->fil = fil;
    }
    FC_UNLOCK();
    if (c) {
        f_close(&fil);
    } else if (pos > f_size(&f->fil)) {
        f->past = true;
        f->past_pos = (uint32_t)pos;
    } else {
        f_lseek(&f->fil, pos);
    }
}

static void refresh(int fd)
{
    if (fd < FD_CONSOLE_COUNT || fd >= VFS_MAX_FILES || !files[fd].stale)
        return;
    klock_acquire(&vfs_lock);
    refresh_unlocked(fd);
    klock_release(&vfs_lock);
}

static int open_file_unlocked(const char *path, int flags)
{
    BYTE mode;
    switch (flags & O_ACCMODE) {
    case O_RDONLY:  mode = FA_READ; break;
    case O_WRONLY:  mode = FA_WRITE; break;
    default:        mode = FA_READ | FA_WRITE; break;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL))
        mode |= FA_CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC))
        mode |= FA_CREATE_ALWAYS;
    else if (flags & O_CREAT)
        mode |= FA_OPEN_ALWAYS;
    else
        mode |= FA_OPEN_EXISTING;

    // 読み出し専用ならキャッシュから。書き込むなら、そのパスのキャッシュを捨てる
    char key[FCACHE_PATH];
    bool cacheable = open_cacheable(path, flags, key);
    if (!cacheable && (flags & O_ACCMODE) != O_RDONLY)
        fc_invalidate(path);
    if (flags & O_CREAT)
        neg_forget(path);
    if (cacheable) {
        // vfs_lock を待つ間に、ほかのコアがキャッシュに入れたかもしれない（同じものを 2 つ入れないように）
        FC_LOCK();
        int r = open_cached_locked(key);
        FC_UNLOCK();
        if (r != -2)
            return r;
    }

    FC_LOCK();
    int fd = slot_alloc_locked();
    FC_UNLOCK();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    FIL *fil = &files[fd].fil;
    FRESULT r = f_open(fil, path, mode);
    if (r != FR_OK) {
        // FatFs はフォルダを開こうとしても FR_NO_FILE を返す。フォルダなら「ないパス」として覚えない（覚えると、
        // 続く stat がフォルダを「ない」と答えた。2026-11-19、run がフォルダのアプリを開けなかった）。errno は EISDIR
        FILINFO fi;
        if (r == FR_NO_FILE && f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR)) {
            slot_free(fd);
            errno = EISDIR;
            return -1;
        }
        if (cacheable && (r == FR_NO_FILE || r == FR_NO_PATH))
            neg_add(key);
        slot_free(fd);
        return set_errno(r);
    }
    if (cacheable) {
        FC_LOCK();
        fc_misses++;
        FC_UNLOCK();
        struct fcache *c = fc_load(key, fil);   // 入れたら、この fd の分を数えてある
        if (c) {
            f_close(fil);
            FC_LOCK();
            files[fd].fc = c;
            files[fd].pos = 0;
            FC_UNLOCK();
            return fd;
        }
    }
    if ((flags & O_TRUNC) && !(flags & O_CREAT) && (mode & FA_WRITE))
        r = f_truncate(fil);
    if (r == FR_OK && (flags & O_APPEND))
        r = f_lseek(fil, f_size(fil));
    if (r != FR_OK) {
        f_close(fil);
        slot_free(fd);
        return set_errno(r);
    }
    return fd;
}

// キャッシュから開いた fd を閉じる（vfs_fc_lock の中で）。キャッシュのものでなければ false
static bool close_cached_locked(int fd)
{
    struct vfs_file *f = &files[fd];
    if (f->used && f->null) {   // /dev/null: 枠を返すだけ
        f->null = false;
        f->used = false;
        return true;
    }
    if (!f->used || !f->fc)
        return false;
    fc_release(f->fc);
    f->fc = nullptr;
    f->used = false;
    return true;
}

// vfs_lock の中で（FatFs で開いたもの）
static int close_unlocked(int fd)
{
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;
    FC_LOCK();
    bool done = close_cached_locked(fd);
    FC_UNLOCK();
    if (done)
        return 0;
    bool wrote = (f->fil.flag & FA_WRITE) != 0;
    FRESULT r = f_close(&f->fil);
    if (wrote)
        mark_others_stale(fd);
    slot_free(fd);
    return r == FR_OK ? 0 : set_errno(r);
}

// キャッシュから開いた fd なら、そのキャッシュ（ロックを取らずに見る。上の vfs_fc_lock の説明）
static struct fcache *cached_file(int fd)
{
    if (fd < FD_CONSOLE_COUNT || fd >= VFS_MAX_FILES || !files[fd].used)
        return nullptr;
    return files[fd].fc;
}

static ssize_t read_unlocked(int fd, void *buf, size_t count)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT) {
        // コンソールは 1 文字ずつ返す（シェルのリダイレクトの間はファイルから。os/libc/syscalls.c）
        return console_read(fd, buf, count);
    }
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;
    if (f->fc) {
        size_t left = f->pos < f->fc->size ? f->fc->size - f->pos : 0;
        size_t n = count < left ? count : left;
        memcpy(buf, f->fc->data + f->pos, n);
        f->pos += n;
        return n;
    }
    if (f->past) {
        if (f->past_pos >= f_size(&f->fil))
            return 0;   // 終わりより先: 読むものはない
        FRESULT r = f_lseek(&f->fil, f->past_pos);  // ほかの fd で書き足されていた
        if (r != FR_OK)
            return set_errno(r);
        f->past = false;
    }
    UINT n;
    FRESULT r = f_read(&f->fil, buf, count, &n);
    return r == FR_OK ? (ssize_t)n : set_errno(r);
}

// 今の大きさから end まで 0 を書く（位置は end になる）。FR_DENIED はいっぱい
static FRESULT zero_fill(FIL *fil, FSIZE_t end)
{
    static const char zeros[512];
    FSIZE_t size = f_size(fil);
    FRESULT r = f_lseek(fil, size);
    while (r == FR_OK && size < end) {
        UINT n = (UINT)(end - size < sizeof zeros ? end - size : sizeof zeros), w;
        r = f_write(fil, zeros, n, &w);
        if (r == FR_OK && w < n)
            r = FR_DENIED;
        size += w;
    }
    return r;
}

static bool others_reading(int fd);
static void mark_others_stale(int fd);

static ssize_t write_unlocked(int fd, const void *buf, size_t count)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT) {
        return console_write(fd, buf, count);  // stdout はリダイレクトの間はファイルへ（os/libc/syscalls.c）
    }
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;
    if (f->fc) {
        errno = EBADF;  // 読み出し専用で開いた
        return -1;
    }
    if (f->append) {
        f->past = false;
        FRESULT r = f_lseek(&f->fil, f_size(&f->fil));
        if (r != FR_OK)
            return set_errno(r);
    }
    if (f->past) {
        FRESULT r = f->past_pos > f_size(&f->fil) ? zero_fill(&f->fil, f->past_pos) : f_lseek(&f->fil, f->past_pos);
        if (r == FR_DENIED) {
            errno = ENOSPC;
            return -1;
        }
        if (r != FR_OK)
            return set_errno(r);
        f->past = false;
    }
    UINT n;
    FRESULT r = f_write(&f->fil, buf, count, &n);
    if (r != FR_OK)
        return set_errno(r);
    if (n < count && n == 0) {
        errno = ENOSPC;
        return -1;
    }
    // 同じファイルをほかの fd で読んでいれば、書いたものをすぐ見せる（Linux と同じ）。FatFs は書いたものを fd の
    // バッファと、閉じるまで書き換えない大きさに持つので、f_sync してから読む側を開き直させる。読む fd がなければ何もしない
    if (others_reading(fd) && f_sync(&f->fil) == FR_OK)
        mark_others_stale(fd);
    return n;
}

static off_t lseek_unlocked(int fd, off_t offset, int whence)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT) {
        errno = ESPIPE;
        return -1;
    }
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;

    off_t base;
    switch (whence) {
    case SEEK_SET:  base = 0; break;
    case SEEK_CUR:  base = f->fc ? f->pos : f->past ? f->past_pos : f_tell(&f->fil); break;
    case SEEK_END:  base = f->fc ? f->fc->size : f_size(&f->fil); break;
    default:        errno = EINVAL; return -1;
    }
    off_t pos = base + offset;
    if (pos < 0 || pos > 0xFFFFFFFF) {
        errno = EINVAL;
        return -1;
    }
    if (f->fc) {
        f->pos = pos;
        return pos;
    }
    if ((FSIZE_t)pos > f_size(&f->fil)) {
        f->past = true;     // 大きさは変えない（write のときに 0 を書き足す）
        f->past_pos = (uint32_t)pos;
        return pos;
    }
    f->past = false;
    FRESULT r = f_lseek(&f->fil, pos);
    return r == FR_OK ? (off_t)f_tell(&f->fil) : set_errno(r);
}

// FatFs の日時（地方時。FILINFO の fdate・ftime）を UNIX 時間に（タイムゾーンは TZ。os/kernel/clock.c）
static time_t fat_time(WORD fdate, WORD ftime)
{
    struct tm tm = {
        .tm_year = (fdate >> 9) + 80, .tm_mon = ((fdate >> 5) & 15) - 1, .tm_mday = fdate & 31,
        .tm_hour = ftime >> 11, .tm_min = (ftime >> 5) & 63, .tm_sec = (ftime & 31) * 2, .tm_isdst = 0,
    };
    return fdate ? mktime(&tm) : 0;
}

// FatFs が書くファイルの日時（FF_FS_NORTC 0）。日時が設定されるまでは 2026-01-01 00:00:00 の固定
// （今までの FF_FS_NORTC 1 と同じ。QEMU の検証の結果が変わらないように）
DWORD get_fattime(void)
{
    if (!clock_approx())
        return (DWORD)(2026 - 1980) << 25 | 1 << 21 | 1 << 16;
    time_t t = (time_t)(clock_realtime_us() / 1000000);
    struct tm tm;
    localtime_r(&t, &tm);
    return (DWORD)(tm.tm_year - 80) << 25 | (DWORD)(tm.tm_mon + 1) << 21 | (DWORD)tm.tm_mday << 16 |
           (DWORD)tm.tm_hour << 11 | (DWORD)tm.tm_min << 5 | (DWORD)(tm.tm_sec / 2);
}

uint32_t vfs_now_stamp(void)
{
    return get_fattime();
}

static void fill_stat(struct stat *st, FSIZE_t size, bool is_dir, time_t mtime, uint32_t ino)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = ino;
    st->st_mtime = st->st_ctime = st->st_atime = mtime;
    st->st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = size;
    st->st_blksize = VFS_BLKSIZE; // picolibc の FILE のバッファの大きさ（fstat-bufsiz）
    st->st_blocks = (size + 511) / 512;
}

static int fstat_unlocked(int fd, struct stat *st)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR | 0666;
        return 0;
    }
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;
    fill_stat(st, f->fc ? f->fc->size : f_size(&f->fil), false, f->fc ? f->fc->mtime : 0, f->ino);
    return 0;
}

// キャッシュだけで答えられる stat（vfs_fc_lock の中で）。0、-1（errno）、-2（わからない）
static int stat_cached_locked(const char *key, struct stat *st)
{
    struct fcache *c = fc_find(key);
    if (c) {
        fill_stat(st, c->size, false, c->mtime, ino_of(key));
        return 0;
    }
    if (neg_has(key)) {
        errno = ENOENT;
        return -1;
    }
    return -2;
}

static int stat_unlocked(const char *restrict path, struct stat *restrict st)
{
    // ルートディレクトリは f_stat で調べられない
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        fill_stat(st, 0, true, 0, ino_of("/"));
        return 0;
    }
    char key[FCACHE_PATH];
    bool have_key = make_key(path, key);
    // キャッシュ（stat が vfs_lock を取る前にも調べている。待つ間にほかのコアが入れたかもしれない）
    if (have_key) {
        FC_LOCK();
        int r = stat_cached_locked(key, st);
        FC_UNLOCK();
        if (r != -2)
            return r;
    }
    FILINFO info;
    FRESULT r = f_stat(path, &info);
    if (r != FR_OK) {
        if (have_key && (r == FR_NO_FILE || r == FR_NO_PATH))
            neg_add(key);
        return set_errno(r);
    }
    fill_stat(st, info.fsize, (info.fattrib & AM_DIR) != 0, fat_time(info.fdate, info.ftime),
              have_key ? ino_of(key) : 0);
    return 0;
}

static int unlink_unlocked(const char *path)
{
    fc_invalidate(path);
    FRESULT r = f_unlink(path);
    return r == FR_OK ? 0 : set_errno(r);
}

int rmdir(const char *path)
{
    return unlink(path);
}

// POSIX と同じく、行き先がファイルならそれを置き換える（FatFs の f_rename は行き先があると FR_EXIST）。
// 消すのと名前を変えるのは VFS のロックの中で続けて行うので、ほかのコアからは置き換わったように見える。
// 行き先がディレクトリなら、置き換えずに失敗する
static int rename_unlocked(const char *from, const char *to)
{
    fc_invalidate(from);
    fc_invalidate(to);
    neg_forget(to);
    FRESULT r = f_rename(from, to);
    if (r == FR_EXIST) {
        FILINFO fi;
        if (f_stat(to, &fi) == FR_OK && !(fi.fattrib & AM_DIR) && f_stat(from, &fi) == FR_OK &&
            !(fi.fattrib & AM_DIR) && f_unlink(to) == FR_OK)
            r = f_rename(from, to);
    }
    return r == FR_OK ? 0 : set_errno(r);
}

static int mkdir_unlocked(const char *path, mode_t mode)
{
    (void)mode;
    neg_forget(path);
    FRESULT r = f_mkdir(path);
    return r == FR_OK ? 0 : set_errno(r);
}

static int chdir_unlocked(const char *path)
{
    FRESULT r = f_chdir(path);
    if (r != FR_OK)
        return set_errno(r);
    char buf[sizeof(cwd)];
    if (f_getcwd(buf, sizeof(buf)) == FR_OK) {
        FC_LOCK();
        strcpy(cwd, buf);
        FC_UNLOCK();
    }
    return 0;
}

// 開いているファイルの fd か（select・poll。os/libc/select.c）
bool vfs_fd_open(int fd)
{
    return fd >= FD_CONSOLE_COUNT && fd < VFS_MAX_FILES && files[fd].used;
}

// fcntl: ソケットの O_NONBLOCK（os/libc/socket.c）。ファイル・端末は形だけ（F_GETFL は O_RDWR。O_NONBLOCK は使わない）。
// レコードのロック（F_SETLK・F_SETLKW・F_GETLK。2026-11-20、SQLite）: POSIX のロックはプロセスごとで、同じプロセスの
// ロックどうしはぶつからない。Monocs はアプリが 1 つ（プロセス 1 つ）なので、いつも取れ、F_GETLK は F_UNLCK を返す
int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int arg = 0;
    struct flock *lk = nullptr;
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
        lk = va_arg(ap, struct flock *);
    else
        arg = va_arg(ap, int);
    va_end(ap);
    bool sock = socket_is_fd(fd), pip = pipe_is_fd(fd);
    if (!sock && !pip && !(fd >= 0 && fd < FD_CONSOLE_COUNT) && !vfs_fd_open(fd)) {
        errno = EBADF;
        return -1;
    }
    switch (cmd) {
    case F_GETFL:
        return sock ? socket_get_flags(fd) : pip ? pipe_get_flags(fd) : O_RDWR;
    case F_SETFL:
        return sock ? socket_set_flags(fd, arg) : pip ? pipe_set_flags(fd, arg) : 0;
    case F_GETFD:
    case F_SETFD:
        return 0;   // FD_CLOEXEC（exec がないので意味がない）
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        if (sock || pip || !lk) {
            errno = sock || pip ? EINVAL : EFAULT;
            return -1;
        }
        if (cmd == F_GETLK)
            lk->l_type = F_UNLCK;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int isatty(int fd)
{
    // stdin・stdout・stderr はシェルのリダイレクト・パイプ・ssh の exec の間はファイル（os/libc/syscalls.c）
    if ((fd == 0 && console_in_is_file()) || (fd == 1 && console_out_is_file()) || (fd == 2 && console_err_is_file())) {
        errno = ENOTTY;
        return 0;
    }
    if (fd >= 0 && fd < FD_CONSOLE_COUNT)
        return 1;
    errno = ENOTTY;
    return 0;
}

static int vfs_dir_open_unlocked(const char *path)
{
    for (int h = 0; h < VFS_MAX_DIRS; h++) {
        if (dirs[h].used)
            continue;
        FRESULT r = f_opendir(&dirs[h].dir, path);
        if (r != FR_OK)
            return set_errno(r);
        dirs[h].used = true;
        dirs[h].owner = svc_owner();
        return h;
    }
    errno = EMFILE;
    return -1;
}

static int vfs_dir_read_unlocked(int handle, char *name, size_t size, bool *is_dir, bool *hidden)
{
    if (handle < 0 || handle >= VFS_MAX_DIRS || !dirs[handle].used) {
        errno = EBADF;
        return -1;
    }
    FILINFO info;
    FRESULT r = f_readdir(&dirs[handle].dir, &info);
    if (r != FR_OK)
        return set_errno(r);
    if (info.fname[0] == '\0')
        return 0;
    strncpy(name, info.fname, size - 1);
    name[size - 1] = '\0';
    *is_dir = (info.fattrib & AM_DIR) != 0;
    *hidden = (info.fattrib & AM_HID) != 0;
    return 1;
}

static int vfs_dir_close_unlocked(int handle)
{
    if (handle < 0 || handle >= VFS_MAX_DIRS || !dirs[handle].used) {
        errno = EBADF;
        return -1;
    }
    dirs[handle].used = false;
    FRESULT r = f_closedir(&dirs[handle].dir);
    return r == FR_OK ? 0 : set_errno(r);
}

static void vfs_close_owner_unlocked(int owner)
{
    for (int fd = FD_CONSOLE_COUNT; fd < VFS_MAX_FILES; fd++) {
        FC_LOCK();
        bool mine = files[fd].used && files[fd].owner == owner;
        bool fatfs_file = mine && !close_cached_locked(fd);
        FC_UNLOCK();
        if (fatfs_file) {
            f_close(&files[fd].fil);
            slot_free(fd);
        }
    }
    for (int h = 0; h < VFS_MAX_DIRS; h++) {
        if (dirs[h].used && dirs[h].owner == owner) {
            dirs[h].used = false;
            f_closedir(&dirs[h].dir);
        }
    }
}

// ロックを取って呼ぶ（コア 1〜3 から呼ばれてもよいように）
void vfs_sync(bool force)
{
    unsigned dirty;
    uint64_t flushes, rewrites;
    diskio_writeback_info(&dirty, &flushes, &rewrites);
    if (!dirty)
        return;     // ロックを取らずに調べる（シェルの待ちの間に何度も呼ばれるため）
    klock_acquire(&vfs_lock);
    diskio_writeback(force);
    klock_release(&vfs_lock);
}

bool vfs_init(void)
{
    klock_acquire(&vfs_lock);
    bool r = vfs_init_unlocked();
    klock_release(&vfs_lock);
    return r;
}

// ---- /dev/null（Linux と同じく、ファイルとして開ける: 書いたものを捨て、読むとすぐ終わり）。SD カードには
// 何もなく、ファイルの表の枠だけを使う。シェルのリダイレクト（> /dev/null。os/libc/syscalls.c）は別の仕組み ----
static bool is_dev_null(const char *path)
{
    return !strcmp(path, "/dev/null");
}

static bool null_fd(int fd)
{
    return fd >= FD_CONSOLE_COUNT && fd < VFS_MAX_FILES && files[fd].used && files[fd].null;
}

static void null_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

int open(const char *path, int flags, ...)
{
    if (is_dev_null(path)) {
        FC_LOCK();
        int fd = slot_alloc_locked();
        if (fd >= 0)
            files[fd].null = true;
        FC_UNLOCK();
        if (fd < 0)
            errno = EMFILE;
        return fd;
    }
    // キャッシュに当たれば vfs_lock を取らない
    char key[FCACHE_PATH];
    FC_LOCK();
    int r = open_cacheable(path, flags, key) ? open_cached_locked(key) : -2;
    FC_UNLOCK();
    if (r != -2)
        return r;
    klock_acquire(&vfs_lock);
    r = open_unlocked(path, flags);
    klock_release(&vfs_lock);
    return r;
}

int close(int fd)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT)
        return 0;
    if (socket_is_fd(fd))
        return socket_close(fd);    // ソケット（os/libc/socket.c）
    if (pipe_is_fd(fd))
        return pipe_close(fd);      // pipe（os/libc/pipe.c）
    if (fd < 0 || fd >= VFS_MAX_FILES) {
        errno = EBADF;
        return -1;
    }
    FC_LOCK();
    bool done = close_cached_locked(fd);
    FC_UNLOCK();
    if (done)
        return 0;
    klock_acquire(&vfs_lock);
    int r = close_unlocked(fd);
    klock_release(&vfs_lock);
    return r;
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT)
        return read_unlocked(fd, buf, count); // キー入力を待つ間もロックを持たない
    if (socket_is_fd(fd))
        return socket_read(fd, buf, count);
    if (pipe_is_fd(fd))
        return pipe_read(fd, buf, count);
    if (null_fd(fd))
        return 0;
    refresh(fd);
    struct fcache *c = cached_file(fd);
    if (c) {
        // キャッシュから: ロックを取らない
        struct vfs_file *f = &files[fd];
        size_t left = f->pos < c->size ? c->size - f->pos : 0;
        size_t n = count < left ? count : left;
        memcpy(buf, c->data + f->pos, n);
        f->pos += n;
        return n;
    }
    klock_acquire(&vfs_lock);
    ssize_t r = read_unlocked(fd, buf, count);
    klock_release(&vfs_lock);
    return r;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT)
        return write_unlocked(fd, buf, count); // キー入力を待つ間もロックを持たない
    if (socket_is_fd(fd))
        return socket_write(fd, buf, count);
    if (pipe_is_fd(fd))
        return pipe_write(fd, buf, count);
    if (null_fd(fd))
        return (ssize_t)count;
    klock_acquire(&vfs_lock);
    ssize_t r = write_unlocked(fd, buf, count);
    klock_release(&vfs_lock);
    return r;
}

off_t lseek(int fd, off_t offset, int whence)
{
    if (null_fd(fd))
        return 0;
    if (socket_is_fd(fd) || pipe_is_fd(fd)) {
        errno = ESPIPE;
        return -1;
    }
    refresh(fd);
    if (cached_file(fd))
        return lseek_unlocked(fd, offset, whence);  // キャッシュから: ロックを取らない（FatFs を使わない）
    klock_acquire(&vfs_lock);
    off_t r = lseek_unlocked(fd, offset, whence);
    klock_release(&vfs_lock);
    return r;
}

int fstat(int fd, struct stat *st)
{
    if (socket_is_fd(fd)) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFSOCK | 0777;
        return 0;
    }
    if (pipe_is_fd(fd)) {
        pipe_fstat(fd, st);
        return 0;
    }
    if (null_fd(fd)) {
        null_stat(st);
        return 0;
    }
    refresh(fd);
    if (cached_file(fd))
        return fstat_unlocked(fd, st);  // キャッシュから: ロックを取らない
    klock_acquire(&vfs_lock);
    int r = fstat_unlocked(fd, st);
    klock_release(&vfs_lock);
    return r;
}

// ファイルの大きさと FAT の生の日時（fdate << 16 | ftime。地方時の 2 秒刻み。TZ で換算しないので、TZ を変えても
// 変わらない）。コンパイルのキャッシュが、中身を読まずに確かめるのに使う（os/slimcc/cache.c。2026-11-21）。
// ファイルのキャッシュに載っていれば SD カードを読まない。ふつうのファイルでなければ false
bool vfs_file_stamp(const char *path, uint64_t *size, uint32_t *stamp)
{
    char key[FCACHE_PATH];
    if (!make_key(path, key))
        return false;
    FC_LOCK();
    struct fcache *c = fc_find(key);
    bool hit = c && c->stamp;
    if (hit) {
        *size = c->size;
        *stamp = c->stamp;
    }
    bool missing = !c && neg_has(key);
    FC_UNLOCK();
    if (hit)
        return true;
    if (missing)
        return false;
    FILINFO info;
    klock_acquire(&vfs_lock);
    FRESULT r = f_stat(path, &info);
    klock_release(&vfs_lock);
    if (r != FR_OK || (info.fattrib & AM_DIR))
        return false;
    *size = info.fsize;
    *stamp = (uint32_t)info.fdate << 16 | info.ftime;
    return true;
}

int stat(const char *restrict path, struct stat *restrict st)
{
    if (is_dev_null(path)) {
        null_stat(st);
        return 0;
    }
    // キャッシュで答えられれば vfs_lock を取らない
    if (strcmp(path, "/") && strcmp(path, "")) {
        char key[FCACHE_PATH];
        FC_LOCK();
        int r = make_key(path, key) ? stat_cached_locked(key, st) : -2;
        FC_UNLOCK();
        if (r != -2)
            return r;
    }
    klock_acquire(&vfs_lock);
    int r = stat_unlocked(path, st);
    klock_release(&vfs_lock);
    return r;
}

int unlink(const char *path)
{
    klock_acquire(&vfs_lock);
    int r = unlink_unlocked(path);
    klock_release(&vfs_lock);
    return r;
}

int rename(const char *from, const char *to)
{
    klock_acquire(&vfs_lock);
    int r = rename_unlocked(from, to);
    klock_release(&vfs_lock);
    return r;
}

// ftruncate: 大きさを length にする（短くするのは f_truncate、長くするのは 0 を書き足す（POSIX と同じく、増えた
// ところは 0）。読み書きの位置は変えない）。2026-11-20、SQLite
static int ftruncate_unlocked(int fd, off_t length)
{
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;
    if (f->fc || !(f->fil.flag & FA_WRITE)) {
        errno = EINVAL;     // 書き込みで開いていない
        return -1;
    }
    if (length < 0 || length > 0xFFFFFFFF) {
        errno = EINVAL;
        return -1;
    }
    FSIZE_t pos = f->past ? f->past_pos : f_tell(&f->fil), size = f_size(&f->fil);
    FRESULT r = FR_OK;
    if ((FSIZE_t)length < size) {
        r = f_lseek(&f->fil, (FSIZE_t)length);
        if (r == FR_OK)
            r = f_truncate(&f->fil);
    } else if ((FSIZE_t)length > size) {
        r = zero_fill(&f->fil, (FSIZE_t)length);
    }
    // 読み書きの位置は元のまま（新しい大きさより先なら、覚えておく）
    if (pos > f_size(&f->fil)) {
        f->past = true;
        f->past_pos = (uint32_t)pos;
    } else {
        f->past = false;
        FRESULT r2 = f_lseek(&f->fil, pos);
        if (r == FR_OK)
            r = r2;
    }
    if (r == FR_OK)
        r = f_sync(&f->fil);    // 大きさをディレクトリに書き、ほかの fd に見せる
    if (r == FR_OK)
        mark_others_stale(fd);
    if (r == FR_DENIED) {
        errno = ENOSPC;
        return -1;
    }
    return r == FR_OK ? 0 : set_errno(r);
}

int ftruncate(int fd, off_t length)
{
    if (socket_is_fd(fd) || pipe_is_fd(fd) || null_fd(fd) || (fd >= 0 && fd < FD_CONSOLE_COUNT)) {
        errno = EINVAL;
        return -1;
    }
    klock_acquire(&vfs_lock);
    int r = ftruncate_unlocked(fd, length);
    klock_release(&vfs_lock);
    return r;
}

int truncate(const char *path, off_t length)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int r = ftruncate(fd, length);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

// fsync・fdatasync: ファイルを書き出し（f_sync）、SD カードの書き込みのまとめ（os/fs/diskio.c）も書き出す
// （Linux と同じく、戻ったら SD カードに書かれている）。ふだんの書き込みは 1 秒以内にまとめて書き出す
static int fsync_unlocked(int fd)
{
    if (fd >= 0 && fd < FD_CONSOLE_COUNT)
        return 0;
    struct vfs_file *f = get_file(fd);
    if (!f)
        return -1;
    FRESULT r = FR_OK;
    if (!f->fc && !f->null && (f->fil.flag & FA_WRITE))
        r = f_sync(&f->fil);
    if (r != FR_OK)
        return set_errno(r);
    if (!f->fc && !f->null && (f->fil.flag & FA_WRITE))
        mark_others_stale(fd);
    diskio_writeback(true);
    return 0;
}

int fsync(int fd)
{
    if (socket_is_fd(fd)) {
        errno = EINVAL;
        return -1;
    }
    if (null_fd(fd))
        return 0;
    klock_acquire(&vfs_lock);
    int r = fsync_unlocked(fd);
    klock_release(&vfs_lock);
    return r;
}

int fdatasync(int fd)
{
    return fsync(fd);
}

// chmod・fchmod・chown・fchown: FAT にはファイルの持ち主と許可の印がない。ファイルがあれば何もせずに成功する
// （Linux の vfat の quiet と同じ。SQLite は新しい控えのファイルを本体と同じ許可にしようとする）。
// 読み出し専用の属性は fatattr（monocs_fat_attr）で
int chmod(const char *path, mode_t mode)
{
    (void)mode;
    struct stat st;
    return stat(path, &st);
}

int fchmod(int fd, mode_t mode)
{
    (void)mode;
    struct stat st;
    return fstat(fd, &st);
}

int chown(const char *path, uid_t owner, gid_t group)
{
    (void)owner;
    (void)group;
    struct stat st;
    return stat(path, &st);
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)owner;
    (void)group;
    struct stat st;
    return fstat(fd, &st);
}

// ファイルの日時を変える（times が NULL なら今。FatFs の f_utime。FAT は書き換えた日時だけを持つ）
int utime(const char *path, const struct utimbuf *times)
{
    DWORD t = get_fattime();
    if (times) {
        struct tm tm;
        localtime_r(&times->modtime, &tm);
        // FAT が持てるのは 1980〜2107 年。範囲の外は端に寄せる（Linux の vfat と同じ。前は年の欄が回り込み、
        // 1970 年が 2098 年になった。2026-11-21、QuickJS の test_std）
        if (tm.tm_year < 80)
            tm = (struct tm){.tm_year = 80, .tm_mday = 1};
        else if (tm.tm_year > 207)
            tm = (struct tm){.tm_year = 207, .tm_mon = 11, .tm_mday = 31, .tm_hour = 23, .tm_min = 59, .tm_sec = 58};
        t = (DWORD)(tm.tm_year - 80) << 25 | (DWORD)(tm.tm_mon + 1) << 21 | (DWORD)tm.tm_mday << 16 |
            (DWORD)tm.tm_hour << 11 | (DWORD)tm.tm_min << 5 | (DWORD)(tm.tm_sec / 2);
    }
    FILINFO fno = {.fdate = (WORD)(t >> 16), .ftime = (WORD)t};
    klock_acquire(&vfs_lock);
    fc_invalidate(path);    // ファイルのキャッシュは日時も持つ
    FRESULT r = f_utime(path, &fno);
    klock_release(&vfs_lock);
    return r == FR_OK ? 0 : set_errno(r);
}

// FAT の属性（MONOCS_FAT_*: 読み出し専用 1、隠し 2、システム 4、アーカイブ 32。FatFs の AM_* と同じ値）を
// set で立て、clear で消す（両方 0 なら読むだけ）。今の属性を返す。だめなら -1（errno）
int monocs_fat_attr(const char *path, int set, int clear)
{
    const int mask = AM_RDO | AM_HID | AM_SYS | AM_ARC;
    klock_acquire(&vfs_lock);
    FILINFO fno;
    FRESULT r = f_stat(path, &fno);
    if (r == FR_OK && ((set | clear) & mask)) {
        BYTE attr = (BYTE)((fno.fattrib | (set & mask)) & ~(clear & mask));
        r = f_chmod(path, attr, (BYTE)mask);
        if (r == FR_OK)
            r = f_stat(path, &fno);
    }
    klock_release(&vfs_lock);
    return r == FR_OK ? fno.fattrib & (mask | AM_DIR) : set_errno(r);
}

// 大きさと空き（SD カードの FAT の区画。どのパスでも同じ）
int statvfs(const char *restrict path, struct statvfs *restrict buf)
{
    (void)path;
    klock_acquire(&vfs_lock);
    DWORD nfree;
    FATFS *fs;
    FRESULT r = f_getfree("", &nfree, &fs);
    klock_release(&vfs_lock);
    if (r != FR_OK)
        return set_errno(r);
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = buf->f_frsize = (unsigned long)fs->csize * FF_MAX_SS;
    buf->f_blocks = fs->n_fatent - 2;
    buf->f_bfree = buf->f_bavail = nfree;
    buf->f_namemax = FF_MAX_LFN;
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    klock_acquire(&vfs_lock);
    int r = mkdir_unlocked(path, mode);
    klock_release(&vfs_lock);
    return r;
}

int chdir(const char *path)
{
    klock_acquire(&vfs_lock);
    int r = chdir_unlocked(path);
    klock_release(&vfs_lock);
    return r;
}

char *getcwd(char *buf, size_t size)
{
    // 写しから返す（vfs_lock を取らない。realpath がヘッダーを開くたびに呼ぶ）
    FC_LOCK();
    size_t n = strlen(cwd);
    bool ok = buf && n < size;
    if (ok)
        memcpy(buf, cwd, n + 1);
    FC_UNLOCK();
    if (!ok) {
        errno = buf ? ERANGE : EINVAL;
        return nullptr;
    }
    return buf;
}

int vfs_dir_open(const char *path)
{
    klock_acquire(&vfs_lock);
    int r = vfs_dir_open_unlocked(path);
    klock_release(&vfs_lock);
    return r;
}

int vfs_dir_read(int handle, char *name, size_t size, bool *is_dir, bool *hidden)
{
    klock_acquire(&vfs_lock);
    int r = vfs_dir_read_unlocked(handle, name, size, is_dir, hidden);
    klock_release(&vfs_lock);
    return r;
}

int vfs_dir_close(int handle)
{
    klock_acquire(&vfs_lock);
    int r = vfs_dir_close_unlocked(handle);
    klock_release(&vfs_lock);
    return r;
}

// fd を、アプリ・サービスの終わりに閉じないものにする（シェルのリダイレクトのファイル。os/libc/syscalls.c）
void vfs_keep_open(int fd)
{
    FC_LOCK();
    struct vfs_file *f = get_file(fd);
    if (f)
        f->owner = VFS_OWNER_KERNEL;
    FC_UNLOCK();
}

void vfs_close_owner(int owner)
{
    klock_acquire(&vfs_lock);
    vfs_close_owner_unlocked(owner);
    klock_release(&vfs_lock);
}

// SD カードを直接読み書きする（サービスの HTTP サーバーの区画の取り出し・書き戻し）。FatFs と同じロックの中で
static int vfs_raw(bool write, uint32_t sector, uint32_t count, void *buf)
{
    klock_acquire(&vfs_lock);
    bool ok = write ? sd_write(sector, count, buf) : sd_read(sector, count, buf);
    klock_release(&vfs_lock);
    return ok ? 0 : -1;
}

int vfs_raw_read(uint32_t sector, uint32_t count, void *buf)
{
    return vfs_raw(false, sector, count, buf);
}

int vfs_raw_write(uint32_t sector, uint32_t count, const void *buf)
{
    return vfs_raw(true, sector, count, (void *)buf);
}

// SD カードを独り占めして fn を呼ぶ（まだ書いていない分を書き出してから。シェルの sdtest）
int vfs_exclusive(int (*fn)(void *), void *arg)
{
    klock_acquire(&vfs_lock);
    diskio_writeback(true);
    int r = fn(arg);
    klock_release(&vfs_lock);
    return r;
}

void vfs_discard(void)
{
    klock_acquire(&vfs_lock);
    diskio_discard();
    klock_release(&vfs_lock);
}

void vfs_cache_info(struct vfs_cache_info *info)
{
    FC_LOCK();
    *info = (struct vfs_cache_info){.bytes = fc_total, .hits = fc_hits, .misses = fc_misses,
                                    .missing = neg_count, .missing_hits = neg_hits};
    for (struct fcache *c = fc_head; c; c = c->next)
        info->files++;
    FC_UNLOCK();
}
