// slimcc-a64（libslimcc）で C のソースをメモリ上でコンパイルし、その場で実行する
//
// コンパイル（cc/libslimcc.c）→ メモリ上のリンク（cc/memlink.c）→ main の呼び出し。
// 外部のシンボルは OS API の関数表（os/api/api.def）から探す。
// アプリの終わり方（return、exit、例外、Ctrl-C）の扱いは os/kernel/app.c。

#define _DEFAULT_SOURCE     // open_memstream・fmemopen（実行ファイルの読み書き）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../libc/pthread_os.h"
#include "../kernel/klog.h"
#include "../libc/heap.h"
#include "../libc/termios_os.h"
#include "../kernel/pmu.h"
#include "../net/udp_api.h"
#include "libslimcc.h"
#include "run.h"
#include "makefile.h"
#include "cache.h"
#include <ctype.h>
void __clear_cache(void *begin, void *end);     // 命令キャッシュとデータキャッシュの整合（cc/rt/clear_cache.c）
#include "../drivers/led.h"
#include "../fs/vfs.h"
#include "../kernel/app.h"
#include "../kernel/sysreg.h"
#include "../kernel/smp.h"
#include "../api/api.h"
#include "../net/tcp_api.h"
#include "../drivers/uart.h"
#include "../libc/posix.h"
#include "../libc/pipe_os.h"
#include "../libc/socket_os.h"
#include "../curses/curses_os.h"
#include "../kernel/fault.h"
#include "../kernel/ksym.h"
#include "../kernel/klog_file.h"
#include "../drivers/sd.h"
#include "../kernel/exception.h"
#include "../drivers/timer.h"

#define CODE_ALIGN 4096

// memlink は TLS の変数を "tls:名前" で探し、TP（TPIDR_EL0）からのオフセットを受け取る
// （オフセットはどのコアでも同じ。cc/libslimcc.h の SlimccResolver）
static void *resolve(const char *name, void *ctx)
{
    (void)ctx;
    bool tls = !strncmp(name, "tls:", 4);
    if (tls)
        name += 4;
    for (size_t i = 0; i < os_api_count; i++) {
        const struct os_api_entry *e = &os_api_table[i];
        if (strcmp(e->name, name) != 0 || e->tls != tls)
            continue;
        if (!tls)
            return (void *)e->addr;
        const char *addr = ((const void *(*)(void))e->addr)();
        return (void *)(addr - (const char *)read_sysreg(tpidr_el0));
    }
    return nullptr;
}

// 恒等マップで RAM はすべて実行できるので、普通に確保する（命令キャッシュとの整合は memlink.c が取る）
static void *alloc_code(size_t size)
{
    return aligned_alloc(CODE_ALIGN, (size + CODE_ALIGN - 1) / CODE_ALIGN * CODE_ALIGN);
}

static void free_code(void *p, size_t size)
{
    (void)size;
    free(p);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// アプリに渡す定義。_POSIX_MONOTONIC_CLOCK は「CLOCK_MONOTONIC がある」の印で、本来は OS（unistd.h）が決める。
// picolibc は RTEMS のときしか定義せず、time.h が CLOCK_MONOTONIC を隠すので、Monocs（os/libc/posix.c の
// clock_gettime）が持つことをここで知らせる。picolibc・カーネルのビルド（slimcc コマンド）には渡さない
// _POSIX_THREADS は pthread がある印（os/libc/pthread.c）。_SC_NPROCESSORS_* などは picolibc の unistd.h にないので
// ここで渡す（値は os/libc/syscalls.c の sysconf と同じ）
// コンパイルがメモリを多く使った（cc/libslimcc.c が SLIMCC_MEM_REPORT 以上のとき呼ぶ）: dmesg に残す（2026-11-20。vim の
// regexp.c の 1 つの関数が中間表現で 240MB を使い、3 コアで同時にコンパイルしてヒープが尽きた。どこが重いかを残す）
void slimcc_mem_report(const char *path, size_t zone_bytes, const char *big_fn, size_t big_fn_bytes)
{
    klog_printf("cc: %s used %zu MB (the largest function in the IR: %s, %zu MB)", path, zone_bytes >> 20,
                big_fn[0] ? big_fn : "-", big_fn_bytes >> 20);
}

static const char *const app_defines[] = {"_POSIX_MONOTONIC_CLOCK=200112L", "_POSIX_THREADS=200809L",
                                          "_SC_NPROCESSORS_CONF=1000", "_SC_NPROCESSORS_ONLN=1001",
                                          "_SC_PHYS_PAGES=1002", "_SC_AVPHYS_PAGES=1003",
                                          // tmpnam の名前の大きさと置き場所（Linux の glibc と同じ。picolibc は 8 と ""。
                                          // 2026-11-16。os/libc/posix.c の tmpnam・tmpfile）
                                          "__L_tmpnam=20", "__P_tmpdir=\"/tmp\"",
                                          // POSIX の _setjmp・_longjmp（シグナルのマスクを保存しない版。Monocs の
                                          // setjmp はもともと保存しないので同じ。picolibc にない。Lua の LUA_USE_POSIX）
                                          "_setjmp=setjmp", "_longjmp=longjmp",
                                          // select の fd_set の大きさ（Linux の glibc と同じ 1024。picolibc は 64 で、
                                          // fd は 255 まで使える。カーネルの select は n ビットだけを読み書きする。
                                          // os/libc/select.c。2026-11-21）
                                          "FD_SETSIZE=1024", nullptr};

// 共有のライブラリ（-l名前）の置き場所。ライブラリは C のソースのまま置き、アプリと一緒にコンパイルしてつなぐ
#define LIB_DIR "/usr/lib"
#define MAX_LIBS 16

// コンパイラのオプション（-std=、-D、-U、-I）と、つなぐライブラリ（-l）。文字列は呼び出し側が持ち続けること
#define MAX_FLAGS 256     // -D・-U・-I それぞれの数（-D は Monocs が渡す app_defines の分も含む）
typedef struct Flags {
    SlimccOptions opt;
    char key[1024];     // オプションを表す文字列（キャッシュのキー）
    const char *defines[MAX_FLAGS + 2];
    const char *undefines[MAX_FLAGS + 1];
    const char *includes[MAX_FLAGS + MAX_LIBS + 1];
    const char *forced[MAX_FLAGS + 1];  // -include FILE（2026-11-20、vim）
    int nforced;
    bool want_include;                  // 前が -include（次がファイル）
    int ndef, nundef, ninc;
    char libs[MAX_LIBS][256];       // /lib/名前
    char lib_inc[MAX_LIBS][260];    // -I に足す /lib/名前（includes が指す）
    int nlib;
    char self_inc[260];             // ライブラリ自身の /lib/名前（ライブラリの .c が自分のヘッダーを探せるように）
    bool too_many;                  // 上の表に入りきらないオプションがあった（コンパイルしない）
    bool key_full;                  // key に入りきらなかった（終わりを全部の文字列のハッシュにする）
    uint64_t key_hash;              // key に足した文字列全部の FNV-1a
} Flags;

// キャッシュのキーに足す。入りきらなければ、終わりの 17 文字を全部のハッシュにする（前は黙って切れ、オプションの
// 違うコンパイルが同じキーになりえた。2026-11-20）
static void key_add(Flags *f, const char *a)
{
    if (!f->key_hash)
        f->key_hash = 0xcbf29ce484222325ULL;
    for (const char *p = a;; p++) {
        f->key_hash = (f->key_hash ^ (uint8_t)(*p ? *p : 0x1f)) * 0x100000001b3ULL;
        if (!*p)
            break;
    }
    size_t k = strlen(f->key);
    if (!f->key_full && k + strlen(a) + 2 <= sizeof(f->key)) {
        snprintf(f->key + k, sizeof(f->key) - k, "%s\x1f", a);
        return;
    }
    f->key_full = true;
    snprintf(f->key + sizeof(f->key) - 18, 18, "#%016llx", (unsigned long long)f->key_hash);
}

static void flag_overflow(Flags *f, const char *a, int max)
{
    if (!f->too_many)
        fprintf(stderr, "run: too many %.2s options (up to %d in all); not compiling\n", a, max);
    f->too_many = true;
}

static bool add_flag(Flags *f, const char *a)
{
    // キャッシュのキーにも入れる（順も含めて同じオプションなら同じキー）
    key_add(f, a);
    if (f->want_include) {
        f->want_include = false;
        if (f->nforced < MAX_FLAGS)
            f->forced[f->nforced++] = a;
        else
            flag_overflow(f, a, MAX_FLAGS);
    } else if (!strcmp(a, "-include"))
        f->want_include = true;     // GCC と同じく、次の語がファイル
    else if (!strncmp(a, "-std=", 5))
        f->opt.std = a + 5;
    else if (!strncmp(a, "-D", 2) && a[2]) {
        if (f->ndef < MAX_FLAGS)
            f->defines[f->ndef++] = a + 2;
        else
            flag_overflow(f, a, MAX_FLAGS);
    } else if (!strncmp(a, "-U", 2) && a[2]) {
        if (f->nundef < MAX_FLAGS)
            f->undefines[f->nundef++] = a + 2;
        else
            flag_overflow(f, a, MAX_FLAGS);
    }
    else if (!strcmp(a, "-fopenmp"))
        f->opt.openmp = true;   // #pragma omp（cc/omp.inc。ランタイムは os/libc/omp.c）
    else if (!strcmp(a, "-fno-schedule-insns2") || !strcmp(a, "-fschedule-insns2"))
        f->opt.no_schedule = a[2] == 'n';   // 命令の並べ替え（cc/sched.inc。既定でする）
    else if (!strcmp(a, "-fno-unroll-loops") || !strcmp(a, "-funroll-loops"))
        f->opt.no_unroll = a[2] == 'n';     // ループの展開（cc/ir.inc。既定でする）
    else if (!strcmp(a, "-fno-strict-aliasing") || !strcmp(a, "-fstrict-aliasing"))
        f->opt.no_strict_aliasing = a[2] == 'n';  // 型による別名の判定（cc/ir.inc。既定でする）
    else if (!strncmp(a, "-I", 2) && a[2]) {
        if (f->ninc < MAX_FLAGS)
            f->includes[f->ninc++] = a + 2;
        else
            flag_overflow(f, a, MAX_FLAGS);
    } else if (!strncmp(a, "-l", 2) && a[2] && !strchr(a + 2, '/') && f->nlib >= MAX_LIBS) {
        flag_overflow(f, a, MAX_LIBS);
    } else if (!strncmp(a, "-l", 2) && a[2] && !strchr(a + 2, '/')) {
        // /lib/名前 の .c を一緒にコンパイルし、そのヘッダーも探せるようにする
        snprintf(f->libs[f->nlib], sizeof(f->libs[0]), "%s/%s", LIB_DIR, a + 2);
        snprintf(f->lib_inc[f->nlib], sizeof(f->lib_inc[0]), "%s", f->libs[f->nlib]);
        if (f->ninc < MAX_FLAGS + MAX_LIBS)
            f->includes[f->ninc++] = f->lib_inc[f->nlib];
        f->nlib++;
    } else
        return false;
    return true;
}

static void init_flags(Flags *f)
{
    *f = (Flags){.opt = {.std = "gnu23"}};
    for (int i = 0; app_defines[i]; i++) {
        f->defines[f->ndef++] = app_defines[i];
        key_add(f, app_defines[i]);
    }
    f->opt.defines = f->defines;
    f->opt.undefines = f->undefines;
    f->opt.include_paths = f->includes;
    f->opt.forced_includes = f->forced;
}

static void add_flags(Flags *f, const char *const *flags, const char *from)
{
    for (int i = 0; flags && flags[i]; i++)
        if (!add_flag(f, flags[i]))
            fprintf(stderr, "%s: unknown option %s (ignored)\n", from, flags[i]);
}

// ディレクトリの compile_flags.txt（1 行に 1 つのオプション。clangd と同じ）を読む。返すのは読んだ文字列の塊
// （オプションが指すので、コンパイルが終わるまで解放しない）。-I の相対パスはディレクトリから
static char *read_compile_flags(const char *dir, Flags *f)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/compile_flags.txt", dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return nullptr;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    char line[512];
    while (buf && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#')
            continue;
        char tmp[600];
        if (!strncmp(line, "-I", 2) && line[2] && line[2] != '/')
            snprintf(tmp, sizeof(tmp), "-I%s/%s", dir, line + 2);
        else
            snprintf(tmp, sizeof(tmp), "%s", line);
        size_t n = strlen(tmp) + 1;
        if (len + n > cap)
            break;
        memcpy(buf + len, tmp, n);
        len += n;
    }
    fclose(fp);
    for (size_t p = 0; buf && p < len; p += strlen(buf + p) + 1)
        if (!add_flag(f, buf + p))
            fprintf(stderr, "%s: unknown option %s (ignored)\n", path, buf + p);
    return buf;
}

// ---- 進み具合（2026-11-21。ユーザーの「キャッシュをクリアして vim を起動すると数十秒待たされるが、フリーズか
// コンパイル中か判断できない」から） ----
// run・名前で動かすソースのプログラムの読み込みが PROGRESS_AFTER_US を超えたら、標準エラー（端末のときだけ。
// nos.py・ssh のコマンドの出力やリダイレクトには混ぜない）に「名前: compiling 57/134 files」を出し、
// 同じ行を書き換える。キャッシュから読むだけなら loading、つなぐ間は linking。終わったら行を消してから動かす。
// 書くのは、ファイルが 1 つ終わったときと、コンパイラが関数を 1 つ出し終えたとき（slimcc_progress。どのコアからも）
#define PROGRESS_AFTER_US   500000
#define PROGRESS_EVERY_US   200000

static struct {
    atomic_bool on, shown;
    atomic_int done, total;
    _Atomic uint64_t next_us;   // 次に書いてよい時刻
    atomic_flag busy;
    uint64_t misses0;           // 始めのキャッシュの外れの数（増えたらコンパイルしている）
    char name[64];
} prog;     // 0 で始まる（atomic_flag も 0 が外れ）

static void progress_write(const char *what)
{
    uint64_t hits, misses;
    if (!what) {
        cache_stats(&hits, &misses);
        what = misses != prog.misses0 ? "compiling" : "loading";
    }
    // 終わりの \r: カーソルを行の頭に置く（コンパイラの誤りのメッセージが、この行の頭から上書きして始まるように）
    fprintf(stderr, "\r\033[K%s: %s %d/%d files\r", prog.name, what, atomic_load(&prog.done),
            atomic_load(&prog.total));
    atomic_store(&prog.shown, true);
}

static void progress_tick(void)
{
    if (!atomic_load_explicit(&prog.on, memory_order_relaxed))
        return;
    uint64_t now = timer_now_us();
    if (now < atomic_load(&prog.next_us) || atomic_flag_test_and_set_explicit(&prog.busy, memory_order_acquire))
        return;
    if (atomic_load(&prog.on) && now >= atomic_load(&prog.next_us)) {
        atomic_store(&prog.next_us, now + PROGRESS_EVERY_US);
        progress_write(nullptr);
    }
    atomic_flag_clear_explicit(&prog.busy, memory_order_release);
}

void slimcc_progress(void)
{
    progress_tick();
}

// #include のファイルが見つからなかったとき（cc/libslimcc.h の slimcc_include_hint。2026-11-21）: 同じ名前のファイルが
// 共有のライブラリ（/usr/lib/名前/ の直下か inc/・include/）にあれば、-l名前 の付け忘れとみて案内を返す
// （例: #include <monocs_pwm.h> を -lpwm なしで）。勝手につながないのは、Linux の gcc と同じ動きにしておくため。
// エラーのときだけ呼ばれるので、ふだんのコンパイルの速さには関係しない
const char *slimcc_include_hint(const char *filename)
{
    static _Thread_local char hint[320];
    static const char *const subdirs[] = { "", "inc/", "include/" };
    if (!filename || !*filename || filename[0] == '/' || strstr(filename, ".."))
        return nullptr;
    DIR *d = opendir("/usr/lib");
    if (!d)
        return nullptr;
    const char *found = nullptr;
    char lib[64];
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        // slimcc（コンパイラのヘッダー）と firmware はライブラリではない
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "slimcc") || !strcmp(e->d_name, "firmware") ||
            strlen(e->d_name) >= sizeof(lib))
            continue;
        for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
            char path[512];
            struct stat st;
            snprintf(path, sizeof(path), "/usr/lib/%s/%s%s", e->d_name, subdirs[i], filename);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                strcpy(lib, e->d_name);
                found = lib;
                break;
            }
        }
    }
    closedir(d);
    if (!found)
        return nullptr;
    snprintf(hint, sizeof(hint),
             "note: %s is in the library /usr/lib/%s; add -l%s (run -l%s FILE, cc -l%s, or LDLIBS = -l%s in the Makefile)",
             filename, found, found, found, found, found);
    return hint;
}

static void progress_begin(const char *path)
{
    atomic_store(&prog.shown, false);
    if (!path || !isatty(2))
        return;
    const char *end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/')
        end--;
    const char *b = end;
    while (b > path && b[-1] != '/')
        b--;
    size_t len = (size_t)(end - b);
    if (len > 2 && !strncmp(end - 2, ".c", 2))
        len -= 2;
    snprintf(prog.name, sizeof prog.name, "%.*s", (int)len, b);
    uint64_t hits;
    cache_stats(&hits, &prog.misses0);
    atomic_store(&prog.done, 0);
    atomic_store(&prog.total, 1);
    atomic_store(&prog.next_us, timer_now_us() + PROGRESS_AFTER_US);
    atomic_store(&prog.on, true);
}

// 書いている途中のコアを待って止める
static void progress_end(void)
{
    if (!atomic_exchange(&prog.on, false))
        return;
    while (atomic_flag_test_and_set_explicit(&prog.busy, memory_order_acquire))
        __asm__ volatile("yield");
    atomic_flag_clear_explicit(&prog.busy, memory_order_release);
}

// ファイルを分担してコンパイルする（コアごとに slimcc の状態を持つので、同時にコンパイルできる。
// tools/selfhost/build.c と同じ）。どのコアが何を受け持っても、つなぐ順は名前の順なので結果は同じ
typedef struct Flags Flags;

typedef struct {
    char **names;
    const Flags **flags;    // ファイルごとのオプション（アプリとライブラリで違う）
    SlimccObj **objs;
    int n;
    atomic_int next;
    atomic_bool failed;
    bool cached;            // run: コンパイルのキャッシュを使う（os/slimcc/cache.c）
} CompileJob;

static void compile_worker(void *arg)
{
    CompileJob *j = arg;
    // コア 0 と同じくカーネル用のヒープから（2026-11-21。アプリの中の system() で動かすときも、ほかのコアは
    // 深さ 0 で始まるので、呼んだアプリのヒープから取っていた。そのアプリが終わると捨てられるのに、コンパイラは
    // コアごとの状態に覚えていて、次のコンパイルで free: invalid pointer が出た）
    malloc_kernel_begin();
    slimcc_cache_files(true);   // 同じヘッダーを読む .c をまとめてコンパイルするので、読んだものを使い回す
    while (!atomic_load(&j->failed)) {
        int i = atomic_fetch_add(&j->next, 1);
        if (i >= j->n)
            break;
        j->objs[i] = j->flags[i]->too_many ? nullptr
                     : j->cached ? cache_compile(j->names[i], &j->flags[i]->opt, j->flags[i]->key)
                                 : slimcc_compile(j->names[i], nullptr, &j->flags[i]->opt);
        if (!j->objs[i])
            atomic_store(&j->failed, true);
        atomic_fetch_add(&prog.done, 1);
        progress_tick();
    }
    slimcc_cache_files(false);
    malloc_kernel_end();
}

// コンパイルするファイルの一覧
typedef struct {
    char **names;
    const Flags **flags;    // それぞれのファイルのオプション
    int n, cap;
} Sources;

static bool add_source(Sources *s, const char *path, const Flags *f)
{
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        char **p = realloc(s->names, sizeof(char *) * (size_t)cap);
        if (!p)
            return false;
        s->names = p;
        const Flags **q = realloc(s->flags, sizeof(Flags *) * (size_t)cap);
        if (!q)
            return false;
        s->flags = q;
        s->cap = cap;
    }
    s->flags[s->n] = f;
    size_t len = strlen(path) + 1;
    if (!(s->names[s->n] = malloc(len)))
        return false;
    memcpy(s->names[s->n++], path, len);
    return true;
}

static void free_sources(Sources *s)
{
    for (int i = 0; i < s->n; i++)
        free(s->names[i]);
    free(s->names);
    free(s->flags);
    *s = (Sources){0};
}

// 除くもの（アプリ・ライブラリのフォルダの .runignore。.gitignore と同じ考え方）: 1 行に 1 つのパターン
// （# で始まる行と空の行は飛ばす。* ? [] が使える。fnmatch）。/ を含まないもの（test_buffer.c、*_test.c）は
// どのフォルダの中でも名前で比べ、/ を含むもの（tests/、tools/*.c）はフォルダからの位置で比べる。
// 終わりが / ならフォルダだけ。compile_flags.txt に書かないのは、clangd がその行をすべてオプションとして読むため
#define MAX_IGNORE 64
typedef struct {
    char pats[MAX_IGNORE][128];
    bool dir_only[MAX_IGNORE];
    int n;
} Ignore;

static void load_ignore(const char *root, Ignore *ig)
{
    ig->n = 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/.runignore", root);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    char line[160];
    while (ig->n < MAX_IGNORE && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t len = strlen(p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            p[--len] = '\0';
        if (!len || p[0] == '#')
            continue;
        bool dir_only = p[len - 1] == '/';
        if (dir_only)
            p[--len] = '\0';
        if (p[0] == '/')    // 先頭の / は「このフォルダから」（.gitignore と同じ）
            p++;
        if (!*p)
            continue;
        snprintf(ig->pats[ig->n], sizeof(ig->pats[0]), "%s", p);
        ig->dir_only[ig->n++] = dir_only;
    }
    fclose(fp);
}

// rel（フォルダからの位置）を除くか
static bool ignored(const Ignore *ig, const char *rel, bool is_dir)
{
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    for (int i = 0; i < ig->n; i++) {
        if (ig->dir_only[i] && !is_dir)
            continue;
        const char *pat = ig->pats[i];
        bool match = strchr(pat, '/') ? fnmatch(pat, rel, FNM_PATHNAME) == 0 : fnmatch(pat, base, 0) == 0;
        if (match)
            return true;
    }
    return false;
}

// dir の中（サブフォルダも）の .c を足す。「.」で始まるもの（隠しファイル・.cache など）と、.runignore に
// 書いたものは飛ばす。root は .runignore を読んだフォルダ。足したものは、この呼び出しの分だけ名前の順に
// 並べる（どのコアが何をコンパイルしても、つなぐ順は同じ）
static bool add_directory_in(Sources *s, const char *root, const Ignore *ig, const char *dir, const Flags *f,
                             int depth)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "%s: cannot open\n", dir);
        return false;
    }
    int first = s->n;
    bool ok = true;
    struct dirent *e;
    char sub[512];
    while (ok && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.')
            continue;
        snprintf(sub, sizeof(sub), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
        size_t rlen = strlen(root);
        const char *rel = sub + rlen + (root[rlen - 1] == '/' ? 0 : 1);
        bool is_dir = e->d_type == DT_DIR;
        if (ignored(ig, rel, is_dir))
            continue;
        if (is_dir) {
            if (depth < 8)      // 深すぎるもの（輪になったリンクはないが念のため）は探さない
                ok = add_directory_in(s, root, ig, sub, f, depth + 1);
        } else if (len >= 3 && !strcmp(e->d_name + len - 2, ".c")) {
            ok = add_source(s, sub, f);
        }
    }
    closedir(d);
    qsort(s->names + first, (size_t)(s->n - first), sizeof(char *), compare_names);
    return ok;
}

// アプリ・ライブラリのフォルダ root の .c を足す（root の .runignore を使う）
static bool add_directory(Sources *s, const char *root, const Flags *f)
{
    Ignore *ig = malloc(sizeof(Ignore));
    if (!ig)
        return false;
    load_ignore(root, ig);
    bool ok = add_directory_in(s, root, ig, root, f, 0);
    free(ig);
    return ok;
}

// フォルダの設定: Makefile（Linux と同じもの。os/slimcc/makefile.c）があって、プログラムの .c がわかれば、
// そのオプションと .c。なければ compile_flags.txt と、フォルダの中の .c（.runignore を除く）
typedef struct {
    MakeInfo mk;
    bool make;          // Makefile を使った
    char *keep;         // compile_flags.txt の文字列
} FolderConf;

// オプションを f に読み（extra はその後に足す。-std= は後のものが勝つ）、.c を s に足す。
// オプションの文字列は c が持つ（コンパイルが終わるまで free_folder しない）
static bool load_folder(Sources *s, const char *dir, Flags *f, FolderConf *c, const char *const *extra)
{
    *c = (FolderConf){0};
    if (make_read(dir, &c->mk)) {
        if (c->mk.nsrcs)
            c->make = true;
        else
            make_free(&c->mk);  // プログラムを作る規則がない: 前と同じく、フォルダの .c を使う
    }
    if (c->make) {
        for (int i = 0; i < c->mk.nflags; i++)
            add_flag(f, c->mk.flags[i]);
    } else {
        c->keep = read_compile_flags(dir, f);
    }
    add_flags(f, extra, "run");
    if (!c->make)
        return add_directory(s, dir, f);
    for (int i = 0; i < c->mk.nsrcs; i++)
        if (!add_source(s, c->mk.srcs[i], f))
            return false;
    return true;
}

static void free_folder(FolderConf *c)
{
    make_free(&c->mk);
    free(c->keep);
    *c = (FolderConf){0};
}

// つなぐライブラリ。ライブラリは、そのフォルダの Makefile か compile_flags.txt（なければ既定）でコンパイルする
// （アプリのオプションは使わない。ふつうの OS でライブラリを別にビルドするのと同じ。同じライブラリを使う
// アプリどうしでコンパイルのキャッシュも共有できる）。そこに -l があれば、そのライブラリもつなぐ
typedef struct {
    char name[64];
    Flags *f;
    FolderConf conf;    // Makefile・compile_flags.txt の文字列（f が指す）
} Lib;

typedef struct {
    Lib libs[MAX_LIBS];
    int n;
} LibSet;

static void free_libs(LibSet *ls)
{
    for (int i = 0; i < ls->n; i++) {
        free_folder(&ls->libs[i].conf);
        free(ls->libs[i].f);
    }
    ls->n = 0;
}

static bool add_library(Sources *s, LibSet *ls, const char *name, int depth)
{
    for (int i = 0; i < ls->n; i++)
        if (!strcmp(ls->libs[i].name, name))
            return true;    // もうつないだ（輪になった指定でも止まる）
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/%s", LIB_DIR, name);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "-l%s: %s がありません\n", name, dir);
        return false;
    }
    if (ls->n == MAX_LIBS || depth > 8) {
        fprintf(stderr, "-l%s: ライブラリが多すぎます\n", name);
        return false;
    }
    Flags *f = malloc(sizeof(Flags));
    if (!f)
        return false;
    init_flags(f);
    Lib *lib = &ls->libs[ls->n++];
    snprintf(lib->name, sizeof(lib->name), "%s", name);
    lib->f = f;
    // 自分のヘッダーを <名前.h> でも探せるように、自分のフォルダを -I に（キャッシュのキーにも入る）
    snprintf(f->self_inc, sizeof(f->self_inc), "-I%s", dir);
    add_flag(f, f->self_inc);
    int before = s->n;
    if (!load_folder(s, dir, f, &lib->conf, nullptr))
        return false;
    if (s->n == before) {
        fprintf(stderr, "%s: no .c files\n", dir);
        return false;
    }
    // このライブラリが使うライブラリ（-l は add_flag が f->libs に入れ、-I も足してある）
    for (int i = 0; i < f->nlib; i++)
        if (!add_library(s, ls, f->libs[i] + strlen(LIB_DIR) + 1, depth + 1))
            return false;
    return true;
}

// アプリがつなぐライブラリ（-l）の .c を足す
static bool add_libraries(Sources *s, LibSet *ls, const Flags *f)
{
    for (int i = 0; i < f->nlib; i++)
        if (!add_library(s, ls, f->libs[i] + strlen(LIB_DIR) + 1, 0))
            return false;
    return true;
}

// ファイルを分担してコンパイルし、1 つのプログラムにつなぐ（make の SRCS の代わり）
// s の .c をすべてコンパイルする（使えるコアで分担）。結果の配列（n 個）を返す。失敗したら NULL
static SlimccObj **compile_many(Sources *s, bool cached)
{
    int n = s->n;
    SlimccObj **objs = calloc((size_t)n, sizeof(SlimccObj *));
    if (!objs)
        return nullptr;
    CompileJob job = {.names = s->names, .flags = s->flags, .objs = objs, .n = n, .cached = cached};
    atomic_store(&prog.total, n);
    if (cached)
        cache_begin();
    bool started[SMP_MAX_CORES] = {};
    for (int c = 1; c < smp_cores() && c < n; c++)
        started[c] = smp_run(c, compile_worker, &job);
    compile_worker(&job);
    for (int c = 1; c < SMP_MAX_CORES; c++)
        if (started[c] && !smp_wait(c))
            atomic_store(&job.failed, true);
    if (cached)
        cache_end();
    if (atomic_load(&job.failed)) {
        for (int i = 0; i < n; i++)
            slimcc_free_obj(objs[i]);
        free(objs);
        return nullptr;
    }
    return objs;
}

static void free_objs(SlimccObj **objs, int n)
{
    for (int i = 0; i < n; i++)
        slimcc_free_obj(objs[i]);
    free(objs);
}

// プログラムをコンパイルして、つなぐ前の結果（*n 個）を返す。失敗したら NULL。
// path がフォルダなら Makefile のプログラムの .c（なければ中の .c。サブフォルダも）と、つなぐライブラリ。
// Makefile・compile_flags.txt のオプションを先に、コマンドで指定したものを後に（-std= は後のものが勝つ）
static SlimccObj **compile_program(const char *path, const char *src, const char *const *flags, bool cached, int *n)
{
    Flags f;
    init_flags(&f);
    struct stat st;
    SlimccObj **objs = nullptr;
    *n = 0;
    if (path && !src && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        Sources s = {0};
        LibSet ls = {0};
        FolderConf conf;
        if (load_folder(&s, path, &f, &conf, flags)) {
            if (!s.n) {
                fprintf(stderr, "%s: no .c files\n", path);
            } else if (add_libraries(&s, &ls, &f)) {
                objs = compile_many(&s, cached);
                *n = s.n;
            }
        }
        free_sources(&s);
        free_libs(&ls);
        free_folder(&conf);
        return objs;
    }
    add_flags(&f, flags, "run");
    if (path && !src && f.nlib) {
        // 1 つのファイルと、つなぐライブラリ（run -l名前 FILE.c）
        Sources s = {0};
        LibSet ls = {0};
        if (add_source(&s, path, &f) && add_libraries(&s, &ls, &f)) {
            objs = compile_many(&s, cached);
            *n = s.n;
        }
        free_sources(&s);
        free_libs(&ls);
        return objs;
    }
    SlimccObj *obj;
    if (f.too_many)
        return nullptr;
    if (path && !src && cached) {
        cache_begin();
        obj = cache_compile(path, &f.opt, f.key);
        cache_end();
    } else if (path && !src) {
        obj = slimcc_compile(path, nullptr, &f.opt);
    } else {
        obj = slimcc_compile(path ? path : "<cc>", src, &f.opt);
    }
    if (!obj)
        return nullptr;
    objs = malloc(sizeof *objs);
    if (!objs) {
        slimcc_free_obj(obj);
        return nullptr;
    }
    objs[0] = obj;
    *n = 1;
    return objs;
}

static SlimccImage *link_objs(SlimccObj **objs, int n)
{
    if (n == 1)
        return slimcc_link(objs[0], resolve, nullptr, alloc_code, free_code);
    return slimcc_link_many(objs, n, resolve, nullptr, alloc_code, free_code);
}

// ---- 実行ファイル（2026-11-17。Linux と同じく make・cc -o で作り、./prog で動かす） ----
// 形（リトルエンディアン）: "MONOCSX\0"、u32 版、u32 オブジェクトの数、各: u64 大きさと slimcc_obj_save の中身。
// 中はつなぐ前のコンパイルの結果（OS の関数は名前のまま）で、動かすたびに OS の関数とつなぐ。なので、
// カーネルを入れ替えても作り直さずに動く（Linux の実行ファイルが、新しいカーネルでも動くのと同じ）
#define EXE_MAGIC   "MONOCSX"   // 8 バイト（終わりの \0 も）。2026-11-21 までは "UNUXEXE"、その後 "HIBANAX"（作り直す）
#define EXE_VERSION 1

bool slimcc_is_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;   // フォルダ・ないもの（フォルダを fopen しない）
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char m[8];
    bool ok = fread(m, 1, 8, f) == 8 && !memcmp(m, EXE_MAGIC, 8);
    fclose(f);
    return ok;
}

static bool write_executable(const char *out, SlimccObj **objs, int n)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "%s: cannot create\n", out);
        return false;
    }
    uint32_t ver = EXE_VERSION, cnt = (uint32_t)n;
    bool ok = fwrite(EXE_MAGIC, 1, 8, f) == 8 && fwrite(&ver, 4, 1, f) == 1 && fwrite(&cnt, 4, 1, f) == 1;
    for (int i = 0; ok && i < n; i++) {
        char *buf = nullptr;
        size_t len = 0;
        FILE *m = open_memstream(&buf, &len);
        ok = m && slimcc_obj_save(objs[i], m);
        if (m)
            fclose(m);
        uint64_t len64 = len;
        ok = ok && fwrite(&len64, 8, 1, f) == 1 && fwrite(buf, 1, len, f) == len;
        free(buf);
    }
    ok = !fclose(f) && ok;
    if (!ok || rename(tmp, out) != 0) {
        remove(tmp);
        fprintf(stderr, "%s: cannot write\n", out);
        return false;
    }
    return true;
}

// 実行ファイルの中のオブジェクトを読み解く仕事（使えるコアで分担する。Lua の 33 個を 1 つのコアで読むと、
// ソースからキャッシュで動かすより遅かった）
typedef struct {
    const char *buf;
    const uint64_t *off, *len;
    SlimccObj **objs;
    int n;
    _Atomic int next;
    _Atomic bool failed;
} ExeJob;

static void exe_worker(void *arg)
{
    ExeJob *j = arg;
    malloc_kernel_begin();      // コア 0 と同じくカーネル用のヒープから（compile_worker と同じ）
    for (int i; (i = atomic_fetch_add(&j->next, 1)) < j->n;) {
        j->objs[i] = slimcc_obj_load_mem(j->buf + j->off[i], j->len[i]);
        if (!j->objs[i])
            atomic_store(&j->failed, true);
    }
    malloc_kernel_end();
}

static char *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    char *buf = nullptr;
    long size;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)size + 1)) &&
        fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = nullptr;
    }
    if (buf)
        *len = (size_t)size;
    fclose(f);
    return buf;
}

static SlimccImage *load_executable(const char *path)
{
    size_t size = 0;
    char *buf = read_whole(path, &size);
    if (!buf)
        return nullptr;
    uint32_t ver, cnt;
    if (size < 16 || memcmp(buf, EXE_MAGIC, 8) || (memcpy(&ver, buf + 8, 4), memcpy(&cnt, buf + 12, 4), ver != EXE_VERSION) ||
        cnt == 0 || cnt > 4096) {
        fprintf(stderr, "%s: not a Monocs executable of this version (make it again)\n", path);
        free(buf);
        return nullptr;
    }
    uint64_t *off = calloc(cnt, sizeof *off), *len = calloc(cnt, sizeof *len);
    SlimccObj **objs = calloc(cnt, sizeof *objs);
    bool ok = off && len && objs;
    size_t p = 16;
    for (uint32_t i = 0; ok && i < cnt; i++) {
        uint64_t l;
        if (p + 8 > size)
            ok = false;
        else {
            memcpy(&l, buf + p, 8);
            p += 8;
            ok = l <= size - p;
            off[i] = p;
            len[i] = l;
            p += l;
        }
    }
    SlimccImage *img = nullptr;
    if (ok) {
        ExeJob job = {.buf = buf, .off = off, .len = len, .objs = objs, .n = (int)cnt};
        bool started[SMP_MAX_CORES] = {};
        for (int c = 1; c < smp_cores() && c < (int)cnt; c++)
            started[c] = smp_run(c, exe_worker, &job);
        exe_worker(&job);
        for (int c = 1; c < SMP_MAX_CORES; c++)
            if (started[c] && !smp_wait(c))
                atomic_store(&job.failed, true);
        ok = !atomic_load(&job.failed);
        if (ok)
            img = link_objs(objs, (int)cnt);
    }
    if (!ok)
        fprintf(stderr, "%s: broken executable\n", path);
    if (objs)
        free_objs(objs, (int)cnt);
    free(off);
    free(len);
    free(buf);
    return img;
}

bool slimcc_make_executable(const char *path, const char *const *flags, const char *out)
{
    int n;
    SlimccObj **objs = compile_program(path, nullptr, flags, false, &n);
    if (!objs)
        return false;
    // つながるか（足りない関数がないか）を先に確かめる（Linux の ld の undefined reference と同じ時に知らせる）
    SlimccImage *img = link_objs(objs, n);
    bool ok = img && write_executable(out, objs, n);
    if (img)
        slimcc_free_image(img);
    free_objs(objs, n);
    return ok;
}

// cc -o OUT FILE.c...（gcc と同じ書き方）: いくつかの .c を、flags（-D・-U・-I・-std=・-l名前 など）でコンパイルして
// 1 つの実行ファイルにする
bool slimcc_make_executable_files(const char *const *files, int nfiles, const char *const *flags, const char *out)
{
    Flags f;
    init_flags(&f);
    add_flags(&f, flags, "cc");
    Sources s = {0};
    LibSet ls = {0};
    bool ok = true;
    for (int i = 0; ok && i < nfiles; i++)
        ok = add_source(&s, files[i], &f);
    ok = ok && add_libraries(&s, &ls, &f);
    SlimccObj **objs = nullptr;
    int n = s.n;
    if (ok && n) {
        objs = compile_many(&s, false);
    }
    free_sources(&s);
    free_libs(&ls);
    if (!objs)
        return false;
    SlimccImage *img = link_objs(objs, n);
    ok = img && write_executable(out, objs, n);
    if (img)
        slimcc_free_image(img);
    free_objs(objs, n);
    return ok;
}

// プログラムを読み込んで、OS の関数とつなぐ（実行はしない。サービス、os/kernel/svc.c）: 実行ファイルならそれを、
// ソース（.c・フォルダ）ならコンパイルして（キャッシュあり）
SlimccImage *slimcc_load_program(const char *path)
{
    if (slimcc_is_executable(path))
        return load_executable(path);
    int n;
    SlimccObj **objs = compile_program(path, nullptr, nullptr, true, &n);
    if (!objs)
        return nullptr;
    SlimccImage *img = link_objs(objs, n);
    free_objs(objs, n);
    return img;
}

void *slimcc_program_symbol(SlimccImage *img, const char *name)
{
    return slimcc_symbol(img, name);
}

void slimcc_unload_program(SlimccImage *img)
{
    slimcc_free_image(img);
}

// perf record のサンプルのうち、このアプリの番地のものに関数名を付ける（アプリのコードと記号表を捨てる前に）。
// 名前は複製して取っておく（同じ名前は 1 つ。カーネル用のヒープ）
static const char *intern_name(const char *name)
{
    static const char **names;
    static int nnames, capnames;
    for (int i = 0; i < nnames; i++)
        if (!strcmp(names[i], name))
            return names[i];
    if (nnames == capnames) {
        int cap = capnames ? capnames * 2 : 256;
        const char **p = realloc(names, sizeof(char *) * (size_t)cap);
        if (!p)
            return nullptr;
        names = p;
        capnames = cap;
    }
    size_t len = strlen(name) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, name, len);
        names[nnames++] = copy;
    }
    return copy;
}

static void name_samples(SlimccImage *img)
{
    int n;
    PmuSample *s = pmu_samples_mut(&n);
    for (int i = 0; i < n; i++) {
        size_t off;
        if (!s[i].name) {
            const char *name = slimcc_symbol_at(img, (const void *)s[i].pc, &off);
            if (name) {
                s[i].name = intern_name(name);
                s[i].off = off;
            }
        }
        for (int c = 0; c < PMU_CALLS; c++) {
            if (s[i].call_names[c] || !s[i].calls[c])
                continue;
            // 戻り先は呼び出しの次の命令なので、1 つ前（呼び出しの命令）で引く
            const char *name = slimcc_symbol_at(img, (const void *)(s[i].calls[c] - 4), &off);
            if (name)
                s[i].call_names[c] = intern_name(name);
        }
    }
}

// アプリを動かす。アプリの実行中（アプリが system() を呼んだ）なら入れ子: 子のアプリの終わりの片付け（開いた
// ファイル・接続を閉じる、メモリを捨てる、端末を戻す）は子の分だけにして、外側のアプリの状態（curses・termios・
// pid）を退避して戻す。アプリを動かしたときは終了の値、動かせなかったら -1。止められたときは 128 + シグナルの番号（Ctrl-C 130、例外 139）、timeout の期限は 124
// アプリが止まった場所（os/kernel/fault.c が例外・Ctrl-C で記録したもの）を、関数名に直して出す。アプリの関数は
// アプリの記号表（slimcc_symbol_at）、カーネルの関数（アプリが呼んだ OS API の中など）は /boot/kernel8.sym。
// where に、いちばん上（止まった関数）を入れる（dmesg 用）
extern char __tdata_start[];    // カーネルのコード・定数の終わり（os/boot/linker.ld）

static void name_addr(SlimccImage *img, uint64_t addr, char *buf, size_t size)
{
    size_t off;
    const char *name = slimcc_symbol_at(img, (const void *)addr, &off);
    if (name) {
        snprintf(buf, size, "%s+%#zx", name, off);
        return;
    }
    const char *why;
    uint64_t koff;
    const char *kname = ksym_load(&why) ? ksym_at_off(addr, &koff) : nullptr;
    if (kname && addr >= 0x80000 && addr < (uint64_t)__tdata_start)
        snprintf(buf, size, "%s+%#llx [kernel]", kname, (unsigned long long)koff);
    else
        snprintf(buf, size, "%#llx", (unsigned long long)addr);
}

static void print_fault(SlimccImage *img, char *where, size_t size)
{
    where[0] = '\0';
    const FaultRecord *r = fault_get();
    if (!r)
        return;
    char buf[96];
    name_addr(img, r->pc, buf, sizeof buf);
    printf("  at %s%s\n", buf, r->core ? " (on another core: a thread)" : "");
    snprintf(where, size, "%s", buf);
    // 呼び出し元: x30（葉の関数のとき、まだフレームに積んでいない）と、フレームをたどった戻り先（同じものは 1 回）
    uint64_t prev = r->pc;
    uint64_t list[FAULT_FRAMES + 1];
    int n = 0;
    list[n++] = r->lr;
    for (int i = 0; i < r->nframes; i++)
        list[n++] = r->frames[i];
    for (int i = 0; i < n; i++) {
        uint64_t a = list[i];
        if (a == 0 || a == prev || (i == 0 && r->nframes > 0 && r->frames[0] == a))
            continue;
        prev = a;
        name_addr(img, a - 4, buf, sizeof buf);    // 戻り先の 1 つ前（呼んだ命令）
        if (!strncmp(buf, "app_call+", 9))
            break;      // ここから下はアプリを動かしたシェル（Monocs）の側
        // x30 が止まった関数の中を指すときは、前に呼んだ関数の戻り先が残っているだけ（呼び出し元ではない）
        size_t fn = strcspn(where, "+");
        if (i == 0 && !strncmp(buf, where, fn) && buf[fn] == '+')
            continue;
        printf("  from %s\n", buf);
    }
}

// 時間のかかった起動・終わりを dmesg に書く（2026-11-16。「たまに開くのが遅い」を後から調べるため）。
// 内訳: SD カードの読み書きの時間。残りはロックの待ちなど（どこで待っていたかも付ける）
#define SLOW_APP_US 1000000

typedef struct {
    uint64_t us;
    struct sd_stats sd;
} AppMark;

static void app_now(AppMark *m)
{
    m->us = timer_now_us();
    sd_get_stats(&m->sd);
}

// 測り始める（コア 0 なら、どこにいたかの記録も始める）
static void app_mark(AppMark *m)
{
    if (cpu_id() == 0)
        stall_sample_begin();
    app_now(m);
}

// コア 0 が多くいた関数（割り込みのたびの記録。os/kernel/exception.c）を、多い順に 3 つまで out に書く。
// 「関数 <- 呼び出し元（x30）」の形。眠っていた（cpu_idle など）なら、その呼び出し元で何を待っていたかがわかる
static void stall_where(char *out, size_t cap)
{
    const uint64_t *pc, *lr;
    const uint8_t *cr;
    int n = stall_sample_end(&pc, &lr, &cr);
    out[0] = '\0';
    if (n == 0) {
        snprintf(out, cap, "; no samples");
        return;
    }
    const char *why;
    malloc_kernel_begin();      // 関数名の表はカーネル用のヒープに（入れ子のアプリの間でも、捨てられないように）
    bool ok = ksym_load(&why);
    malloc_kernel_end();
    if (!ok) {
        snprintf(out, cap, "; %d samples (%s)", n, why);
        return;
    }
    // コアごとに数える（割合はそのコアの記録の数に対して）
    enum { TOP = 32 };
    const char *fn[TOP], *caller[TOP];
    int core[TOP], count[TOP] = {}, nt = 0, per_core[SMP_MAX_CORES] = {};
    for (int i = 0; i < n; i++) {
        const char *f = ksym_at(pc[i]), *c = ksym_at(lr[i] - 4);
        int k = 0;
        if (!f)
            f = "?";
        if (!c)
            c = "?";
        per_core[cr[i] & 3]++;
        while (k < nt && (fn[k] != f || caller[k] != c || core[k] != cr[i]))
            k++;
        if (k == nt) {
            if (nt == TOP)
                continue;
            fn[nt] = f;
            caller[nt] = c;
            core[nt] = cr[i];
            nt++;
        }
        count[k]++;
    }
    size_t len = 0;
    for (int c = 0; c < SMP_MAX_CORES; c++) {
        for (int r = 0; r < 2; r++) {   // コアごとに多い 2 つ
            int best = -1;
            for (int k = 0; k < nt; k++)
                if (core[k] == c && count[k] > 0 && (best < 0 || count[k] > count[best]))
                    best = k;
            if (best < 0)
                break;
            int w = snprintf(out + len, cap - len, "%score%d %s<-%s %d%%", len ? ", " : "; at ", c, fn[best],
                             caller[best], count[best] * 100 / per_core[c]);
            if (w < 0 || (size_t)w >= cap - len)
                return;
            len += (size_t)w;
            count[best] = 0;
        }
    }
}

// 起動の段階ごとの時間（slimcc_run_source が入れ、app_slow_log が付ける。2026-11-20、vim の起動を調べるため）
static char slow_phases[96];

static void app_slow_log(const char *what, const char *path, const AppMark *a)
{
    AppMark b;
    app_now(&b);
    uint64_t us = b.us - a->us;
    char where[400] = "";
    // 基準は環境変数 MONOCS_SLOW_MS（ミリ秒）で変えられる（なければ 1 秒）
    uint64_t limit = SLOW_APP_US;
    const char *env = getenv("MONOCS_SLOW_MS");
    if (env && *env >= '0' && *env <= '9')
        limit = (uint64_t)strtoull(env, nullptr, 10) * 1000;
    if (us < limit) {
        if (cpu_id() == 0) {
            const uint64_t *pc, *lr;
            const uint8_t *cr;
            stall_sample_end(&pc, &lr, &cr);
        }
        return;
    }
    if (cpu_id() == 0)
        stall_where(where, sizeof where);
    uint64_t rd = b.sd.read_us - a->sd.read_us, wr = b.sd.write_us - a->sd.write_us;
    uint64_t sd_us = rd + wr;
    klog_printf("app %s: slow %s %llu.%03llu s (%ssd read %llu ms %llu KB, write %llu ms %llu KB; other %llu ms%s)",
                path ? path : "-", what, (unsigned long long)(us / 1000000), (unsigned long long)(us % 1000000 / 1000),
                slow_phases, (unsigned long long)(rd / 1000), (unsigned long long)((b.sd.read_sectors - a->sd.read_sectors) / 2),
                (unsigned long long)(wr / 1000), (unsigned long long)((b.sd.write_sectors - a->sd.write_sectors) / 2),
                (unsigned long long)(us > sd_us ? (us - sd_us) / 1000 : 0), where);
}

static int run_image(SlimccImage *img, const char *path, int argc, char **argv);

// ---- つないだ結果の覚え（2026-11-20。vim の起動 2.1 秒のうち、キャッシュの確かめと読み込み 1.0 秒、つなぐ 0.9 秒、
// 部品の解放 0.3 秒を、2 回目から省く） ----
// run・名前で動かすソースのプログラムを、つないだ直後のメモリ（コード・データ・BSS のひと続き）の写しと一緒に覚え、
// 次は写しを戻して（アプリが書き換えたデータを最初に戻す）そのまま動かす。使わないのは:
//   - 覚えたハッシュのファイル（ソース・ヘッダー）が書き換えられた（cache_src_gen が進んだ。os/slimcc/cache.c）
//   - プログラムのフォルダ（ファイルならそのフォルダ）か /usr/lib の中で何かが変わった（.c が増えた、Makefile・
//     .monocs-make・compile_flags.txt が変わった、など。image_cache_changed）
//   - オプションが違う（鍵に入れる）、入れ子のアプリ（system()）から（つないだものがアプリ用のヒープに入る）、
//     同じものが入れ子で動いている
// 覚えるのは写しと合わせて IMG_CACHE_MAX まで（カーネル用のヒープ。使っていない古いものから捨てる）。数の上限
// IMG_CACHE_N は表の大きさだけのため（2026-11-21 に 3 → 32。ユーザーと合意。3 では ls・cat などの小さなコマンド
// （数十 KB）でも埋まり、そのたびに vim が押し出された。メモリは量の上限で抑えている）
#define IMG_CACHE_N     32
#define IMG_CACHE_MAX   (64u << 20)

typedef struct {
    char key[1024];         // パスとオプション
    char root[512];         // フォルダ（絶対パス・小文字）
    unsigned gen;           // つなぐ前の cache_src_gen
    bool stale, busy;
    SlimccImage *img;
    void *pristine;         // つないだ直後の写し
    size_t size;
    uint64_t used;
} ImgEntry;

static ImgEntry img_cache[IMG_CACHE_N];
static uint64_t img_clock;
static atomic_flag img_lock = ATOMIC_FLAG_INIT;

static void img_lock_take(void)
{
    while (atomic_flag_test_and_set_explicit(&img_lock, memory_order_acquire))
        __asm__ volatile("yield");
}

static void img_lock_give(void)
{
    atomic_flag_clear_explicit(&img_lock, memory_order_release);
}

static void img_drop(ImgEntry *e)
{
    slimcc_free_image(e->img);
    free(e->pristine);
    *e = (ImgEntry){0};
}

void image_cache_changed(const char *key)
{
    img_lock_take();
    for (int i = 0; i < IMG_CACHE_N; i++) {
        ImgEntry *e = &img_cache[i];
        if (!e->img)
            continue;
        size_t rl = strlen(e->root);
        if (!key || !strncmp(key, e->root, rl) || !strncmp(key, "/usr/lib/", 9))
            e->stale = true;
    }
    img_lock_give();
}

// key のつないだ結果があり使えるなら、写しを戻して返す（使っている印を付ける）
static SlimccImage *img_take(const char *key)
{
    SlimccImage *img = nullptr;
    img_lock_take();
    for (int i = 0; i < IMG_CACHE_N; i++) {
        ImgEntry *e = &img_cache[i];
        if (!e->img || strcmp(e->key, key))
            continue;
        if (e->stale || e->gen != cache_src_gen()) {
            if (!e->busy)
                img_drop(e);
            break;
        }
        if (e->busy)
            break;
        void *mem;
        size_t size;
        slimcc_image_range(e->img, &mem, &size);
        memcpy(mem, e->pristine, size);
        __clear_cache(mem, (char *)mem + size);
        e->busy = true;
        e->used = ++img_clock;
        img = e->img;
        break;
    }
    img_lock_give();
    return img;
}

// つないだ結果を覚える（gen はつなぐ前の cache_src_gen）。覚えたら true（img はこれから覚えの持ち物）
static bool img_put(const char *key, const char *root, unsigned gen, SlimccImage *img)
{
    void *mem;
    size_t size;
    slimcc_image_range(img, &mem, &size);
    if (size * 2 > IMG_CACHE_MAX || strlen(key) >= sizeof img_cache[0].key)
        return false;
    malloc_kernel_begin();
    void *copy = malloc(size);
    malloc_kernel_end();
    if (!copy)
        return false;
    memcpy(copy, mem, size);
    img_lock_take();
    // 場所: 空き、なければ使っていない古いもの。合計が上限を超えるなら、使っていない古いものから捨てる
    for (;;) {
        size_t total = size * 2;
        int victim = -1;
        for (int i = 0; i < IMG_CACHE_N; i++) {
            ImgEntry *e = &img_cache[i];
            if (!e->img)
                continue;
            total += e->size * 2;
            if (!e->busy && (victim < 0 || e->used < img_cache[victim].used))
                victim = i;
        }
        bool full = true;
        for (int i = 0; i < IMG_CACHE_N; i++)
            if (!img_cache[i].img)
                full = false;
        if (total <= IMG_CACHE_MAX && !full)
            break;
        if (victim < 0) {
            img_lock_give();
            free(copy);
            return false;
        }
        img_drop(&img_cache[victim]);
    }
    ImgEntry *e = nullptr;
    for (int i = 0; i < IMG_CACHE_N && !e; i++)
        if (!img_cache[i].img)
            e = &img_cache[i];
    *e = (ImgEntry){.gen = gen, .busy = true, .img = img, .pristine = copy, .size = size, .used = ++img_clock};
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->root, sizeof e->root, "%s", root);
    img_lock_give();
    return true;
}

// run_image の終わり: 覚えたものなら残し（使っている印を外す）、そうでなければ捨てる
static void img_release(SlimccImage *img)
{
    img_lock_take();
    for (int i = 0; i < IMG_CACHE_N; i++)
        if (img_cache[i].img == img) {
            img_cache[i].busy = false;
            img_lock_give();
            return;
        }
    img_lock_give();
    slimcc_free_image(img);
}

// 鍵（パスとオプション）とフォルダ（絶対パス・小文字）。作れなければ false
static bool img_key(const char *path, const char *const *flags, char *key, size_t ksz, char *root, size_t rsz)
{
    char abs[512];
    if (!path || !realpath(path, abs))
        return false;
    struct stat st;
    if (stat(abs, &st) != 0)
        return false;
    size_t n = (size_t)snprintf(key, ksz, "%s", abs);
    for (int i = 0; flags && flags[i] && n < ksz; i++)
        n += (size_t)snprintf(key + n, ksz - n, "\x1f%s", flags[i]);
    if (n >= ksz)
        return false;
    snprintf(root, rsz, "%s", abs);
    if (!S_ISDIR(st.st_mode)) {
        char *slash = strrchr(root, '/');
        if (slash && slash != root)
            *slash = '\0';
    }
    for (char *p = root; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return true;
}

// 実行ファイル path を動かす（make・cc -o が作ったもの。Linux と同じ）
int slimcc_run_program(const char *path, int argc, char **argv)
{
    if (!slimcc_is_executable(path)) {
        fprintf(stderr, "%s: not an executable (make it with make or cc -o)\n", path);
        return -1;
    }
    AppMark start;
    app_mark(&start);
    SlimccImage *img = load_executable(path);
    app_slow_log("start", path, &start);
    if (!img)
        return -1;
    return run_image(img, path, argc, argv);
}

// run: ソース（.c・フォルダ）か src の文字列をその場でコンパイルして動かす（ファイルは作らない。2 回目からは
// コンパイルのキャッシュ /var/cache/slimcc で速い。2026-11-19 に戻した。tcc -run と同じ使い方）。
// path が実行ファイルなら、それを動かす
int slimcc_run_source(const char *path, const char *src, const char *const *flags, int argc, char **argv)
{
    if (path && !src && slimcc_is_executable(path))
        return slimcc_run_program(path, argc, argv);
    AppMark start;
    app_mark(&start);
    // つないだ結果の覚え（外側のアプリのときだけ）
    char key[1024], root[512];
    bool cacheable = !src && !app_is_running() && img_key(path, flags, key, sizeof key, root, sizeof root);
    SlimccImage *img = cacheable ? img_take(key) : nullptr;
    if (img) {
        snprintf(slow_phases, sizeof slow_phases, "reused the linked program; ");
    } else {
        unsigned gen = cache_src_gen();     // つなぐ前（間にファイルが変われば、次は使わない）
        int n;
        progress_begin(src ? nullptr : path);
        SlimccObj **objs = compile_program(path, src, flags, true, &n);
        uint64_t t1 = timer_now_us();
        progress_end();
        bool shown = atomic_load(&prog.shown);
        if (objs && shown) {
            atomic_store(&prog.done, n);
            progress_write("linking");
        }
        img = objs ? link_objs(objs, n) : nullptr;
        if (shown)
            fputs("\r\033[K", stderr);
        uint64_t t2 = timer_now_us();
        if (objs)
            free_objs(objs, n);
        uint64_t t3 = timer_now_us();
        if (img && cacheable)
            img_put(key, root, gen, img);
        snprintf(slow_phases, sizeof slow_phases, "compile or cache %llu ms, link %llu ms, free %llu ms; ",
                 (unsigned long long)((t1 - start.us) / 1000), (unsigned long long)((t2 - t1) / 1000),
                 (unsigned long long)((t3 - t2) / 1000));
    }
    app_slow_log("start", path, &start);
    slow_phases[0] = '\0';
    if (!img)
        return -1;
    return run_image(img, path ? path : "-", argc, argv);
}

static int run_image(SlimccImage *img, const char *path, int argc, char **argv)
{
    bool nested = app_is_running();

    app_main_t entry = (app_main_t)slimcc_symbol(img, "main");
    if (!entry) {
        fprintf(stderr, "slimcc: main() がありません\n");
        img_release(img);
        return -1;
    }

    // アプリが led_set を使うと OS の点滅が止まるので、終わったら元に戻す
    unsigned blink = led_get_blink();
    int owner = nested ? -app_depth() : 0;  // このアプリが開くファイル・接続の持ち主（app_owner。os/kernel/app.c）

    if (nested && !curses_push()) {
        fprintf(stderr, "slimcc: too many nested programs\n");
        img_release(img);
        return -1;
    }
    if (nested)
        termios_push();
    PosixApp outer_posix;
    // アプリが chdir しても、終わったら元に戻す（Linux で、子のプロセスの chdir がシェルに残らないのと同じ）
    char cwd[512];
    bool have_cwd = getcwd(cwd, sizeof cwd) != nullptr;
    int status;
    fflush(stdout);
    posix_app_begin(&outer_posix);
    // 前のアプリが stdin に押し戻した文字（scanf などの ungetc）と EOF の印を消す（Linux では、プログラムごとに
    // stdin は新しい。消さないと、前のアプリが読み残した文字が次のアプリの最初の文字になる）
    stdin->unget = 0;
    clearerr(stdin);
    if (!malloc_app_begin()) {      // アプリの確保はアプリ用のヒープから（os/libc/malloc.c）
        fprintf(stderr, "slimcc: too many nested programs\n");
        posix_app_end(&outer_posix);
        curses_pop();
        termios_pop();
        img_release(img);
        return -1;
    }
    if (!nested)
        pmu_app_begin();    // perf の間なら、アプリの実行中だけを数える（os/kernel/pmu.c。外側のアプリだけ）
    int my_depth = app_depth() + 1;
    fault_clear();
    enum app_result result = app_call(entry, argc, argv, &status);
    pthread_app_end(my_depth);  // このアプリのスレッドの記録を消す（コアは app_call が止めた）
    if (!nested)
        pmu_app_end();
    malloc_app_stop();
    if (pmu_recording() && !nested)
        name_samples(img);
    // 止められたときの終了の値は、Linux のシェルと同じ 128 + シグナルの番号（Ctrl-C は SIGINT で 130、Ctrl-\ は
    // SIGQUIT で 131、例外は SIGSEGV で 139）。timeout の期限は 124（Linux の timeout と同じ）
    switch (result) {
    case APP_RETURNED:
    case APP_EXITED:
        break;
    case APP_FAULTED: {
        printf("\n*** program stopped by exception\n");
        char where[96];
        print_fault(img, where, sizeof where);
        klog_printf("app %s: stopped by exception%s%s", path ? path : "-", where[0] ? " at " : "", where);
        status = 128 + 11;
        break;
    }
    case APP_INTERRUPTED: {
        curses_app_exit();
        printf("\n%s\n", uart_interrupt_key_name());
        char where[96];
        print_fault(img, where, sizeof where);
        status = 128 + (uart_interrupt_is_quit() ? 3 : 2);
        break;
    }
    case APP_TIMEDOUT:
        curses_app_exit();
        status = 124;
        break;
    }
    if (nested) {
        curses_pop();       // 子の curses を片付けて、外側のアプリのものに戻す（os/curses/curses.c）
        termios_pop();      // 外側のアプリの端末の設定に戻す（os/libc/termios.c）
    } else {
        curses_app_exit();  // endwin を呼ばずに終わったとき、端末を元に戻す（os/curses/curses.c）
        termios_reset();    // 端末の設定（tcsetattr）を既定に戻す（os/libc/termios.c。uart_set_raw も）
    }
    char now[512];
    if (have_cwd && (!getcwd(now, sizeof now) || strcmp(now, cwd) != 0))
        chdir(cwd);

    // アプリが開いたままのファイルを閉じ、コード領域を解放する。アプリが malloc したメモリは、
    // 最後にアプリ用のヒープごと捨てる（それまでに、アプリ用のものを持っているカーネルの後始末を済ませる）
    vfs_close_owner(owner); // サービス（os/kernel/svc.c）と、外側のアプリが開いたものは閉じない
    AppMark end;
    app_mark(&end);
    vfs_sync(true);         // アプリが書いたものを SD カードに書き出す
    app_slow_log("end", path, &end);
    if (!nested)
        klog_file_sync(true);   // アプリの例外などの記録を /var/log/dmesg に（os/kernel/klog_file.c）
    socket_close_owner(owner);  // ソケット（os/libc/socket.c）を先に（中の TCP・UDP のハンドルも閉じる）
    pipe_close_owner(owner);    // pipe（os/libc/pipe.c）
    mman_close_owner(owner);    // mmap（os/libc/mman.c）
    tcp_api_close_owner(owner);
    udp_api_close_owner(owner);
    led_set_blink(blink);
    img_release(img);
    malloc_app_discard();
    posix_app_end(&outer_posix);
    return status;
}
