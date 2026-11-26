// シェル: 1 行ずつコマンドを読んで実行する
// C のプログラムは run（ファイル）または cc（その場で入力）で、slimcc-a64 でコンパイルして実行する。

// カーネルは -std=c23（ISO）でビルドするので、POSIX の宣言（strdup、strcasecmp など）を見えるようにする
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <fnmatch.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <time.h>
#include "../libc/heap.h"
#include "shell.h"
#include "../slimcc/makefile.h"
#include "../libc/env.h"
#include "../drivers/bcm2837.h"
#include "../drivers/mmio.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../drivers/mailbox.h"
#include "../drivers/led.h"
#include "../drivers/usb.h"
#include "../net/net.h"
#include "../kernel/svc.h"
#include "../kernel/contap.h"
#include "../kernel/rexec.h"
#include "../kernel/tryboot.h"
#include "../kernel/watchdog.h"
#include "../kernel/smp.h"
#include "../kernel/klog_file.h"
#include "../libc/console.h"
#include "../kernel/app.h"
#include "../kernel/sandbox.h"
int sandbox_cmd(int argc, char **argv);    // os/slimcc/sandbox_cmd.c
#include "../kernel/lock.h"
#include "../kernel/sysreg.h"
#include "../net/tcp_api.h"
#include "../drivers/sd.h"
#include "../drivers/sd_backend.h"
#include "../fs/writeback.h"
#include "../kernel/memory.h"
#include "../fs/vfs.h"
#include "../slimcc/cmd.h"
#include "../slimcc/run.h"
#include "../slimcc/cache.h"
#include "lineedit.h"
#include "wifi_cmd.h"
#include "../libc/console.h"
#include "../kernel/term.h"
#include "../kernel/clock.h"
#include "../kernel/klog.h"
#include "../api/include/glob.h"
#include "../curses/curses_os.h"

#define LINE_MAX_LEN    1024
#define TEXT_MAX        8192    // 続きの行をつないだ文の長さ
#define SOURCE_MAX      (256 * 1024)

#define READ_EOF        LINE_EDIT_EOF       // 行頭で Ctrl-D
#define READ_CANCEL     LINE_EDIT_CANCEL    // Ctrl-C

// "." だけの行か Ctrl-D までを読み、malloc した文字列で返す
static char *read_text(void)
{
    char *text = malloc(SOURCE_MAX);
    if (!text) {
        printf("out of memory\n");
        return nullptr;
    }
    size_t len = 0;
    char line[LINE_MAX_LEN];
    printf("(\".\" だけの行か Ctrl-D で終わり、Ctrl-C で取り消し)\n");
    for (;;) {
        int n = line_edit("> ", line, sizeof(line), nullptr, false);
        if (n == READ_CANCEL) {
            free(text);
            return nullptr;
        }
        if (n == READ_EOF || strcmp(line, ".") == 0)
            break;
        if (len + n + 2 > SOURCE_MAX) {
            printf("too long\n");
            break;
        }
        memcpy(text + len, line, n);
        len += n;
        text[len++] = '\n';
    }
    text[len] = '\0';
    return text;
}

// エラーは stderr へ（リダイレクトしても画面に出る。GNU と同じ）
static void report_errno(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
}

// --- コマンド ---------------------------------------------------------

static int cmd_help(int argc, char **argv);
static int cmd_which(int argc, char **argv);
static int cmd_export(int argc, char **argv);
static const char *svar_get(const char *name);
static void svar_unset(const char *name);
static void func_unset(const char *name);
static int cmd_env(int argc, char **argv);

static int cmd_cd(int argc, char **argv)
{
    if (chdir(argc > 1 ? argv[1] : "/") != 0) {
        report_errno(argc > 1 ? argv[1] : "/");
        return 1;
    }
    return 0;
}

static int cmd_pwd(int, char **)
{
    char buf[LINE_MAX_LEN];
    if (!getcwd(buf, sizeof(buf))) {
        report_errno("pwd");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}

static int cmd_write(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: write FILE\n");
        return 1;
    }
    char *text = read_text();
    if (!text)
        return 1;   // 取り消し
    FILE *f = fopen(argv[1], "w");
    if (!f) {
        report_errno(argv[1]);
        free(text);
        return 1;
    }
    fputs(text, f);
    fclose(f);
    printf("%zu bytes written\n", strlen(text));
    free(text);
    return 0;
}

// history [-c]: 履歴の一覧（/.shell_history）。-c で消す
static int cmd_history(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        line_edit_history_clear();
        return 0;
    }
    int n = line_edit_history_count();
    for (int i = 0; i < n; i++)
        printf("%5d  %s\n", i + 1, line_edit_history_get(i));
    return 0;
}

// .monocs-make を前の中身に戻す（old が NULL なら消す）。old は解放する
static void make_restore(const char *path, char *old)
{
    if (old) {
        FILE *f = fopen(path, "w");
        if (f) {
            fputs(old, f);
            fclose(f);
        }
    } else {
        remove(path);
    }
    free(old);
}

// src（.c かフォルダ）をコンパイルして、つなげるかまで確かめ、キャッシュに入れる（実行ファイルは作らない。Monocs は
// ソースのまま動かすので、make は「作り方を選ぶ・誤りを先に見る・次の起動を速くする」だけ。2026-11-19）
static int make_build(const char *name, const char *src, int nsrcs)
{
    printf("make: compiling '%s' (%d file%s)\n", name, nsrcs, nsrcs == 1 ? "" : "s");
    SlimccImage *img = slimcc_load_program(src);
    if (!img) {
        fprintf(stderr, "make: *** [%s] Error 1\n", name);
        return 2;
    }
    slimcc_free_image(img);
    printf("make: '%s' is ready (run it with ./%s)\n", name, name);
    return 0;
}

// GNU make の暗黙の規則と同じく、目標 NAME に NAME.c があれば、それをコンパイルする（キャッシュに入れる。
// ./NAME で NAME.c が動く）。NAME.c がなければ -1
static int make_implicit(const char *dir, const char *name)
{
    char src[600];
    struct stat st;
    snprintf(src, sizeof src, "%s%s%s.c", dir, strcmp(dir, "/") ? "/" : "", name);
    if (strchr(name, '/') || strchr(name, '.') || stat(src, &st) != 0 || S_ISDIR(st.st_mode))
        return -1;
    return make_build(name, src, 1);
}

// make [-C DIR] [目標] [変数=値 ...]（2026-11-16）: Monocs は make のコマンドを動かさない（Makefile を読んで、プログラムと
// その .c・オプションを決めるだけ。os/slimcc/makefile.c）。make は、選んだ目標と 変数=値 をフォルダの .monocs-make に
// 覚え（引数なしなら忘れる）、その選び方でプログラムを決めて、コンパイルまで済ませる（誤りはここで出る。結果は
// コンパイルのキャッシュに残る）。その後 ./プログラム・run DIR・名前だけ（PATH のフォルダなら）で、その選び方で動く。
// Linux で make posix してから ./lua するのと同じ流れ。make clean は覚えたものを忘れる
static int cmd_make(int argc, char **argv)
{
    char dir[512];
    if (!getcwd(dir, sizeof dir))
        return 2;
    char line[1024];        // 覚える引数（引用符で囲む）
    size_t ln = 0;
    line[0] = '\0';
    const char *first_target = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-C") && i + 1 < argc) {
            const char *d = argv[++i];
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s%s%s", d[0] == '/' ? "" : dir, d[0] == '/' || !strcmp(dir, "/") ? "" : "/", d);
            snprintf(dir, sizeof dir, "%s", tmp);
            continue;
        }
        if (argv[i][0] == '-')
            continue;       // -j・-s などは使わない
        if (!strchr(argv[i], '=') && !first_target)
            first_target = argv[i];
        // 引用符で囲んで覚える（値に空白があってもよい。' は '\''）
        if (ln && ln + 1 < sizeof line)
            line[ln++] = ' ';
        if (ln + 1 < sizeof line)
            line[ln++] = '\'';
        for (const char *c = argv[i]; *c && ln + 5 < sizeof line; c++) {
            if (*c == '\'') {
                memcpy(line + ln, "'\\''", 4);
                ln += 4;
            } else {
                line[ln++] = *c;
            }
        }
        if (ln + 1 < sizeof line)
            line[ln++] = '\'';
        line[ln] = '\0';
    }
    char path[600];
    snprintf(path, sizeof path, "%s%s" MAKE_ARGS_FILE, dir, strcmp(dir, "/") ? "/" : "");
    MakeInfo mi;
    if (!make_read(dir, &mi)) {
        // Makefile がない: 目標ごとに NAME.c（暗黙の規則）。目標がなければ GNU make と同じく誤り
        bool any_target = false;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-C")) {
                i++;
                continue;
            }
            if (argv[i][0] == '-' || strchr(argv[i], '='))
                continue;
            any_target = true;
            int r = make_implicit(dir, argv[i]);
            if (r < 0) {
                fprintf(stderr, "make: *** No rule to make target '%s'.  Stop.\n", argv[i]);
                return 2;
            }
            if (r)
                return r;
        }
        if (any_target)
            return 0;
        fprintf(stderr, "make: *** No targets specified and no makefile found.  Stop.\n");
        return 2;
    }
    if (first_target && (!strcmp(first_target, "clean") || !strcmp(first_target, "distclean"))) {
        // Monocs は Makefile のコマンドを動かさないので、clean は「作った実行ファイルと、選んだ作り方を消す」
        char prog[600];
        if (mi.target[0]) {
            snprintf(prog, sizeof prog, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", mi.target);
            if (slimcc_is_executable(prog) && remove(prog) == 0)
                printf("rm -f %s\n", mi.target);
        }
        make_free(&mi);
        remove(path);
        return 0;
    }
    make_free(&mi);
    // 前の .monocs-make（うまくいかなければ戻す）
    char *old = nullptr;
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[1024];
        size_t k = fread(buf, 1, sizeof buf - 1, f);
        buf[k] = '\0';
        old = strdup(buf);
        fclose(f);
    }
    if (ln) {
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    } else {
        remove(path);
    }
    make_read(dir, &mi);
    bool ok = mi.target[0] != '\0';
    char target[256];
    snprintf(target, sizeof target, "%s", mi.target);
    if (!ok && first_target && !strchr(line, '=')) {
        // Makefile にその目標の規則がなければ、暗黙の規則（NAME.c）。GNU make と同じ
        make_free(&mi);
        make_restore(path, old);
        int r = make_implicit(dir, first_target);
        if (r >= 0)
            return r;
        fprintf(stderr, "make: *** No rule to make target '%s'.  Stop.\n", first_target);
        return 2;
    }
    if (!ok) {
        make_free(&mi);
        make_restore(path, old);
        if (first_target)
            fprintf(stderr, "make: *** No rule to make target '%s'.  Stop.\n", first_target);
        else
            fprintf(stderr, "make: *** no program found in the Makefile (Monocs reads the Makefile; it does not run its commands).  Stop.\n");
        return 2;
    }
    // 前に make が作った実行ファイル（2026-11-17〜19 の形）が残っていれば消す（./prog でソースより先に動いてしまう）
    char out[600];
    snprintf(out, sizeof out, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", target);
    if (slimcc_is_executable(out))
        remove(out);
    int r = make_build(target, dir, mi.nsrcs);
    make_free(&mi);
    if (r) {
        make_restore(path, old);    // うまくいかなかった選び方は覚えない
        return r;
    }
    free(old);
    return 0;
}

static int sdtest_fn(void *arg)
{
    return sd_selftest(*(int *)arg);
}

static int sdtest_hs_fn(void *arg)
{
    return sd_selftest_hs(*(int *)arg);
}

// SDHOST のドライバを、区画の外の空いているセクタで試す（os/drivers/sd_test.c）。
// sdtest hs [N]: High Speed（50MHz）を、SDHOST のまま区画の最後の空きクラスタで試す
// sbtest（2026-11-26。開発用）: サンドボックス（os/kernel/sandbox.c）の段階 2 を試す。小さな機械語を EL0 で動かし、
// 終わる・OS のメモリ・OS のコード・周辺機器・特権の命令・自分のコードへの書き込み・回り続ける（時間の上限）で戻るか
static int cmd_sbtest(int, char **)
{
    static const uint32_t exit42[] = { 0xD2800540, 0xD4000001 };               // mov x0, #42; svc #0
    static const uint32_t stack[] = { 0xD28000E0, 0xF81F0FE0, 0xD2800000,     // mov x0, #7; str x0, [sp, #-16]!; mov x0, #0
                                      0xF84107E0, 0xD4000001 };                // ldr x0, [sp], #16; svc #0
    static const uint32_t rd_kernel[] = { 0xD2A00101, 0xF9400020, 0xD4000001 }; // mov x1, #0x80000; ldr x0, [x1]
    static const uint32_t run_kernel[] = { 0xD2A00101, 0xD61F0020 };           // mov x1, #0x80000; br x1
    static const uint32_t rd_gpio[] = { 0xD2A7E401, 0xB9400020, 0xD4000001 };   // mov x1, #0x3F200000; ldr w0, [x1]
    static const uint32_t priv[] = { 0xD50342DF, 0xD4000001 };                 // msr daifset, #2
    static const uint32_t wr_code[] = { 0x10000001, 0xF9000020, 0xD4000001 };  // adr x1, .; str x0, [x1]
    static const uint32_t bad_svc[] = { 0xD2800C68, 0xD4000001 };              // mov x8, #99; svc #0（ない番号）
    static const uint32_t loop[] = { 0x14000000 };                             // b .
    static const struct { const char *name; const uint32_t *code; size_t n; enum sandbox_result want; int64_t st; } t[] = {
        { "exit 42", exit42, 2, SANDBOX_EXITED, 42 },
        { "use the stack", stack, 5, SANDBOX_EXITED, 7 },
        { "read kernel memory", rd_kernel, 3, SANDBOX_FAULTED, 0 },
        { "run kernel code", run_kernel, 2, SANDBOX_FAULTED, 0 },
        { "read GPIO registers", rd_gpio, 3, SANDBOX_FAULTED, 0 },
        { "privileged instruction", priv, 2, SANDBOX_FAULTED, 0 },
        { "write own code", wr_code, 3, SANDBOX_FAULTED, 0 },
        { "unknown system call", bad_svc, 2, SANDBOX_FAULTED, 0 },
        { "endless loop (timeout 300 ms)", loop, 1, SANDBOX_TIMEDOUT, 0 },
    };
    int ng = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int64_t st = -1;
        uint64_t t0 = timer_now_us();
        enum sandbox_result r = sandbox_run(t[i].code, t[i].n, 300, &st);
        bool ok = r == t[i].want && (r != SANDBOX_EXITED || st == t[i].st);
        ng += !ok;
        printf("%s: %s (result %d, status %lld, %llu us)\n", ok ? "ok" : "NG", t[i].name, (int)r, (long long)st,
               (unsigned long long)(timer_now_us() - t0));
    }
    printf(ng ? "sbtest: %d NG\n" : "sbtest: all ok\n", ng);
    return ng ? 1 : 0;
}

static int cmd_sdtest(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "hs")) {
        int n = argc > 2 ? atoi(argv[2]) : 1;
        if (n < 1)
            n = 1;
        return vfs_exclusive(sdtest_hs_fn, &n);
    }
    int rounds = argc > 1 ? atoi(argv[1]) : 1;
    if (rounds < 1)
        rounds = 1;
    sdhost_disable_fixes = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 0) : 0;
    int r = vfs_exclusive(sdtest_fn, &rounds);
    sdhost_disable_fixes = 0;
    return r;
}

// mem [-r]: メモリ・ファイルのキャッシュ・SD カードの様子。-r は「いちばん多かったとき」を今の量に戻す
static int cmd_mem(int argc, char **argv)
{
    // sbrk で切り出した量（減らない）と、そのうち malloc で使用中の量
    struct mallinfo mi = mallinfo();
    printf("heap %#lx-%#lx (%lu KB)\n",
           (unsigned long)memory_heap_start(), (unsigned long)memory_heap_end(),
           (unsigned long)((memory_heap_end() - memory_heap_start()) / 1024));
    printf("  sbrk %zu KB, in use %zu KB, free %zu KB\n",
           memory_heap_used() / 1024, mi.uordblks / 1024, mi.fordblks / 1024);
    // 使用中の量のいちばん多かったとき（起動してから、または mem -r から。2026-11-20）。多く使うコンパイルは dmesg の「cc:」
    size_t kp, ap, tp;
    malloc_peak(&kp, &ap, &tp, argc > 1 && !strcmp(argv[1], "-r"));
    printf("  peak in use %zu KB (kernel %zu KB, app %zu KB)%s\n", tp / 1024, kp / 1024, ap / 1024,
           argc > 1 && !strcmp(argv[1], "-r") ? " (reset now)" : "");
    size_t app_arena, app_leaked;
    malloc_last_app(&app_arena, &app_leaked);
    printf("  last app heap %zu KB, not freed %zu KB (discarded when the app ended)\n",
           app_arena / 1024, app_leaked / 1024);
    struct vfs_cache_info ci;
    vfs_cache_info(&ci);
    printf("  file cache %zu KB (%u files), hits %llu, misses %llu; missing paths %u (hits %llu)\n",
           ci.bytes / 1024, ci.files, (unsigned long long)ci.hits, (unsigned long long)ci.misses,
           ci.missing, (unsigned long long)ci.missing_hits);
    struct sd_stats st;
    sd_get_stats(&st);
    char sdname[32];
    if (sdhost_clock_hz())
        snprintf(sdname, sizeof sdname, "%s %u MHz", sd_controller(), (unsigned)(sdhost_clock_hz() / 1000000));
    else
        snprintf(sdname, sizeof sdname, "%s", sd_controller());
    printf("  sd (%s): read %llu cmds %llu KB %llu ms, write %llu cmds %llu KB %llu ms\n", sdname,
           (unsigned long long)st.reads, (unsigned long long)st.read_sectors / 2,
           (unsigned long long)st.read_us / 1000, (unsigned long long)st.writes,
           (unsigned long long)st.write_sectors / 2, (unsigned long long)st.write_us / 1000);
    printf("      command %llu ms, wait ready %llu ms, pio %llu ms, wait done %llu ms\n",
           (unsigned long long)st.cmd_us / 1000, (unsigned long long)st.ready_us / 1000,
           (unsigned long long)st.pio_us / 1000, (unsigned long long)st.done_us / 1000);
    unsigned dirty;
    uint64_t flushes, rewrites;
    diskio_writeback_info(&dirty, &flushes, &rewrites);
    printf("      write-back: %u sectors pending, %llu flushes, %llu rewrites merged; errors: read %llu, write %llu\n",
           dirty, (unsigned long long)flushes, (unsigned long long)rewrites,
           (unsigned long long)st.read_errors, (unsigned long long)st.write_errors);
    return 0;
}

// ボーレートを変える。新しい速さで 10 秒以内に y が届かなければ元に戻す
// （端末の速さが合わずに通信できなくなっても、戻れるように）
static int cmd_baud(int argc, char **argv)
{
    if (argc < 2) {
        printf("%lu bps\n", (unsigned long)uart_baud());
        return 0;
    }
    uint32_t old = uart_baud();
    uint32_t baud = (uint32_t)strtoul(argv[1], nullptr, 10);
    printf("switching to %lu bps. type y within 10 s to keep it\n", (unsigned long)baud);
    uart_flush();
    if (!uart_set_baud(baud)) {
        printf("baud: unsupported rate\n");
        return 1;
    }
    uint64_t end = timer_now_us() + 10000000;
    while (timer_now_us() < end) {
        if (uart_has_input() && uart_getc() == 'y') {
            printf("\n%lu bps\n", (unsigned long)baud);
            return 0;
        }
        uart_wait_input();
    }
    uart_set_baud(old);
    printf("\nbaud: no answer, back to %lu bps\n", (unsigned long)old);
    return 1;
}

// CRC-32（IEEE 802.3。zlib の crc32 と同じ値）
static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// UART か USB でファイルを受け取る（Pi 5 側は tools/uart/nos.py）。
//   recv PATH SIZE CRC32（16 進） [BLOCK]
// 「recv: ready」の後、送る側は BLOCK バイト（既定 4096、最大 32768）ずつ送り、そのたびに「+」を待つ
// （受信バッファ 64KB があふれないように）。全体をメモリに受け取り、CRC32 が合えば PATH.tmp に書いてから
// PATH に置き換える
#define RECV_BLOCK      4096
#define RECV_BLOCK_MAX  32768
#define RECV_TIMEOUT_US 3000000

static int cmd_recv(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        printf("usage: recv PATH SIZE CRC32 [BLOCK]\n");
        return 1;
    }
    size_t block = argc == 5 ? strtoul(argv[4], nullptr, 10) : RECV_BLOCK;
    if (block == 0 || block > RECV_BLOCK_MAX) {
        printf("usage: recv PATH SIZE CRC32 [BLOCK]（BLOCK は 1〜%d）\n", RECV_BLOCK_MAX);
        return 1;
    }
    const char *path = argv[1];
    size_t size = strtoul(argv[2], nullptr, 10);
    uint32_t want = (uint32_t)strtoul(argv[3], nullptr, 16);
    unsigned char *buf = malloc(size ? size : 1);
    if (!buf) {
        printf("recv: out of memory\n");
        return 1;
    }
    uint64_t dropped = uart_rx_dropped();
    printf("recv: ready\n");
    size_t off = 0;
    while (off < size) {
        size_t n = size - off < block ? size - off : block;
        size_t got = uart_read(buf + off, n, RECV_TIMEOUT_US);
        off += got;
        if (got < n) {
            printf("recv: timeout at %zu of %zu bytes\n", off, size);
            free(buf);
            return 1;
        }
        printf("+\n");
    }
    uint32_t crc = crc32_update(0, buf, size);
    if (crc != want || uart_rx_dropped() != dropped) {
        printf("recv: crc mismatch (%08lx, expected %08lx, dropped %llu)\n", (unsigned long)crc,
               (unsigned long)want, (unsigned long long)(uart_rx_dropped() - dropped));
        free(buf);
        return 1;
    }

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    ssize_t w = fd >= 0 ? write(fd, buf, size) : -1;
    int c = fd >= 0 ? close(fd) : -1;
    free(buf);
    if (w != (ssize_t)size || c != 0) {
        printf("recv: cannot write %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return 1;
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        printf("recv: cannot rename %s: %s\n", tmp, strerror(errno));
        return 1;
    }
    printf("recv: ok %zu bytes\n", size);
    return 0;
}

// exit・logout: telnet・ssh でつないでいるとき、その接続を閉じる（閉じるのはサービス。os/kernel/contap.c）。
// シリアルのコンソール（USB・UART）には終わる先がないので、何もしない

static int last_status;     // $?（最後に動かしたパイプライン・コマンドの置き換えの終了の値）
static bool ret_pending;        // return が呼ばれた
static int ret_status;

static int cmd_exit(int argc, char **argv)
{
    if (rexec_running()) {
        // ssh の exec の行の中: 行を終わらせ、その終了の値にする（ssh host 'exit 3' は 3。bash と同じ）
        ret_status = argc > 1 ? atoi(argv[1]) & 0xFF : last_status;
        ret_pending = true;
        return ret_status;
    }
    if (contap_request_hangup())
        printf("logout\n");
    else
        printf("exit: this is the system console (exit closes a telnet or ssh session)\n");
    return 0;
}

// ウォッチドッグで再起動する（出典: Linux の drivers/watchdog/bcm2835_wdt.c の bcm2835_restart）
// reboot tryboot: 次の 1 回だけ、ファームウェアに config.txt の代わりに tryboot.txt を読ませる（Linux の reboot "0 tryboot"）
// run FILE.c|DIR|- [引数]: その場でコンパイルして動かす（ファイルは作らない。キャッシュで 2 回目から速い。2026-11-19 に戻した）
static int cmd_run(int argc, char **argv)
{
    // -std=c17 などで規格を選ぶ（既定は gnu23）。-D・-U・-I・-l（/lib のライブラリ）も（ファイルの前に）
    const char *flags[argc + 1];    // 引数の数だけ（前は 31 個で、それより後のオプションをファイル名と取り違えた）
    int nflags = 0;
    while (argc >= 2 && argv[1][0] == '-' && argv[1][1] &&
           (strchr("sDUIl", argv[1][1]) || !strcmp(argv[1], "-fopenmp") ||
            !strcmp(argv[1], "-fno-schedule-insns2") || !strcmp(argv[1], "-fschedule-insns2") ||
            !strcmp(argv[1], "-fno-unroll-loops") || !strcmp(argv[1], "-funroll-loops") ||
            !strcmp(argv[1], "-fno-strict-aliasing") || !strcmp(argv[1], "-fstrict-aliasing"))) {
        flags[nflags++] = argv[1];
        argc--;
        argv++;
    }
    flags[nflags] = nullptr;
    if (argc < 2) {
        printf("usage: run [-std=STD] [-DNAME[=VALUE]] [-UNAME] [-IDIR] [-lLIB] [-fopenmp] [-fno-schedule-insns2] [-fno-unroll-loops] [-fno-strict-aliasing] FILE[.c]|DIR|- [ARGS...]\n");
        return 1;
    }
    if (!strcmp(argv[1], "-")) {
        // run -: キーボードから打ち込んだ C をその場で実行する（2026-11-07 までのシェルの cc）
        char *src = read_text();
        if (!src)
            return 1;
        int r = slimcc_run_source("-", src, flags, argc - 1, argv + 1);
        free(src);
        return r;
    }
    // run NAME: NAME（ファイル・フォルダ）がなく NAME.c があれば、それを（2026-11-19。名前で動かすときと同じ）
    struct stat st;
    char withc[LINE_MAX_LEN];
    if (stat(argv[1], &st) != 0) {
        snprintf(withc, sizeof withc, "%s.c", argv[1]);
        if (stat(withc, &st) == 0 && S_ISREG(st.st_mode))
            argv[1] = withc;
    }
    // プログラムには argv[1]（ファイル名）を argv[0] として渡す
    return slimcc_run_source(argv[1], nullptr, flags, argc - 1, argv + 1);
}

// コンパイルのキャッシュ（os/slimcc/cache.c）: 数・大きさ・使えた回数。clear で消す
static bool find_app(const char *name, char *path, size_t size);

// cache clear NAME|PATH: そのプログラムのキャッシュだけ消す（2026-11-21）。名前はシェルが動かすものと同じ探し方
// （PATH）、/ を含むか .c で終わるものはパス（ファイル・フォルダ）
static int cache_clear_one(const char *arg)
{
    char found[PATH_MAX], abs[PATH_MAX];
    struct stat st;
    size_t n = strlen(arg);
    bool as_path = strchr(arg, '/') || (n > 2 && !strcmp(arg + n - 2, ".c"));
    if (as_path && stat(arg, &st) == 0)
        snprintf(found, sizeof found, "%s", arg);
    else if (!find_app(arg, found, sizeof found)) {
        fprintf(stderr, "cache: %s: not found\n", arg);
        return 1;
    }
    if (!realpath(found, abs) || stat(abs, &st) != 0) {
        fprintf(stderr, "cache: %s: not found\n", arg);
        return 1;
    }
    if (!S_ISDIR(st.st_mode) && slimcc_is_executable(abs)) {
        fprintf(stderr, "cache: %s: an executable (made with cc -o; it has no compile cache)\n", abs);
        return 1;
    }
    int removed = cache_clear_program(abs, S_ISDIR(st.st_mode));
    printf("%s: removed %d cached file%s (%s)\n", arg, removed, removed == 1 ? "" : "s", abs);
    return 0;
}

static int cmd_cache(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        if (argc == 2) {
            cache_clear();
            printf("compile cache cleared\n");
            return 0;
        }
        int r = 0;
        for (int i = 2; i < argc; i++)
            r |= cache_clear_one(argv[i]);
        return r;
    }
    if (argc != 1) {
        printf("usage: cache [clear [NAME|PATH]...]\n");
        return 1;
    }
    int files;
    uint64_t bytes, hits, misses;
    cache_usage(&files, &bytes);
    cache_stats(&hits, &misses);
    printf("compile cache: %d files, %llu KB (/var/cache/slimcc). since boot: %llu used, %llu compiled\n", files,
           (unsigned long long)(bytes / 1024), (unsigned long long)hits, (unsigned long long)misses);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    if (argc > 1) {
        if (strcmp(argv[1], "tryboot") != 0) {
            fprintf(stderr, "usage: reboot [tryboot]\n");
            return 2;
        }
        bool ok = mailbox_set_reboot_flags(1);
        printf("reboot: tryboot flag %s\n", ok ? "set" : "NOT accepted by the firmware");
        if (!ok)
            return 1;
    }
    system_reboot(true);
    return 1;
}

// tryboot [status]: 試しの起動か・残りの秒数・コマンドライン。tryboot confirm: 確かめた（ウォッチドッグを止め、このカーネルで
// 動き続ける）。os/kernel/tryboot.c
static int cmd_tryboot(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "confirm")) {
        if (!tryboot_confirm()) {
            fprintf(stderr, "tryboot: not a trial boot (or already confirmed)\n");
            return 1;
        }
        printf("tryboot: confirmed\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "status") != 0) {
        fprintf(stderr, "usage: tryboot [status|confirm]\n");
        return 2;
    }
    if (tryboot_active())
        printf("trial boot: back to the previous kernel in %d s unless confirmed\n", tryboot_seconds_left());
    else if (tryboot_confirmed())
        printf("trial boot, confirmed\n");
    else
        printf("normal boot\n");
    printf("cmdline: %s\n", tryboot_cmdline());
    return 0;
}

// 表示を送り切ってから、ウォッチドッグでリセットする。halt なら、ファームウェアに「止まる」と知らせる
// （出典: Linux の drivers/watchdog/bcm2835_wdt.c の bcm2835_power_off・__bcm2835_restart）
static void watchdog_reset(bool halt)
{
    if (rexec_running()) {
        // ssh の exec の行なら、表示を相手に届ける（sshd が読み終えた後も、送り出すまでに少しかかる）
        rexec_drain(500);
        uint64_t t = timer_now_us() + 300000;
        while (timer_now_us() < t)
            cpu_idle();
    }
    uart_flush();
    while (mmio_read32(UART0_FR) & (1U << 3))  // 送り終わるまで（FR の BUSY）
        ;
    usb_disconnect();
    klog_set_end(halt ? KLOG_END_POWEROFF : KLOG_END_REBOOT);   // 次の起動で、ふつうに終わったとわかる
    klog_persist_flush();
    if (halt)
        mmio_write32(PM_RSTS, mmio_read32(PM_RSTS) | PM_PASSWORD | PM_RSTS_RASPBERRYPI_HALT);
    mmio_write32(PM_WDOG, PM_PASSWORD | 10);  // 10 / 65536 秒後
    mmio_write32(PM_RSTC, (mmio_read32(PM_RSTC) & PM_RSTC_WRCFG_CLR) | PM_PASSWORD |
                              PM_RSTC_WRCFG_FULL_RESET);
    uint64_t end = timer_now_us() + 1000000;
    while (timer_now_us() < end)
        ;
}

void system_reboot(bool sync)
{
    if (sync) {
        klog_printf("reboot");
        clock_save(true);       // 今の時刻を覚える（次の起動はこの時刻から。os/kernel/clock.c）
        klog_file_sync(true);   // 記録を /var/log/dmesg に書き足す
        vfs_sync(true);     // 遅らせている SD カードへの書き込みを済ませる
    }
    printf("rebooting...\n");
    watchdog_reset(false);
    printf("reboot: watchdog did not reset\n");
}

// 書き出した後、SD カードを独り占めしたまま止める（ほかのコアのサービスなどが、後から書かないように）
static int poweroff_locked(void *)
{
    led_set(false);
    printf("poweroff: SD カードに書き出しました。止まります（数秒たったら電源を切ってかまいません。電源を入れ直すと起動します）\n");
    watchdog_reset(true);
    printf("poweroff: watchdog did not reset\n");
    return 1;
}

// poweroff・halt: Linux と同じく、書き出してから止まる。Raspberry Pi は電源そのものは切れない（ファームウェアが止まった
// まま待つ）。もう一度動かすには電源を入れ直す
static int cmd_poweroff(int, char **)
{
    klog_printf("poweroff");
    clock_save(true);           // 今の時刻を覚える（次の起動はこの時刻から）
    klog_file_sync(true);       // 記録を /var/log/dmesg に書き足す（独り占めの前に）
    return vfs_exclusive(poweroff_locked, nullptr);
}

// watchdog [on|off|status]: 固まったら・止まったら再起動する仕組み（os/kernel/watchdog.c）。off は固まった様子を
// UART やデバッガで見たいときに（次の起動でまた入る）。watchdog test halt|hang は試験用: カーネルを止める（数秒後に
// 再起動）・割り込みを止めて固める（約 16 秒でリセット）。次の起動の dmesg と /var/log/dmesg.1 で確かめる
static int cmd_watchdog(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "on")) {
        watchdog_set(true);
    } else if (argc >= 2 && !strcmp(argv[1], "off")) {
        watchdog_set(false);
    } else if (argc >= 3 && !strcmp(argv[1], "test") && !strcmp(argv[2], "halt")) {
        klog_printf("watchdog test: halting the kernel");
        clock_save(true);
        klog_file_sync(true);
        vfs_sync(true);
        kernel_halt("watchdog test");
    } else if (argc >= 3 && !strcmp(argv[1], "test") && !strcmp(argv[2], "hang")) {
        klog_printf("watchdog test: hanging with interrupts off");
        clock_save(true);
        klog_file_sync(true);
        vfs_sync(true);
        printf("hanging with interrupts off (the watchdog resets in about 16 s)\n");
        fflush(stdout);
        uart_flush();
        __asm__ volatile("msr daifset, #2" ::: "memory");
        for (;;)
            __asm__ volatile("nop");
    } else if (argc >= 2 && strcmp(argv[1], "status") != 0) {
        fprintf(stderr, "usage: watchdog [on|off|status|test halt|test hang]\n");
        return 2;
    }
    printf("watchdog: %s\n", watchdog_enabled() ? "on (restarts about 16 s after a hang, 5 s after a kernel halt)"
                                                  : "off (a hang or a kernel halt stays as it is)");
    return 0;
}

// CPU のサイクルカウンタ（PMCCNTR_EL0）を動かす（ARM ARM D7.10 PMU。PMCR_EL0.E、PMCNTENSET_EL0.C）
static uint64_t read_cycles(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

static void print_mhz(const char *name, uint32_t tag, uint32_t id)
{
    uint32_t v;
    if (mailbox_get_value(tag, id, &v))
        printf("  %-18s %u MHz\n", name, (unsigned)(v / 1000000));
    else
        printf("  %-18s (error)\n", name);
}

// クロックの確認: ファームウェアの申告と、サイクルカウンタで実測した値
// clock: クロック・温度・電圧を出す。clock arm MHz: ARM のクロックをファームウェアに頼む（実験用。config.txt の
// force_turbo=1 のときは変わらない）
static int cmd_clock(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "arm")) {
        uint32_t got = mailbox_set_clock_rate(MBOX_CLOCK_ARM, (uint32_t)atoi(argv[2]) * 1000000U, false);
        printf("clock arm: asked %s MHz, firmware says %u MHz\n", argv[2], (unsigned)(got / 1000000));
    }
    printf("ARM:\n");
    print_mhz("current", MBOX_TAG_GET_CLOCK_RATE, MBOX_CLOCK_ARM);
    print_mhz("measured", MBOX_TAG_GET_CLOCK_RATE_MEASURED, MBOX_CLOCK_ARM);
    print_mhz("min", MBOX_TAG_GET_MIN_CLOCK_RATE, MBOX_CLOCK_ARM);
    print_mhz("max", MBOX_TAG_GET_MAX_CLOCK_RATE, MBOX_CLOCK_ARM);
    printf("CORE (VPU):\n");
    print_mhz("current", MBOX_TAG_GET_CLOCK_RATE, MBOX_CLOCK_CORE);
    print_mhz("measured", MBOX_TAG_GET_CLOCK_RATE_MEASURED, MBOX_CLOCK_CORE);
    print_mhz("max", MBOX_TAG_GET_MAX_CLOCK_RATE, MBOX_CLOCK_CORE);
    uint32_t v;
    if (mailbox_get_value(MBOX_TAG_GET_TEMPERATURE, 0, &v))
        printf("temperature %u.%u C", (unsigned)(v / 1000), (unsigned)(v % 1000 / 100));
    if (mailbox_get_value(MBOX_TAG_GET_MAX_TEMPERATURE, 0, &v))
        printf(" (max %u C)", (unsigned)(v / 1000));
    printf("\n");
    if (mailbox_get_value(MBOX_TAG_GET_THROTTLED, 0, &v))
        printf("throttled %#x\n", (unsigned)v);
    if (mailbox_get_value(MBOX_TAG_GET_VOLTAGE, MBOX_VOLTAGE_CORE, &v))
        printf("core voltage %u.%04u V\n", (unsigned)(v / 1000000), (unsigned)(v % 1000000 / 100));

    // 100ms の間に進んだサイクル数（割り込みを止めて測る）
    __asm__ volatile("msr pmcr_el0, %0; msr pmcntenset_el0, %1; isb" :: "r"(1UL), "r"(1UL << 31));
    __asm__ volatile("msr daifset, #2");
    uint64_t t0 = timer_now_us(), c0 = read_cycles();
    while (timer_now_us() - t0 < 100000)
        ;
    uint64_t t1 = timer_now_us(), c1 = read_cycles();
    __asm__ volatile("msr daifclr, #2");
    printf("cycle counter: %llu MHz\n", (unsigned long long)((c1 - c0) / (t1 - t0)));
    return 0;
}

static int cmd_usb(int, char **)
{
    usb_status();
    net_status();
    tcp_api_status();
    return 0;
}

static int cmd_svc(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "start"))
        return svc_start(argv[2], argc - 2, argv + 2) ? 1 : 0;
    if (argc == 3 && !strcmp(argv[1], "stop"))
        return svc_stop(argv[2]) ? 1 : 0;
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "list"))) {
        svc_list();
        return 0;
    }
    printf("usage: svc [list] | svc start FILE.c|DIR [ARGS] | svc stop NAME\n");
    return 1;
}

static int cmd_sync(int, char **)
{
    vfs_sync(true);
    return 0;
}

// date: 日時の表示と設定（os/kernel/clock.c）。Monocs には時計がないので、nos.py がつなぐたびに Pi 5 の日時を送る
//   date                 今の日時（GNU の date と同じ形）
//   date -s @秒          UNIX 時間で設定（小数も可）
//   date -s '年-月-日 時:分[:秒]'  地方時で設定
//   date -z +0900        タイムゾーン（UTC からの差）
static int cmd_date(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-z") && i + 1 < argc) {
            const char *z = argv[++i];
            int sign = z[0] == '-' ? -1 : 1;
            if (z[0] == '+' || z[0] == '-')
                z++;
            int hhmm = atoi(z);
            if (strlen(z) != 4 || hhmm % 100 >= 60) {
                printf("date: invalid time zone '%s'（+0900 の形）\n", argv[i]);
                return 1;
            }
            clock_set_tz(sign * (hhmm / 100 * 60 + hhmm % 100));
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            const char *v = argv[++i];
            if (v[0] == '@') {
                char *end;
                double sec = strtod(v + 1, &end);
                if (end == v + 1 || *end) {
                    printf("date: invalid date '%s'\n", v);
                    return 1;
                }
                clock_set((int64_t)(sec * 1000000));
            } else {
                // シェルは引用符を外さないので、「年-月-日 時:分」は 2 つの引数に分かれる
                char buf[64];
                snprintf(buf, sizeof(buf), "%s%s%s", v, i + 1 < argc && argv[i + 1][0] != '-' ? " " : "",
                         i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : "");
                if (i + 1 < argc && argv[i + 1][0] != '-')
                    i++;
                for (char *q = buf; *q; q++)
                    if (*q == '\'' || *q == '"')
                        *q = ' ';
                struct tm tm = {0};
                int n = sscanf(buf, " %d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                               &tm.tm_min, &tm.tm_sec);
                if (n < 5) {
                    printf("date: invalid date '%s'（@秒 か '年-月-日 時:分[:秒]'）\n", buf);
                    return 1;
                }
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                tm.tm_isdst = 0;
                clock_set((int64_t)mktime(&tm) * 1000000);
            }
        } else {
            printf("usage: date [-s @秒|'年-月-日 時:分[:秒]'] [-z +0900]\n");
            return 1;
        }
    }
    time_t t = (time_t)(clock_realtime_us() / 1000000);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm);
    printf("%s%s\n", buf, clock_known() ? "" : clock_approx() ? "（推定。前に覚えた時刻から。まだネットワークで合わせていない）"
                                                       : "（日時は未設定。起動を 2026-01-01 として数えている）");
    return 0;
}

static int cmd_uptime(int, char **)
{
    uint64_t now = timer_now_us(), s = now / 1000000;
    // idle: コア 0 が入力などを待って眠っていた（cpu_idle）時間の割合
    printf("%llu:%02llu:%02llu, core 0 idle %llu%%\n", (unsigned long long)(s / 3600),
           (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60),
           (unsigned long long)(now ? timer_idle_us() * 100 / now : 0));
    return 0;
}

// perf（os/shell/perf.c）
int cmd_perf(int argc, char **argv);
static int cmd_unset(int argc, char **argv);
static int cmd_true(int, char **) { return 0; }
static int cmd_false(int, char **) { return 1; }
static int cmd_timeout(int argc, char **argv);
static int cmd_dmesg(int argc, char **argv);
static int cmd_test(int argc, char **argv);
static int cmd_local(int argc, char **argv);
static int cmd_return(int argc, char **argv);
static int cmd_shift(int argc, char **argv);
static int cmd_set(int argc, char **argv);
static int cmd_source(int argc, char **argv);
static int cmd_sh(int argc, char **argv);
static int cmd_break(int argc, char **argv);
static int cmd_continue(int argc, char **argv);

static const struct {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *help;
} commands[] = {
    { "help",   cmd_help,   "コマンドの一覧" },
    { "cd",     cmd_cd,     "cd [DIR]        ディレクトリの移動" },
    { "pwd",    cmd_pwd,    "pwd             現在のディレクトリ" },
    { "write",  cmd_write,  "write FILE      入力した内容をファイルに書く" },
    { "history", cmd_history, "history [-c]    履歴の一覧（-c で消す）" },
    { "which",  cmd_which,  "which NAME...   組み込みのコマンドか、どこのアプリか（PATH）" },
    { "export", cmd_export, "export NAME=VALUE  環境変数（PATH など。残すなら /.profile に書く）" },
    { "env",    cmd_env,    "env [-i] [-u N] [N=V]... [CMD]  環境変数の一覧・変えて動かす" },
    { "unset",  cmd_unset,  "unset NAME...   環境変数を消す" },
    { "true",   cmd_true,   "true / false    何もせず、終了の値 0 / 1 で終わる" },
    { ":",      cmd_true,   nullptr },
    { "false",  cmd_false,  nullptr },
    { "timeout", cmd_timeout, "timeout DURATION CMD...  CMD が DURATION（秒。m・h・d も）より長く動いたら止める（終了の値 124）" },
    { "test",   cmd_test,   "test EXPR / [ EXPR ]  条件（-e -f -d -s -n -z、= !=、-eq -lt など、! -a -o）。真なら 0" },
    { "[",      cmd_test,   nullptr },
    { "break",  cmd_break,  "break [N] / continue [N]  for・while・until の繰り返しを抜ける・次へ" },
    { "continue", cmd_continue, nullptr },
    { "local",  cmd_local,  "local NAME[=値]  関数の中だけの変数" },
    { "return", cmd_return, "return [N]      関数から戻る" },
    { "shift",  cmd_shift,  "shift [N]       位置の引数（$1…）をずらす" },
    { "source", cmd_source, "source FILE [ARG...] / . FILE  ファイルのコマンドを今のシェルで動かす" },
    { ".",      cmd_source, nullptr },
    { "sh",     cmd_sh,     "sh FILE [ARG...] / sh -c CMD  ファイル・文字列のコマンドを別のシェルとして動かす" },
    { "set",    cmd_set,    "set -- ARG...   位置の引数を変える（set だけなら変数の一覧）" },
    { "dmesg",  cmd_dmesg,  "dmesg [-c|-C]   カーネルの記録（起動の表示、例外、Wi-Fi・USB・サービスのできごと）。-c は表示して消す、-C は消す" },
    { "make",   cmd_make,   "make [-C DIR] [目標] [変数=値] Makefile の選び方（make posix など）を覚えてコンパイルする（.monocs-make。make clean で忘れる）" },
    { "cc",     slimcc_cmd, "cc [-o PROG] FILE.c...  実行ファイルを作る（-c なら .o。-I -D -std= -lNAME。--report: 作らずに、使う OS の機能の一覧）" },
    { "ld",     slimld_cmd, "ld -o kernel8.img FILE.o... LIB.a...  リンカ（slimld）。今は Monocs のカーネルを作る専用" },
    { "baud",   cmd_baud,   "baud [BPS]      UART のボーレート（10 秒以内に y で確定。上限 3000000）" },
    { "recv",   cmd_recv,   "recv PATH SIZE CRC32 [BLOCK]  ファイルを受け取る（tools/uart/nos.py から使う）" },
    { "tryboot", cmd_tryboot, "tryboot [status|confirm] 試しの起動の様子・確認（nos.py kernel が使う）" },
    { "watchdog", cmd_watchdog, "watchdog [on|off]  固まったら・止まったら再起動する（off で止まったまま）" },
    { "reboot", cmd_reboot, "reboot [tryboot] 再起動する（tryboot: 次の 1 回だけ tryboot.txt で起動）" },
    { "exit",   cmd_exit,   "exit            telnet・ssh の接続を閉じる（logout も同じ）" },
    { "logout", cmd_exit,   nullptr },
    { "poweroff", cmd_poweroff, "poweroff        SD カードに書き出して止まる（その後、電源を切ってよい。halt も同じ）" },
    { "halt",   cmd_poweroff, nullptr },
    { "mem",    cmd_mem,    "mem [-r]        ヒープ（いちばん多かったときも。-r で戻す）とファイルのキャッシュの使用量" },
    { "wifi",   cmd_wifi,   "wifi [start|scan|connect|disconnect|status]  内蔵 Wi-Fi（connect は /etc/wpa_supplicant.conf か SSID PASS）" },
    { "sandbox", sandbox_cmd, "sandbox FILE.c [ARGS]  サンドボックス（EL0）で動かす（画面に書く・文字列・malloc だけ。OS のメモリ・ファイル・ネットワークは使えない）" },
    { "sbtest", cmd_sbtest, "sbtest          サンドボックス（EL0）の段階 2 を試す（開発用）" },
    { "sdtest", cmd_sdtest, "sdtest [N] | sdtest hs [N]  SD カードの SDHOST のドライバを試す（hs: 50MHz を、区画の最後の空きクラスタで）" },
    { "clock",  cmd_clock,  "clock           CPU のクロック（ファームウェアの値と実測）と温度" },
    { "usb",    cmd_usb,    "usb             USB のコンソール（CDC-ACM）とネットワーク（CDC-ECM、lwIP）の状態" },
    { "run",    cmd_run,    "run FILE[.c]|DIR|- [ARGS]  その場でコンパイルして動かす（ファイルは作らない。.c は省ける。-std= -D -I -l も。- は打ち込んだもの）" },
    { "cache",  cmd_cache,  "cache [clear [NAME|PATH]...]  コンパイルのキャッシュ（/var/cache/slimcc）の量。clear で消す（NAME・PATH ならそのプログラムだけ）" },
    { "svc",    cmd_svc,    "svc [list|start FILE.c|DIR [ARGS]|stop NAME]  サービス（コア 3 で裏で動くプログラム）" },
    { "sync",   cmd_sync,   "sync            遅らせている SD カードへの書き込みを、すぐに済ませる" },
    { "date",   cmd_date,   "date [-s @秒] [-z +0900]  日時（nos.py がつなぐたびに Pi 5 の日時を送る）" },
    { "uptime", cmd_uptime, "uptime          起動からの時間" },
    { "perf",   cmd_perf,   "perf [-e EV,...] [-c] CMD...  CPU の性能カウンタでキャッシュのミスなどを数える（-l で一覧）" },
};

// ---- アプリ（PATH: 既定は /apps:/bin） ----
static bool find_app(const char *name, char *path, size_t size);
// 組み込みのコマンドにない名前は、環境変数 PATH のディレクトリから順に探して run する（Linux と同じ）:
//   名前 → ディレクトリ/名前（ディレクトリ。中の .c をすべて。compile_flags.txt も）か ディレクトリ/名前.c
// 既定の PATH は /apps:/bin（os/libc/syscalls.c）。/bin は Monocs に付いてくるアプリ（リポジトリの bin/）、
// /apps は自分で作ったアプリ（mi など）。/apps が先なので、同じ名前なら自分のものが使われる
// （Linux の /root/apps が /usr/bin より先なのと同じ。ユーザーと合意、2026-11-07）。
// 組み込みのコマンドがいちばん先（同じ名前のアプリは隠れる）。変えるには export PATH=...（/.profile に書けば残る）
#define MAX_APPS        512     // Tab の補完と help に出すコマンドの名前（128 では、それより後を黙って出さなかった）

static char app_names[MAX_APPS][64];
static int napps;

// PATH のディレクトリを順に（区切りは :）。fn が true を返したら止めて true
static bool for_each_path(bool (*fn)(const char *dir, void *arg), void *arg)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/root/apps:/usr/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        char dir[LINE_MAX_LEN];
        if (n > 0 && n < sizeof(dir)) {
            memcpy(dir, path, n);
            dir[n] = '\0';
            if (fn(dir, arg))
                return true;
        }
        path += n + (end ? 1 : 0);
    }
    return false;
}

static bool add_apps_in(const char *dir, void *)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    while ((e = readdir(d)) && napps < MAX_APPS) {
        char name[32];
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.')
            continue;
        if (e->d_type == DT_DIR && n < sizeof(name))
            snprintf(name, sizeof(name), "%s", e->d_name);
        else if (n > 2 && n - 2 < sizeof(name) && !strcmp(e->d_name + n - 2, ".c"))
            snprintf(name, sizeof(name), "%.*s", (int)(n - 2), e->d_name);
        else
            continue;
        bool dup = false;
        for (int i = 0; i < napps && !dup; i++)
            dup = !strcmp(app_names[i], name);
        if (dup)
            continue;
        int i = napps;
        while (i > 0 && strcmp(app_names[i - 1], name) > 0) {
            memcpy(app_names[i], app_names[i - 1], sizeof(app_names[0]));
            i--;
        }
        memcpy(app_names[i], name, sizeof(name));
        napps++;
    }
    closedir(d);
    return false;
}

// PATH のアプリの一覧を読み直す（名前の順。同じ名前は 1 つ）
static void load_apps(void)
{
    napps = 0;
    for_each_path(add_apps_in, nullptr);
}

typedef struct {
    const char *name;
    char *path;
    size_t size;
} FindApp;

static bool find_in(const char *dir, void *arg)
{
    FindApp *f = arg;
    struct stat st;
    snprintf(f->path, f->size, "%s/%s", dir, f->name);
    if (stat(f->path, &st) == 0 && (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && slimcc_is_executable(f->path))))
        return true;    // アプリのフォルダか、実行ファイル（make が作ったもの）
    snprintf(f->path, f->size, "%s/%s.c", dir, f->name);
    return stat(f->path, &st) == 0 && S_ISREG(st.st_mode);
}

// 名前のアプリのパス（PATH の順に、ディレクトリ/名前 か ディレクトリ/名前.c）。なければ false
// / を含む名前（/apps/mi、./tool、sub/app）は PATH を探さず、その場所を動かす（Linux と同じ）: アプリのフォルダ、
// .c のファイル、名前に .c を足したファイル、Makefile のプログラムの順。最後のものは、Linux で make した後に
// ソースのフォルダの中のプログラムを動かす形（./mi、~/dev/tools/mini-editor/mi）: そのフォルダの Makefile が
// 作るプログラムの名前なら、フォルダを動かす（os/slimcc/makefile.c。名前が見つからないときだけ読む）
static bool find_path_app(const char *name, char *path, size_t size)
{
    struct stat st;
    if (stat(name, &st) == 0) {
        size_t n = strlen(name);
        if (!S_ISDIR(st.st_mode) && (n < 2 || strcmp(name + n - 2, ".c")) && !slimcc_is_executable(name))
            return false;   // C のソースでも実行ファイル（make が作ったもの）でもないファイル
        snprintf(path, size, "%s", name);
        return true;
    }
    // 親のフォルダの Makefile がこの名前のプログラムを作るなら、そのフォルダ（Linux で make した後の ./prog と同じ）。
    // 同じ名前の .c（Lua の lua.c）より先に見る（2026-11-16。前は ./lua で lua.c だけを動かしていた）
    const char *slash = strrchr(name, '/');
    char dir[LINE_MAX_LEN], prog[128];
    if (slash == name)
        snprintf(dir, sizeof dir, "/");
    else
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - name), name);
    if (slash[1] && make_program_name(dir, prog, sizeof prog) && !strcmp(prog, slash + 1)) {
        snprintf(path, size, "%s", dir);
        return true;
    }
    snprintf(path, size, "%s.c", name);
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool find_app(const char *name, char *path, size_t size)
{
    if (!name[0])
        return false;
    if (strchr(name, '/'))
        return find_path_app(name, path, size);
    FindApp f = {name, path, size};
    return for_each_path(find_in, &f);
}

// 補完に使うコマンドの名前（os/shell/lineedit.c）: 組み込みのコマンド、続けて PATH のアプリ
static int find_builtin(const char *name)
{
    for (int i = 0; i < (int)(sizeof(commands) / sizeof(commands[0])); i++)
        if (!strcmp(name, commands[i].name))
            return i;
    return -1;
}

static int cmd_which(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[LINE_MAX_LEN];
        if (find_builtin(argv[i]) >= 0)
            printf("%s: shell built-in command\n", argv[i]);
        else if (find_app(argv[i], path, sizeof(path)))
            printf("%s\n", path);
        else {
            fprintf(stderr, "which: no %s in (built-in commands, PATH=%s)\n", argv[i],
                    getenv("PATH") ? getenv("PATH") : "");
            status = 1;
        }
    }
    return status;
}

static const char *command_name(int i)
{
    int nc = (int)(sizeof(commands) / sizeof(commands[0]));
    if (i < nc)
        return commands[i].name;
    if (i == nc)
        load_apps();    // 補完のたびに読み直す（アプリを足したらすぐ出る）
    return i - nc < napps ? app_names[i - nc] : nullptr;
}

// export NAME=VALUE...: 環境変数を設定する（PATH など。電源を切ると消えるので、残すなら /.profile に書く）
static int cmd_export(int argc, char **argv)
{
    if (argc == 1)
        return cmd_env(argc, argv);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == argv[i]) {
            fprintf(stderr, "export: '%s': not a valid identifier\n", argv[i]);
            status = 1;
            continue;
        }
        if (!eq) {
            // export NAME: シェルの変数（NAME=値 だけの行で作ったもの）を環境変数にする（bash と同じ）
            const char *v = svar_get(argv[i]);
            if (v) {
                setenv(argv[i], v, 1);
                svar_unset(argv[i]);
            }
            continue;
        }
        *eq = '\0';
        if (setenv(argv[i], eq + 1, 1) != 0) {
            report_errno("export");
            status = 1;
        }
        svar_unset(argv[i]);
        *eq = '=';
    }
    return status;
}

// unset NAME...: 環境変数を消す（bash の unset と同じ。-v は変数の意味で受け付ける。関数（-f）はない）
static int cmd_unset(int argc, char **argv)
{
    int status = 0;
    bool funcs_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            continue;
        if (!strcmp(argv[i], "-f")) {
            funcs_only = true;
            continue;
        }
        if (funcs_only) {
            func_unset(argv[i]);
            continue;
        }
        if (argv[i][0] == '-' || strchr(argv[i], '=')) {
            fprintf(stderr, "unset: '%s': not a valid identifier\n", argv[i]);
            status = 1;
            continue;
        }
        unsetenv(argv[i]);
        svar_unset(argv[i]);
    }
    return status;
}

// timeout [-s SIG] [-k DUR] [--preserve-status] [-v] DURATION CMD...（GNU の timeout と同じ書き方）。
// 期限を過ぎたら、アプリ（run・PATH のアプリ・system() の子）に SIG（既定 TERM）を届ける: 関数を登録して
// いればその関数（コア 0 が待つところで呼ぶので、待たずに計算し続けるアプリには届かない。-k を付ける）、無視なら
// そのまま、既定の動き（終わる）なら Ctrl-C と同じしくみで止める。-s KILL と、-k DUR を過ぎたときは必ず止める。
// 組み込みのコマンドは止めない。期限を過ぎたら 124（--preserve-status なら、そのコマンドの終了の値）
static bool parse_duration(const char *s, double *sec)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return false;
    if (*end == 'm')
        v *= 60, end++;
    else if (*end == 'h')
        v *= 3600, end++;
    else if (*end == 'd')
        v *= 86400, end++;
    else if (*end == 's')
        end++;
    if (*end)
        return false;
    *sec = v;
    return true;
}

// シグナルの名前（TERM・SIGTERM・15）。わからなければ -1
static int signal_number(const char *s)
{
    static const struct {
        const char *name;
        int sig;
    } names[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ABRT", SIGABRT}, {"KILL", SIGKILL},
        {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
        {"TERM", SIGTERM}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"WINCH", SIGWINCH},
    };
    if (*s >= '0' && *s <= '9') {
        char *end;
        long v = strtol(s, &end, 10);
        return *end || v <= 0 || v >= NSIG ? -1 : (int)v;
    }
    if (!strncasecmp(s, "SIG", 3))
        s += 3;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(s, names[i].name))
            return names[i].sig;
    return -1;
}

static int cmd_timeout(int argc, char **argv)
{
    bool preserve = false, verbose = false;
    int sig = SIGTERM;
    double kill_after = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        const char *sv = nullptr, *kv = nullptr;
        if (!strcmp(a, "--")) {
            i++;
            break;
        } else if (!strcmp(a, "--preserve-status")) {
            preserve = true;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            verbose = true;
        } else if (!strcmp(a, "--foreground")) {
            // 意味がない（端末は 1 つ）
        } else if ((!strcmp(a, "-s") || !strcmp(a, "--signal")) && i + 1 < argc) {
            sv = argv[++i];
        } else if ((!strcmp(a, "-k") || !strcmp(a, "--kill-after")) && i + 1 < argc) {
            kv = argv[++i];
        } else if (!strncmp(a, "--signal=", 9)) {
            sv = a + 9;
        } else if (!strncmp(a, "--kill-after=", 13)) {
            kv = a + 13;
        } else if (a[1] == 's') {
            sv = a + 2;
        } else if (a[1] == 'k') {
            kv = a + 2;
        } else {
            fprintf(stderr, "timeout: invalid option -- '%s'\n", a);
            return 125;
        }
        if (sv && (sig = signal_number(sv)) < 0) {
            fprintf(stderr, "timeout: %s: invalid signal\n", sv);
            return 125;
        }
        if (kv && !parse_duration(kv, &kill_after)) {
            fprintf(stderr, "timeout: invalid time interval '%s'\n", kv);
            return 125;
        }
    }
    double sec;
    if (i >= argc || !parse_duration(argv[i], &sec)) {
        if (i < argc)
            fprintf(stderr, "timeout: invalid time interval '%s'\n", argv[i]);
        else
            fprintf(stderr, "Usage: timeout [OPTION] DURATION COMMAND [ARG]...\n");
        return 125;
    }
    if (++i >= argc) {
        fprintf(stderr, "Usage: timeout [OPTION] DURATION COMMAND [ARG]...\n");
        return 125;
    }
    if (sec == 0)       // 0 は期限なし（GNU と同じ）
        return run_command(argc - i, argv + i);
    if (sec > 1e9)
        sec = 1e9;
    if (kill_after > 1e9)
        kill_after = 1e9;
    int slot = app_timeout_push(timer_now_us() + (uint64_t)(sec * 1e6), sig, (uint64_t)(kill_after * 1e6));
    if (slot < 0) {
        fprintf(stderr, "timeout: too many nested timeouts\n");
        return 125;
    }
    int status = run_command(argc - i, argv + i);
    bool fired = app_timeout_pop(slot);
    if (fired && verbose)
        fprintf(stderr, "timeout: sending signal %d to command '%s'\n", sig, argv[i]);
    return fired && !preserve ? 124 : status;
}

// dmesg [-c|-C]: カーネルの記録（os/kernel/klog.c）
static int cmd_dmesg(int argc, char **argv)
{
    bool show = true, clear = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--read-clear")) {
            clear = true;
        } else if (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--clear")) {
            clear = true;
            show = false;
        } else {
            fprintf(stderr, "dmesg: invalid option -- '%s'\n", argv[i]);
            return 1;
        }
    }
    if (show) {
        size_t size = klog_size();
        char *buf = malloc(size + 1);
        if (!buf) {
            report_errno("dmesg");
            return 1;
        }
        size_t n = klog_read(buf, size);
        fwrite(buf, 1, n, stdout);
        free(buf);
    }
    if (clear)
        klog_clear();
    return 0;
}


static int cmd_help(int, char **)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (commands[i].help)
            printf("  %s\n", commands[i].help);
    load_apps();
    if (napps) {
        printf("  アプリ（PATH=%s。名前だけで動かせる）:", getenv("PATH") ? getenv("PATH") : "");
        for (int i = 0; i < napps; i++)
            printf(" %s", app_names[i]);
        printf("\n");
    }
    printf("  実行中のプログラムは Ctrl-C で止められます\n");
    return 0;
}

// ---- 行の実行: 引用符、展開、リダイレクト（> >> <）、パイプ（|）、if・for・while ----
// 書き方は bash（POSIX の sh）に合わせる。違うところは docs/programming-guide.md の 3.2。
// 1. 切り分け（tokenize_line）: 語と演算子に分ける。引用符を外し、展開するところに印（M_*）を付ける。
//    '...' はそのまま、"..." は \" \\ \$ \` を解いて $ の展開だけをする。外の \ は次の 1 文字をそのまま。
//    語の頭の # から行の終わりまでは注釈。改行は ; と同じ区切り（if・for などの中でも）。
// 2. 構文（parse_*）: 語の並びを木（Node）にする。if・then・elif・else・fi・for・in・do・done・while・until・!
//    は、コマンドの頭の、引用符のない語のときだけ予約語。閉じていなければ「続きがいる」（シェルは "> " で続きを読む）。
// 3. 実行（exec_node）: 単純なコマンドは、動かす直前に展開する（false; echo $? が 1）:
//    $NAME・${NAME}・${NAME:-語} などの形・${#NAME}・$?・$1〜$9（空）・$(...)・`...`・$((計算))。
//    引用符の外で展開した値は空白で語に分け直し、そのあと引用符の外の * ? [...] をファイル名に展開する
//    （合うものがなければそのまま）。代入の形の語（NAME=値）とリダイレクトの行き先は、分けず、展開もしない。
//    コマンドの前の NAME=値 は、そのコマンドの間だけ環境変数。NAME=値 だけなら、シェルの変数（export するまで
//    子には見えない）。
// パイプは一時ファイルを使う（Monocs は同時に 1 つのプログラムしか動かさない。MS-DOS と同じ方式）:
//   a | b | c → a の出力を /.pipe0 に書き、b は /.pipe0 を読んで /.pipe1 に書き、c は /.pipe1 を読む。
//   終わったら一時ファイルを消す。stderr はいつも画面

enum { T_WORD, T_PIPE, T_OUT, T_APPEND, T_IN, T_AND, T_OR, T_SEMI, T_ERR, T_ERR_APPEND, T_ERR_TO_OUT, T_NL,
       T_LPAREN, T_RPAREN, T_DSEMI, T_HEREDOC };

typedef struct {
    int kind;
    char *s;
    bool has_exp;   // 展開するもの（変数・置き換え・計算・ファイル名・{a,b}）を含む（印を付けてある。動かす直前に展開する）
    bool assign;    // NAME=値 の形（値は分け直さない。コマンドの頭なら代入）
    bool plain;     // 引用符も \ も展開もない（予約語になれる）
    bool brace;     // 引用符の外の { がある（{a,b}・{1..3} の展開）
    int off, end;   // 元の文の中の位置（関数の本体を文字列で覚えるため）
} Token;

// 語の中の印（打ち込んだ文字には現れない制御文字）。展開は expand_tokens
#define M_VAR    '\x01'     // M_VAR 名前 M_END: 引用符の外の変数（値を空白で語に分け直す）
#define M_END    '\x02'
#define M_QVAR   '\x03'     // M_QVAR 名前 M_END: "..." の中の変数
#define M_CMD    '\x04'     // M_CMD 行 M_CEND: 引用符の外の $(...)・`...`
#define M_QCMD   '\x05'     // M_QCMD 行 M_CEND: "..." の中の $(...)・`...`
#define M_CEND   '\x06'
#define M_QUOTE  '\x07'     // 引用符があった印（幅なし。"" だけの語も、空の語として残す）
#define M_LIT    '\x08'     // 次の 1 文字は引用された * ? [ ] \（ファイル名の展開に使わない）
#define M_PARAM  '\x0e'     // M_PARAM 中身 M_CEND: 引用符の外の ${NAME:-語} などの形
#define M_QPARAM '\x0f'     // M_QPARAM 中身 M_CEND: "..." の中の ${...}
#define M_ARITH  '\x10'     // M_ARITH 式 M_CEND: $((式))
#define HEREDOC_MAX 8       // 1 行のヒアドキュメントの数
static int shell_status;    // シェルが最後に動かした行の終了の値（httpd の /.status。nos.py cmd が返す）

#define MAX_TOKENS  512
// 1 つのコマンドの展開した語の数の上限（入れ物は要るだけ広げる。前は 256 で、`ls *.o` が 1,125 個で動かなかった。
// 2026-11-20。Linux は語の数ではなく、引数の合計の大きさ（ARG_MAX、約 2MB）で決まる）
#define MAX_WORDS   65536
#define MAX_STAGES  8
#define MAX_NODES   512
static int nest;    // system()・$(...) の入れ子の深さ（一時ファイルの名前を分ける）

// ---- シェルの変数（export していないもの。NAME=値 だけの行）----

typedef struct {
    char *name, *value;
} ShellVar;
static ShellVar *svars;
static int nsvars, capsvars;

static int svar_find(const char *name)
{
    for (int i = 0; i < nsvars; i++)
        if (!strcmp(svars[i].name, name))
            return i;
    return -1;
}

static const char *svar_get(const char *name)
{
    int i = svar_find(name);
    return i >= 0 ? svars[i].value : nullptr;
}

static void svar_unset(const char *name)
{
    int i = svar_find(name);
    if (i < 0)
        return;
    free(svars[i].name);
    free(svars[i].value);
    svars[i] = svars[--nsvars];
}

static void svar_set(const char *name, const char *value)
{
    int i = svar_find(name);
    if (i >= 0) {
        char *v = strdup(value);
        if (v) {
            free(svars[i].value);
            svars[i].value = v;
        }
        return;
    }
    if (nsvars == capsvars) {
        int cap = capsvars ? capsvars * 2 : 8;
        ShellVar *p = realloc(svars, (size_t)cap * sizeof *p);
        if (!p)
            return;
        svars = p;
        capsvars = cap;
    }
    char *n = strdup(name), *v = strdup(value);
    if (!n || !v) {
        free(n);
        free(v);
        return;
    }
    svars[nsvars++] = (ShellVar){n, v};
}

// ---- 関数（NAME() { …; }）と位置の引数（$1・$#・$@）----
// 関数は本体の文字列で覚え、呼ぶたびに読み直す（定義した行が終わっても使えるように）

typedef struct {
    char *name, *body;
} ShellFunc;
static ShellFunc *funcs;
static int nfuncs, capfuncs;
static char **pos_argv;         // 位置の引数（$1 から。malloc した文字列）
static int pos_argc;
static int func_depth;          // 動いている関数の深さ

static int func_find(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (!strcmp(funcs[i].name, name))
            return i;
    return -1;
}

static void func_unset(const char *name)
{
    int i = func_find(name);
    if (i < 0)
        return;
    free(funcs[i].name);
    free(funcs[i].body);
    funcs[i] = funcs[--nfuncs];
}

static void func_set(const char *name, const char *body, size_t len)
{
    char *b = strndup(body, len);
    if (!b)
        return;
    int i = func_find(name);
    if (i >= 0) {
        free(funcs[i].body);
        funcs[i].body = b;
        return;
    }
    if (nfuncs == capfuncs) {
        int cap = capfuncs ? capfuncs * 2 : 8;
        ShellFunc *p = realloc(funcs, (size_t)cap * sizeof *p);
        if (!p) {
            free(b);
            return;
        }
        funcs = p;
        capfuncs = cap;
    }
    char *n = strdup(name);
    if (!n) {
        free(b);
        return;
    }
    funcs[nfuncs++] = (ShellFunc){n, b};
}

static void pos_free(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

// 位置の引数を argv[0..argc-1] の写しにする（前のものは *old・*oldc に渡す。NULL なら捨てる）
static void pos_set(int argc, char **argv, char ***old, int *oldc)
{
    char **v = argc ? calloc((size_t)argc, sizeof(char *)) : nullptr;
    int n = 0;
    for (int i = 0; v && i < argc; i++)
        if ((v[i] = strdup(argv[i])))
            n++;
    if (old) {
        *old = pos_argv;
        *oldc = pos_argc;
    } else {
        pos_free(pos_argv, pos_argc);
    }
    pos_argv = v;
    pos_argc = v ? n : 0;
}

// 入れ子（system()・$(...)）の前後: シェルの変数と環境変数を控えて、終わったら戻す（子の変更を残さない）。
// system() の子には、export していない変数は見えない（Linux の sh -c と同じ）
typedef struct {
    ShellVar *vars;
    int n, cap;
    char **env;     // 環境変数の写し（NAME=値。NULL で終わる）。控えないときは NULL
    ShellFunc *funcs;   // 関数（$(...) と ( … ) では写しを渡す。system() の子には見せない）
    int nf, capf;
    char **pos;         // 位置の引数
    int npos;
} ShellSaved;

static void shell_save(ShellSaved *s, bool keep_vars, bool env)
{
    s->funcs = funcs;
    s->nf = nfuncs;
    s->capf = capfuncs;
    funcs = nullptr;
    nfuncs = capfuncs = 0;
    if (keep_vars)
        for (int i = 0; i < s->nf; i++)
            func_set(s->funcs[i].name, s->funcs[i].body, strlen(s->funcs[i].body));
    s->pos = pos_argv;
    s->npos = pos_argc;
    pos_argv = nullptr;
    pos_argc = 0;
    if (keep_vars)
        pos_set(s->npos, s->pos, nullptr, nullptr);
    s->vars = svars;
    s->n = nsvars;
    s->cap = capsvars;
    s->env = nullptr;
    if (keep_vars) {        // $(...) の中では見える: 写しを渡す
        svars = nullptr;
        nsvars = capsvars = 0;
        for (int i = 0; i < s->n; i++)
            svar_set(s->vars[i].name, s->vars[i].value);
    } else {
        svars = nullptr;
        nsvars = capsvars = 0;
    }
    if (env) {
        extern char **environ;
        int n = 0;
        while (environ && environ[n])
            n++;
        s->env = calloc((size_t)n + 1, sizeof(char *));
        for (int i = 0; s->env && i < n; i++)
            s->env[i] = strdup(environ[i]);
    }
}

static void shell_restore(ShellSaved *s)
{
    while (nfuncs > 0)
        func_unset(funcs[0].name);
    free(funcs);
    funcs = s->funcs;
    nfuncs = s->nf;
    capfuncs = s->capf;
    pos_free(pos_argv, pos_argc);
    pos_argv = s->pos;
    pos_argc = s->npos;
    while (nsvars > 0)
        svar_unset(svars[0].name);
    free(svars);
    svars = s->vars;
    nsvars = s->n;
    capsvars = s->cap;
    if (s->env) {
        extern char **environ;
        // 今の名前をすべて消してから、控えを戻す（unsetenv は environ を詰めるので、名前は先に写す）
        int n = 0;
        while (environ && environ[n])
            n++;
        char **names = calloc((size_t)n + 1, sizeof(char *));
        for (int i = 0; names && i < n; i++)
            names[i] = strndup(environ[i], strcspn(environ[i], "="));
        for (int i = 0; names && i < n; i++) {
            unsetenv(names[i]);
            free(names[i]);
        }
        free(names);
        for (int i = 0; s->env[i]; i++) {
            char *eq = strchr(s->env[i], '=');
            if (eq) {
                *eq = '\0';
                setenv(s->env[i], eq + 1, 1);
            }
            free(s->env[i]);
        }
        free(s->env);
        s->env = nullptr;
    }
}

static void env_clear_all(void)
{
    extern char **environ;
    int n = 0;
    while (environ && environ[n])
        n++;
    char **names = calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; names && i < n; i++)
        names[i] = strndup(environ[i], strcspn(environ[i], "="));
    for (int i = 0; names && i < n; i++) {
        unsetenv(names[i]);
        free(names[i]);
    }
    free(names);
}


// env [-i] [-u NAME]... [NAME=VALUE]... [COMMAND [ARG]...]（GNU の env と同じ書き方。2026-11-20、Lua の main.lua）:
// 環境変数を変えて COMMAND を動かし、終わったらシェルの環境変数を戻す。COMMAND がなければ一覧を出す。
// シェルの関数・export していない変数は COMMAND に見せない（env は外のコマンドなので）
static int cmd_env(int argc, char **argv)
{
    ShellSaved saved;
    shell_save(&saved, false, true);
    int status = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") || !strcmp(a, "-") || !strcmp(a, "--ignore-environment")) {
            env_clear_all();
        } else if (!strcmp(a, "-u") || !strcmp(a, "--unset")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "env: option requires an argument -- 'u'\n");
                status = 125;
                goto out;
            }
            unsetenv(argv[++i]);
        } else if (!strncmp(a, "--unset=", 8)) {
            unsetenv(a + 8);
        } else if (!strncmp(a, "-u", 2)) {
            unsetenv(a + 2);
        } else if (!strcmp(a, "--")) {
            i++;
            break;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "env: invalid option -- '%s'\n", a + 1);
            status = 125;
            goto out;
        } else {
            break;
        }
    }
    for (; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i])
            break;
        *eq = '\0';
        setenv(argv[i], eq + 1, 1);
        *eq = '=';
    }
    if (i == argc) {
        extern char **environ;
        for (char **e = environ; e && *e; e++)
            printf("%s\n", *e);
    } else {
        status = run_command(argc - i, argv + i);
    }
out:
    shell_restore(&saved);
    return status;
}

// ---- 切り分け ----

static bool name_start(char c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool name_char(char c)
{
    return name_start(c) || (c >= '0' && c <= '9');
}

static bool glob_char(char c)
{
    return c == '*' || c == '?' || c == '[';
}

// NAME=... の形か（名前の前後に引用符・印がない）
static bool is_assignment(const char *s)
{
    if (!name_start(*s))
        return false;
    while (name_char(*s))
        s++;
    return *s == '=';
}

static bool quiet_errors;   // 書き方の誤りを表示しない（続きがいるかを調べるとき）

static void syntax_error(const char *fmt, const char *arg)
{
    if (!quiet_errors) {
        fprintf(stderr, "syntax error: ");
        fprintf(stderr, fmt, arg);
        fprintf(stderr, "\n");
    }
}

#define TOK_ERROR       (-1)    // 書き方の誤り
#define TOK_INCOMPLETE  (-2)    // 引用符などが閉じていない（続きがいる）

static bool put(char *words, size_t *w, size_t wsize, char c)
{
    if (*w + 1 >= wsize)
        return false;
    words[(*w)++] = c;
    return true;
}

// 引用符の中を飛ばす（p は ' か " を指す）。閉じていなければ NULL
static const char *skip_quote(const char *p)
{
    if (*p == '\'') {
        const char *q = strchr(p + 1, '\'');
        return q ? q + 1 : nullptr;
    }
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && p[1])
            p++;
    return *p ? p + 1 : nullptr;
}

// $( の次から、対応する ) を探す（中の引用符・入れ子の括弧も見る）。見つからなければ NULL
static const char *paren_end(const char *p)
{
    int depth = 1;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            p = skip_quote(p);
            if (!p)
                return nullptr;
            continue;
        }
        if (*p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        if (*p == '(')
            depth++;
        else if (*p == ')' && --depth == 0)
            return p;
        p++;
    }
    return nullptr;
}

// ${ の次から、対応する } を探す
static const char *brace_end(const char *p)
{
    int depth = 1;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            p = skip_quote(p);
            if (!p)
                return nullptr;
            continue;
        }
        if (*p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        if (p[0] == '$' && p[1] == '{') {
            depth++;
            p += 2;
            continue;
        }
        if (*p == '}' && --depth == 0)
            return p;
        p++;
    }
    return nullptr;
}

// $(( の次から、閉じる )) を探す（その最初の ) を返す）
static const char *arith_end(const char *p)
{
    int depth = 0;
    for (; *p; p++) {
        if (*p == '(')
            depth++;
        else if (*p == ')') {
            if (depth == 0)
                return p[1] == ')' ? p : nullptr;
            depth--;
        }
    }
    return nullptr;
}

// 印 mark、p から e の手前まで、M_CEND を書く
static bool put_raw(char *words, size_t *w, size_t wsize, char mark, const char *p, const char *e)
{
    if (!put(words, w, wsize, mark))
        return false;
    for (; p < e; p++)
        if (!put(words, w, wsize, *p))
            return false;
    return put(words, w, wsize, M_CEND);
}

// *pp が指す $ か ` を読む。展開するものなら印を words に書いて 1、ただの $ なら 0、誤りは TOK_ERROR・TOK_INCOMPLETE
static int scan_dollar(const char **pp, char *words, size_t *w, size_t wsize, bool quoted)
{
    const char *p = *pp;
    if (*p == '`') {
        // `...`: 中の \` \\ \$ を解く（bash と同じ）
        if (!put(words, w, wsize, quoted ? M_QCMD : M_CMD))
            return TOK_ERROR;
        for (p++; *p && *p != '`'; p++) {
            if (*p == '\\' && (p[1] == '`' || p[1] == '\\' || p[1] == '$'))
                p++;
            if (!put(words, w, wsize, *p))
                return TOK_ERROR;
        }
        if (!*p)
            return TOK_INCOMPLETE;
        *pp = p + 1;
        return put(words, w, wsize, M_CEND) ? 1 : TOK_ERROR;
    }
    p++;
    if (p[0] == '(' && p[1] == '(') {
        const char *e = arith_end(p + 2);
        if (!e)
            return TOK_INCOMPLETE;
        if (!put_raw(words, w, wsize, M_ARITH, p + 2, e))
            return TOK_ERROR;
        *pp = e + 2;
        return 1;
    }
    if (*p == '(') {
        const char *e = paren_end(p + 1);
        if (!e)
            return TOK_INCOMPLETE;
        if (!put_raw(words, w, wsize, quoted ? M_QCMD : M_CMD, p + 1, e))
            return TOK_ERROR;
        *pp = e + 1;
        return 1;
    }
    if (*p == '{') {
        const char *e = brace_end(p + 1);
        if (!e)
            return TOK_INCOMPLETE;
        const char *n = p + 1;
        bool simple = false;     // ${NAME}・${?}・${#}・${10} などは $NAME と同じ
        if (n + 1 == e && strchr("?#@*$!0", *n))
            simple = true;
        else if (*n >= '0' && *n <= '9') {
            const char *q = n;
            while (q < e && *q >= '0' && *q <= '9')
                q++;
            simple = q == e;
        } else if (name_start(*n)) {
            const char *q = n;
            while (q < e && name_char(*q))
                q++;
            simple = q == e;
        }
        if (n == e) {
            syntax_error("bad substitution%s", "");
            return TOK_ERROR;
        }
        if (simple) {
            if (!put_raw(words, w, wsize, quoted ? M_QVAR : M_VAR, n, e))
                return TOK_ERROR;
            words[*w - 1] = M_END;
        } else if (!put_raw(words, w, wsize, quoted ? M_QPARAM : M_PARAM, n, e)) {
            return TOK_ERROR;
        }
        *pp = e + 1;
        return 1;
    }
    const char *name = p, *end;
    if (*p && (strchr("?#@*$!", *p) || (*p >= '0' && *p <= '9'))) {
        // $? $# $@ $* $$ $!、位置の引数 $0〜$9（$10 からは ${10}）
        end = p + 1;
    } else if (name_start(*p)) {
        end = p;
        while (name_char(*end))
            end++;
    } else {
        return 0;
    }
    *pp = end;
    if (!put_raw(words, w, wsize, quoted ? M_QVAR : M_VAR, name, end))
        return TOK_ERROR;
    words[*w - 1] = M_END;
    return 1;
}

// 語の終わりか（引用符の外で）
static bool word_end(const char *p)
{
    return !*p || *p == ' ' || *p == '\t' || *p == '\n' || *p == '|' || *p == '>' || *p == '<' || *p == ';' ||
           *p == '&' || *p == '(' || *p == ')';
}

// 引用された文字を書く（ファイル名の展開に使う文字には M_LIT を付ける）
static bool put_quoted(char *words, size_t *w, size_t wsize, char c)
{
    if ((glob_char(c) || c == ']' || c == '\\' || c == '{' || c == '}' || c == ',') && !put(words, w, wsize, M_LIT))
        return false;
    return put(words, w, wsize, c);
}

// ヒアドキュメント（<<WORD）: 本文は、その行の次の行から WORD だけの行まで
typedef struct {
    int tok;            // 本文を入れる語（T_HEREDOC の次の T_WORD）
    char delim[128];
    bool quoted;        // WORD に引用符があった（本文を展開しない）
    bool dash;          // <<-（行の頭のタブを除く）
} Heredoc;

// p（行の頭）から本文を読み、words に詰める。*pp を区切りの行の次に進める。区切りがなければ TOK_INCOMPLETE
static int read_heredoc(const char **pp, Heredoc *h, Token *toks, char *words, size_t *w, size_t wsize)
{
    const char *p = *pp;
    size_t start = *w;
    size_t dl = strlen(h->delim);
    for (;;) {
        if (!*p)
            return TOK_INCOMPLETE;
        const char *le = strchr(p, '\n');
        if (!le)
            le = p + strlen(p);
        const char *q = p;
        if (h->dash)
            while (q < le && *q == '\t')
                q++;
        if ((size_t)(le - q) == dl && !memcmp(q, h->delim, dl)) {
            p = *le ? le + 1 : le;
            break;
        }
        if (!*le)
            return TOK_INCOMPLETE;  // 区切りの行がまだない
        while (q < le) {
            if (!h->quoted && (*q == '$' || *q == '`')) {
                int r = scan_dollar(&q, words, w, wsize, true);
                if (r < 0)
                    return r;
                if (r > 0) {
                    toks[h->tok].has_exp = true;
                    continue;
                }
            }
            if (!h->quoted && *q == '\\' && q + 1 < le && (q[1] == '$' || q[1] == '`' || q[1] == '\\'))
                q++;
            if (!put(words, w, wsize, *q++))
                return TOK_ERROR;
        }
        if (!put(words, w, wsize, '\n'))
            return TOK_ERROR;
        p = le + 1;
    }
    if (!put(words, w, wsize, '\0'))
        return TOK_ERROR;
    toks[h->tok].s = words + start;
    *pp = p;
    return 0;
}

// line を切り分ける（words に引用符を外した語を詰める）。トークンの数、誤りは TOK_ERROR・TOK_INCOMPLETE
static int tokenize_line(const char *line, Token *toks, int max, char *words, size_t wsize)
{
    int n = 0, closed = 0, toff = 0;
    size_t w = 0;
    const char *p = line;
    Heredoc hd[HEREDOC_MAX];
    int nhd = 0;
    for (;;) {
        if (n > closed) {       // 前のトークンの位置
            for (int k = closed; k < n; k++) {
                toks[k].off = toff;
                toks[k].end = (int)(p - line);
            }
            closed = n;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (!*p)
            break;
        toff = (int)(p - line);
        if (n + 2 >= max) {
            syntax_error("too many words%s", "");
            return TOK_ERROR;
        }
        if (*p == '#') {        // 注釈（語の頭の #）
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (*p == '\n') {
            if (n == 0 || toks[n - 1].kind != T_NL)
                toks[n++] = (Token){.kind = T_NL};
            p++;
            for (int k = 0; k < nhd; k++) {     // この行のヒアドキュメントの本文
                int r = read_heredoc(&p, &hd[k], toks, words, &w, wsize);
                if (r < 0)
                    return r;
            }
            nhd = 0;
            continue;
        }
        if (p[0] == ';' && p[1] == ';') {
            toks[n++] = (Token){.kind = T_DSEMI};
            p += 2;
            continue;
        }
        if (*p == '(' || *p == ')') {
            toks[n++] = (Token){.kind = *p == '(' ? T_LPAREN : T_RPAREN};
            p++;
            continue;
        }
        if (*p == '&' && p[1] != '&') {
            syntax_error("'&' (running in the background) is not supported%s", "");
            return TOK_ERROR;
        }
        if (p[0] == '<' && p[1] == '<') {
            // <<WORD・<<-WORD・<<'WORD'（引用符があれば本文を展開しない）
            if (nhd == HEREDOC_MAX) {
                syntax_error("too many here-documents%s", "");
                return TOK_ERROR;
            }
            Heredoc *h = &hd[nhd];
            *h = (Heredoc){.dash = p[2] == '-'};
            p += h->dash ? 3 : 2;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t dl = 0;
            while (!word_end(p)) {
                if (*p == '\'' || *p == '"') {
                    char qc = *p++;
                    h->quoted = true;
                    while (*p && *p != qc && dl + 1 < sizeof h->delim)
                        h->delim[dl++] = *p++;
                    if (*p != qc)
                        return TOK_INCOMPLETE;
                    p++;
                } else if (*p == '\\' && p[1]) {
                    h->quoted = true;
                    if (dl + 1 < sizeof h->delim)
                        h->delim[dl++] = p[1];
                    p += 2;
                } else if (dl + 1 < sizeof h->delim) {
                    h->delim[dl++] = *p++;
                } else {
                    p++;
                }
            }
            h->delim[dl] = '\0';
            if (dl == 0) {
                syntax_error("near unexpected token '%s'", "newline");
                return TOK_ERROR;
            }
            toks[n++] = (Token){.kind = T_HEREDOC};
            h->tok = n;
            toks[n++] = (Token){.kind = T_WORD, .s = (char *)""};
            nhd++;
            continue;
        }
        if (p[0] == '&' && p[1] == '&') {
            toks[n++] = (Token){.kind = T_AND};
            p += 2;
            continue;
        }
        if (p[0] == '|' && p[1] == '|') {
            toks[n++] = (Token){.kind = T_OR};
            p += 2;
            continue;
        }
        if (*p == ';') {
            toks[n++] = (Token){.kind = T_SEMI};
            p++;
            continue;
        }
        if (*p == '|') {
            toks[n++] = (Token){.kind = T_PIPE};
            p++;
            continue;
        }
        if (p[0] == '2' && p[1] == '>' &&
            (p == line || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == ';')) {
            // 2> FILE、2>> FILE、2>&1（stderr の向け先）
            if (p[2] == '&' && p[3] == '1') {
                toks[n++] = (Token){.kind = T_ERR_TO_OUT};
                p += 4;
            } else {
                toks[n++] = (Token){.kind = p[2] == '>' ? T_ERR_APPEND : T_ERR};
                p += p[2] == '>' ? 3 : 2;
            }
            continue;
        }
        if (*p == '>') {
            toks[n++] = (Token){.kind = p[1] == '>' ? T_APPEND : T_OUT};
            p += p[1] == '>' ? 2 : 1;
            continue;
        }
        if (*p == '<') {
            toks[n++] = (Token){.kind = T_IN};
            p++;
            continue;
        }
        size_t start = w;
        Token *t = &toks[n++];
        *t = (Token){.kind = T_WORD, .s = words + w, .plain = true};
        while (!word_end(p)) {
            // ~ と ~/...（語の頭か、NAME= の直後）は $HOME（展開した値は分け直さない）
            if (*p == '~' && (p[1] == '/' || word_end(p + 1)) &&
                (w == start || (words[w - 1] == '=' && is_assignment(words + start)))) {
                // 始めと終わりは同じ配列から（"HOME", "HOME" + 4 と書くと、文字列リテラルをまとめない
                // コンパイラ（slimcc）では別の場所になる。2026-11-16 に slimcc 版のカーネルで見つけた）
                static const char home[] = "HOME";
                if (!put_raw(words, &w, wsize, M_QVAR, home, home + 4))
                    return TOK_ERROR;
                words[w - 1] = M_END;
                t->has_exp = true;
                t->plain = false;
                p++;
                continue;
            }
            // "$@" だけは、引数がなければ語ごとなくなる（引用符の印を付けない）
            if (p[0] == '"' && p[1] == '$' && p[2] == '@' && p[3] == '"') {
                static const char at[] = "@";
                if (!put_raw(words, &w, wsize, M_QVAR, at, at + 1))
                    return TOK_ERROR;
                words[w - 1] = M_END;
                t->has_exp = true;
                t->plain = false;
                p += 4;
                continue;
            }
            if (*p == '{' && !word_end(p + 1)) {
                t->brace = true;        // {a,b}・{1..3} かもしれない（展開のときに確かめる）
                t->has_exp = true;
                t->plain = false;
            }
            if (*p == '\'') {
                t->plain = false;
                if (!put(words, &w, wsize, M_QUOTE))
                    return TOK_ERROR;
                for (p++; *p && *p != '\''; p++)
                    if (!put_quoted(words, &w, wsize, *p))
                        return TOK_ERROR;
                if (!*p)
                    return TOK_INCOMPLETE;
                p++;
            } else if (*p == '"') {
                t->plain = false;
                if (!put(words, &w, wsize, M_QUOTE))
                    return TOK_ERROR;
                for (p++; *p && *p != '"';) {
                    if (*p == '$' || *p == '`') {
                        int r = scan_dollar(&p, words, &w, wsize, true);
                        if (r < 0)
                            return r;
                        if (r > 0) {
                            t->has_exp = true;
                            continue;
                        }
                    }
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`'))
                        p++;
                    if (!put_quoted(words, &w, wsize, *p++))
                        return TOK_ERROR;
                }
                if (!*p)
                    return TOK_INCOMPLETE;
                p++;
            } else if (*p == '\\') {
                t->plain = false;
                if (!p[1] || (p[1] == '\n' && !p[2]))
                    return TOK_INCOMPLETE;      // 行の終わりの \（続きの行へ）
                if (p[1] == '\n') {             // \ と改行は、なかったことにする
                    p += 2;
                    continue;
                }
                if (!put_quoted(words, &w, wsize, p[1]))
                    return TOK_ERROR;
                p += 2;
            } else if (*p == '$' || *p == '`') {
                int r = scan_dollar(&p, words, &w, wsize, false);
                if (r < 0)
                    return r;
                if (r > 0) {
                    t->has_exp = true;
                    t->plain = false;
                } else if (!put(words, &w, wsize, *p++)) {
                    return TOK_ERROR;
                }
            } else {
                if (glob_char(*p)) {
                    t->has_exp = true;  // ファイル名の展開がいるかもしれない
                    t->plain = false;
                }
                if (!put(words, &w, wsize, *p++))
                    return TOK_ERROR;
            }
        }
        if (!put(words, &w, wsize, '\0'))
            return TOK_ERROR;
        t->assign = is_assignment(words + start);
        if (memchr(words + start, M_LIT, w - start))
            t->has_exp = true;  // 引用された * ? [ がある（case のパターンで文字どおりに扱うため、印を残す）
        if (!t->has_exp) {      // 展開しない語は、引用符の印を今のうちに除く
            size_t k = start;
            for (size_t i = start; i < w; i++) {
                if (words[i] == M_QUOTE)
                    continue;
                if (words[i] == M_LIT)
                    i++;
                words[k++] = words[i];
            }
            w = k;
        }
    }
    for (int k = closed; k < n; k++) {
        toks[k].off = toff;
        toks[k].end = (int)(p - line);
    }
    if (nhd > 0)
        return TOK_INCOMPLETE;      // ヒアドキュメントの本文がまだない
    return n;
}

// ---- 構文 ----

enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_SEQ, N_NOT, N_IF, N_FOR, N_WHILE, N_UNTIL, N_GROUP, N_SUBSHELL, N_CASE,
       N_CASEITEM, N_FUNCDEF, N_TIME };

typedef struct Node {
    int kind;
    int a, b;           // N_SIMPLE: 語とリダイレクトの範囲 [a, b)。N_FOR: 名前の位置 a、語の範囲 [wa, wb)。
                        // N_CASE: 語の位置 a。N_CASEITEM: パターン（| をはさむ）の範囲 [wa, wb)。
                        // N_FUNCDEF: 名前の位置 a、本体のトークンの範囲 [wa, wb)
    int wa, wb;
    int ra, rb;         // if・for・while の後ろのリダイレクトの範囲
    bool for_in;        // for に in がある
    struct Node *l, *r, *x;     // N_PIPE: l = この段、r = 次の N_PIPE。if: l 条件・r then・x elif か else
} Node;

typedef struct {
    Token *t;
    int n, pos;
    Node *nodes;
    int nn, cap_nodes;
    int err;            // 0、TOK_ERROR、TOK_INCOMPLETE
    int paren, in_case; // ( … ) の中・case の中（) と ;; で並びが終わる）
    const char *text;   // 元の文（関数の本体を文字列で覚える）
} Parser;

static Node *new_node(Parser *P, int kind)
{
    if (P->nn == P->cap_nodes) {
        syntax_error("too complex%s", "");
        P->err = TOK_ERROR;
        return nullptr;
    }
    Node *n = &P->nodes[P->nn++];
    *n = (Node){.kind = kind};
    return n;
}

static bool is_word(const Parser *P, const char *w)
{
    return P->pos < P->n && P->t[P->pos].kind == T_WORD && P->t[P->pos].plain && !strcmp(P->t[P->pos].s, w);
}

static bool at_stop(const Parser *P, const char *const *stops)
{
    for (; stops && *stops; stops++)
        if (is_word(P, *stops))
            return true;
    return false;
}

static const char *token_text(const Token *t)
{
    switch (t->kind) {
    case T_WORD: return t->s;
    case T_PIPE: return "|";
    case T_AND: return "&&";
    case T_OR: return "||";
    case T_SEMI: return ";";
    case T_NL: return "newline";
    case T_IN: return "<";
    case T_OUT: return ">";
    case T_APPEND: return ">>";
    case T_ERR: return "2>";
    case T_ERR_APPEND: return "2>>";
    case T_LPAREN: return "(";
    case T_RPAREN: return ")";
    case T_DSEMI: return ";;";
    case T_HEREDOC: return "<<";
    default: return "2>&1";
    }
}

static void unexpected(Parser *P)
{
    if (P->err)
        return;
    if (P->pos >= P->n) {
        P->err = TOK_INCOMPLETE;
        return;
    }
    syntax_error("near unexpected token '%s'", token_text(&P->t[P->pos]));
    P->err = TOK_ERROR;
}

static void skip_nl(Parser *P)
{
    while (P->pos < P->n && P->t[P->pos].kind == T_NL)
        P->pos++;
}

static bool expect(Parser *P, const char *w)
{
    if (is_word(P, w)) {
        P->pos++;
        return true;
    }
    unexpected(P);
    return false;
}

static bool is_redirect(int kind)
{
    return kind == T_OUT || kind == T_APPEND || kind == T_IN || kind == T_ERR || kind == T_ERR_APPEND ||
           kind == T_ERR_TO_OUT || kind == T_HEREDOC;
}

// ) か ;; で並びが終わるところか
static bool at_list_end(const Parser *P)
{
    if (P->pos >= P->n)
        return false;
    int k = P->t[P->pos].kind;
    return (k == T_RPAREN && P->paren) || (k == T_DSEMI && P->in_case);
}

static Node *parse_list(Parser *P, const char *const *stops);

// if・for・while の後ろのリダイレクト
static bool parse_redirects(Parser *P, Node *n)
{
    n->ra = P->pos;
    while (P->pos < P->n && is_redirect(P->t[P->pos].kind)) {
        if (P->t[P->pos].kind != T_ERR_TO_OUT) {
            if (P->pos + 1 >= P->n || P->t[P->pos + 1].kind != T_WORD) {
                P->pos++;
                unexpected(P);
                if (P->err == TOK_INCOMPLETE) {      // > の後で行が終わった
                    syntax_error("near unexpected token '%s'", "newline");
                    P->err = TOK_ERROR;
                }
                return false;
            }
            P->pos++;
        }
        P->pos++;
    }
    n->rb = P->pos;
    return true;
}

static Node *parse_if(Parser *P)
{
    static const char *const s_then[] = {"then", nullptr};
    static const char *const s_body[] = {"elif", "else", "fi", nullptr};
    static const char *const s_fi[] = {"fi", nullptr};
    P->pos++;       // if
    Node *top = new_node(P, N_IF);
    if (!top || !(top->l = parse_list(P, s_then)) || !expect(P, "then") || !(top->r = parse_list(P, s_body)))
        return nullptr;
    Node *cur = top;
    while (is_word(P, "elif")) {
        P->pos++;
        Node *e = new_node(P, N_IF);
        if (!e || !(e->l = parse_list(P, s_then)) || !expect(P, "then") || !(e->r = parse_list(P, s_body)))
            return nullptr;
        cur->x = e;
        cur = e;
    }
    if (is_word(P, "else")) {
        P->pos++;
        if (!(cur->x = parse_list(P, s_fi)))
            return nullptr;
    }
    return expect(P, "fi") ? top : nullptr;
}

static Node *parse_loop_body(Parser *P, Node *n)
{
    static const char *const s_done[] = {"done", nullptr};
    if (!expect(P, "do") || !(n->r = parse_list(P, s_done)) || !expect(P, "done"))
        return nullptr;
    return n;
}

static Node *parse_for(Parser *P)
{
    P->pos++;       // for
    Node *n = new_node(P, N_FOR);
    if (!n)
        return nullptr;
    if (P->pos >= P->n || P->t[P->pos].kind != T_WORD || !P->t[P->pos].plain || !name_start(P->t[P->pos].s[0])) {
        unexpected(P);
        return nullptr;
    }
    for (const char *q = P->t[P->pos].s; *q; q++)
        if (!name_char(*q)) {
            unexpected(P);
            return nullptr;
        }
    n->a = P->pos++;
    skip_nl(P);
    if (is_word(P, "in")) {
        P->pos++;
        n->for_in = true;
        n->wa = P->pos;
        while (P->pos < P->n && P->t[P->pos].kind == T_WORD)
            P->pos++;
        n->wb = P->pos;
        if (P->pos >= P->n || (P->t[P->pos].kind != T_SEMI && P->t[P->pos].kind != T_NL)) {
            unexpected(P);
            return nullptr;
        }
        P->pos++;
    } else if (P->pos < P->n && P->t[P->pos].kind == T_SEMI) {
        P->pos++;
    }
    skip_nl(P);
    return parse_loop_body(P, n);
}

static Node *parse_while(Parser *P, int kind)
{
    static const char *const s_do[] = {"do", nullptr};
    P->pos++;       // while・until
    Node *n = new_node(P, kind);
    if (!n || !(n->l = parse_list(P, s_do)))
        return nullptr;
    return parse_loop_body(P, n);
}

static Node *parse_command(Parser *P);

// ( … ): 中を、変数・cd などを持ち越さずに動かす
static Node *parse_subshell(Parser *P)
{
    P->pos++;
    Node *n = new_node(P, N_SUBSHELL);
    if (!n)
        return nullptr;
    int saved_case = P->in_case;
    P->in_case = 0;
    P->paren++;
    n->l = parse_list(P, nullptr);
    P->paren--;
    P->in_case = saved_case;
    if (P->err)
        return nullptr;
    if (P->pos >= P->n) {
        P->err = TOK_INCOMPLETE;
        return nullptr;
    }
    if (P->t[P->pos].kind != T_RPAREN || !n->l) {
        unexpected(P);
        return nullptr;
    }
    P->pos++;
    return n;
}

// { …; }
static Node *parse_group(Parser *P)
{
    static const char *const s_close[] = {"}", nullptr};
    P->pos++;
    Node *n = new_node(P, N_GROUP);
    if (!n || !(n->l = parse_list(P, s_close)) || !expect(P, "}"))
        return nullptr;
    return n;
}

// case 語 in パターン[|パターン]) 並び ;; … esac
static Node *parse_case(Parser *P)
{
    static const char *const s_esac[] = {"esac", nullptr};
    P->pos++;
    Node *n = new_node(P, N_CASE);
    if (!n)
        return nullptr;
    if (P->pos >= P->n || P->t[P->pos].kind != T_WORD) {
        unexpected(P);
        return nullptr;
    }
    n->a = P->pos++;
    skip_nl(P);
    if (!expect(P, "in"))
        return nullptr;
    Node **tail = &n->x;
    for (;;) {
        skip_nl(P);
        if (is_word(P, "esac")) {
            P->pos++;
            return n;
        }
        if (P->pos < P->n && P->t[P->pos].kind == T_LPAREN)
            P->pos++;
        Node *it = new_node(P, N_CASEITEM);
        if (!it)
            return nullptr;
        it->wa = P->pos;
        for (;;) {
            if (P->pos >= P->n || P->t[P->pos].kind != T_WORD) {
                unexpected(P);
                return nullptr;
            }
            P->pos++;
            if (P->pos < P->n && P->t[P->pos].kind == T_PIPE) {
                P->pos++;
                continue;
            }
            break;
        }
        it->wb = P->pos;
        if (P->pos >= P->n || P->t[P->pos].kind != T_RPAREN) {
            unexpected(P);
            return nullptr;
        }
        P->pos++;
        skip_nl(P);
        if (!(P->pos < P->n && P->t[P->pos].kind == T_DSEMI) && !is_word(P, "esac")) {
            P->in_case++;
            it->l = parse_list(P, s_esac);
            P->in_case--;
            if (!it->l)
                return nullptr;
        }
        *tail = it;
        tail = &it->x;
        if (P->pos < P->n && P->t[P->pos].kind == T_DSEMI) {
            P->pos++;
            continue;
        }
        if (is_word(P, "esac"))
            continue;
        unexpected(P);
        return nullptr;
    }
}

// NAME() 本体・function NAME [()] 本体（本体は { …; } などの複合のコマンド）
static Node *parse_funcdef(Parser *P, bool keyword)
{
    if (keyword)
        P->pos++;
    Node *n = new_node(P, N_FUNCDEF);
    if (!n)
        return nullptr;
    if (P->pos >= P->n || P->t[P->pos].kind != T_WORD || !P->t[P->pos].plain) {
        unexpected(P);
        return nullptr;
    }
    n->a = P->pos++;
    if (P->pos + 1 < P->n && P->t[P->pos].kind == T_LPAREN && P->t[P->pos + 1].kind == T_RPAREN)
        P->pos += 2;
    else if (!keyword) {
        unexpected(P);
        return nullptr;
    }
    skip_nl(P);
    if (P->pos >= P->n) {
        P->err = TOK_INCOMPLETE;
        return nullptr;
    }
    n->wa = P->pos;
    Node *body = parse_command(P);
    if (!body)
        return nullptr;
    if (body->kind == N_SIMPLE) {
        P->pos = n->wa;
        unexpected(P);
        return nullptr;
    }
    n->wb = P->pos;
    n->l = body;
    return n;
}

static Node *parse_command(Parser *P)
{
    static const char *const closers[] = {"then", "elif", "else", "fi", "do", "done", "}", "esac", nullptr};
    if (P->pos >= P->n) {
        unexpected(P);
        return nullptr;
    }
    Node *n = nullptr;
    if (is_word(P, "if"))
        n = parse_if(P);
    else if (is_word(P, "for"))
        n = parse_for(P);
    else if (is_word(P, "while"))
        n = parse_while(P, N_WHILE);
    else if (is_word(P, "until"))
        n = parse_while(P, N_UNTIL);
    else if (is_word(P, "case"))
        n = parse_case(P);
    else if (is_word(P, "{"))
        n = parse_group(P);
    else if (P->t[P->pos].kind == T_LPAREN)
        n = parse_subshell(P);
    else if (is_word(P, "function"))
        return parse_funcdef(P, true);
    else if (P->t[P->pos].kind == T_WORD && P->t[P->pos].plain && P->pos + 1 < P->n &&
             P->t[P->pos + 1].kind == T_LPAREN)
        return parse_funcdef(P, false);
    else if (at_stop(P, closers)) {
        unexpected(P);
        return nullptr;
    } else {
        n = new_node(P, N_SIMPLE);
        if (!n)
            return nullptr;
        n->a = P->pos;
        while (P->pos < P->n) {
            int k = P->t[P->pos].kind;
            if (k == T_PIPE || k == T_AND || k == T_OR || k == T_SEMI || k == T_NL || k == T_RPAREN || k == T_DSEMI)
                break;
            if (k == T_LPAREN) {
                unexpected(P);
                return nullptr;
            }
            if (is_redirect(k) && k != T_ERR_TO_OUT) {
                if (P->pos + 1 >= P->n || P->t[P->pos + 1].kind != T_WORD) {
                    P->pos++;
                    if (P->pos >= P->n)
                        syntax_error("near unexpected token '%s'", "newline");
                    else
                        syntax_error("near unexpected token '%s'", token_text(&P->t[P->pos]));
                    P->err = TOK_ERROR;
                    return nullptr;
                }
                P->pos++;
            }
            P->pos++;
        }
        n->b = P->pos;
        if (n->a == n->b) {
            unexpected(P);
            return nullptr;
        }
        return n;
    }
    if (!n || !parse_redirects(P, n))
        return nullptr;
    return n;
}

// time の測り方: real は経った時間。user は、アプリが使えるコア（コア 0〜2。サービスのコアは除く）が眠っていなかった
// 時間の合計（Linux でスレッドの CPU 時間を足すのと同じ考え。シェルの分も入る）。sys は 0（Monocs はアプリと OS を
// 分けて数えない。どちらも EL1）
typedef struct {
    uint64_t us;
    uint64_t idle[4];
} TimeMark;

static void time_mark(TimeMark *t)
{
    t->us = timer_now_us();
    t->idle[0] = timer_idle_us();
    uint64_t f = read_sysreg(cntfrq_el0);
    for (int c = 1; c < 4; c++) {
        uint64_t k = smp_idle_ticks(c);
        t->idle[c] = k / f * 1000000 + k % f * 1000000 / f;
    }
}

static void time_report(const TimeMark *a, const TimeMark *b, bool posix_fmt)
{
    uint64_t real = b->us - a->us, user = 0;
    for (int c = 0; c < 4; c++) {
        if (c > 0 && (smp_core_state(c) == 0 || smp_core_state(c) == 4))
            continue;   // 起きていない・サービスのコア
        uint64_t idle = b->idle[c] - a->idle[c];
        user += idle < real ? real - idle : 0;
    }
    fflush(stdout);
    if (posix_fmt) {
        fprintf(stderr, "real %llu.%02llu\nuser %llu.%02llu\nsys 0.00\n", (unsigned long long)(real / 1000000),
                (unsigned long long)(real % 1000000 / 10000), (unsigned long long)(user / 1000000),
                (unsigned long long)(user % 1000000 / 10000));
        return;
    }
    // bash の形（前に空の行、分と秒、ミリ秒まで）
    fprintf(stderr, "\nreal\t%llum%llu.%03llus\nuser\t%llum%llu.%03llus\nsys\t0m0.000s\n",
            (unsigned long long)(real / 60000000), (unsigned long long)(real / 1000000 % 60),
            (unsigned long long)(real % 1000000 / 1000), (unsigned long long)(user / 60000000),
            (unsigned long long)(user / 1000000 % 60), (unsigned long long)(user % 1000000 / 1000));
}

static Node *parse_pipeline(Parser *P)
{
    // time [-p]（bash と同じく、パイプライン全体に掛かる予約語。2026-11-16）
    bool timed = false, posix_fmt = false;
    if (is_word(P, "time")) {
        timed = true;
        P->pos++;
        if (is_word(P, "-p")) {
            posix_fmt = true;
            P->pos++;
        }
    }
    bool negate = false;
    if (is_word(P, "!")) {
        negate = true;
        P->pos++;
    }
    Node *c = parse_command(P);
    if (!c)
        return nullptr;
    if (P->pos < P->n && P->t[P->pos].kind == T_PIPE) {
        Node *head = new_node(P, N_PIPE), *tail = head;
        if (!head)
            return nullptr;
        head->l = c;
        while (P->pos < P->n && P->t[P->pos].kind == T_PIPE) {
            P->pos++;
            skip_nl(P);
            Node *d = parse_command(P);
            Node *p = d ? new_node(P, N_PIPE) : nullptr;
            if (!p)
                return nullptr;
            p->l = d;
            tail->r = p;
            tail = p;
        }
        c = head;
    }
    if (negate) {
        Node *n = new_node(P, N_NOT);
        if (!n)
            return nullptr;
        n->l = c;
        c = n;
    }
    if (timed) {
        Node *n = new_node(P, N_TIME);
        if (!n)
            return nullptr;
        n->l = c;
        n->a = posix_fmt;
        c = n;
    }
    return c;
}

static Node *parse_and_or(Parser *P)
{
    Node *l = parse_pipeline(P);
    while (l && P->pos < P->n && (P->t[P->pos].kind == T_AND || P->t[P->pos].kind == T_OR)) {
        int kind = P->t[P->pos].kind == T_AND ? N_AND : N_OR;
        P->pos++;
        skip_nl(P);
        Node *r = parse_pipeline(P);
        Node *n = r ? new_node(P, kind) : nullptr;
        if (!n)
            return nullptr;
        n->l = l;
        n->r = r;
        l = n;
    }
    return l;
}

// コマンドの並び（; か改行で区切る）。stops の語（then・fi など）か終わりまで。stops があるのに終わったら続きがいる
static Node *parse_list(Parser *P, const char *const *stops)
{
    Node *seq = nullptr;
    for (;;) {
        skip_nl(P);
        if (P->pos >= P->n) {
            if (stops || P->paren)
                P->err = TOK_INCOMPLETE;
            break;
        }
        if (at_stop(P, stops) || at_list_end(P))
            break;
        Node *c = parse_and_or(P);
        if (!c)
            return nullptr;
        if (seq) {
            Node *n = new_node(P, N_SEQ);
            if (!n)
                return nullptr;
            n->l = seq;
            n->r = c;
            seq = n;
        } else {
            seq = c;
        }
        if (P->pos >= P->n)
            continue;
        int k = P->t[P->pos].kind;
        if (k == T_SEMI || k == T_NL) {
            P->pos++;
            continue;
        }
        if (at_stop(P, stops) || at_list_end(P))
            break;
        unexpected(P);
        return nullptr;
    }
    if (P->err)
        return nullptr;
    if (!seq && stops) {
        unexpected(P);
        return nullptr;
    }
    return seq;
}

// ---- 展開 ----

// 伸びる文字の並び
typedef struct {
    char *p;
    size_t len, cap;
} SBuf;

static bool sb_add(SBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return false;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return true;
}

static bool sb_adds(SBuf *b, const char *s)
{
    return sb_add(b, s, strlen(s));
}

static bool exp_failed;     // 展開の誤り（${X:?}、計算の誤り）。コマンドは動かさず、終了の値 1

// 変数の値（$? は last_status、ほかは環境変数かシェルの変数）。ない変数は NULL
static const char *var_lookup(const char *name, size_t n, char *tmp, size_t tsize)
{
    if (n == 1 && name[0] == '?') {
        snprintf(tmp, tsize, "%d", last_status);
        return tmp;
    }
    if (n == 1 && name[0] == '#') {
        snprintf(tmp, tsize, "%d", pos_argc);
        return tmp;
    }
    if (n == 1 && name[0] == '$')
        return "1";     // シェルの番号（プロセスはないので、いつも 1）
    if (n == 1 && name[0] == '!')
        return nullptr; // 裏で動かしたものはない
    if (n == 1 && name[0] == '0')
        return "sh";
    if (n == 1 && (name[0] == '@' || name[0] == '*')) {
        // 引数を空白でつなぐ（"$@" は expand_tokens が 1 つずつの語にする）
        static char *joined;
        free(joined);
        size_t len = 1;
        for (int i = 0; i < pos_argc; i++)
            len += strlen(pos_argv[i]) + 1;
        joined = malloc(len);
        if (!joined)
            return "";
        joined[0] = '\0';
        for (int i = 0; i < pos_argc; i++) {
            if (i)
                strcat(joined, " ");
            strcat(joined, pos_argv[i]);
        }
        return joined;
    }
    if (name[0] >= '1' && name[0] <= '9') {
        int k = 0;
        for (size_t i = 0; i < n; i++) {
            if (name[i] < '0' || name[i] > '9')
                return nullptr;
            k = k * 10 + (name[i] - '0');
        }
        return k <= pos_argc ? pos_argv[k - 1] : nullptr;
    }
    if (n >= tsize)
        return nullptr;
    memcpy(tmp, name, n);
    tmp[n] = '\0';
    const char *v = getenv(tmp);
    return v ? v : svar_get(tmp);
}

static const char *var_value(const char *name, size_t n, char *tmp, size_t tsize)
{
    const char *v = var_lookup(name, n, tmp, tsize);
    return v ? v : "";
}

// NAME に値を入れる（export してあれば環境変数、なければシェルの変数）
static void set_var(const char *name, const char *value)
{
    if (getenv(name))
        setenv(name, value, 1);
    else
        svar_set(name, value);
}

static int run_text(const char *text);

// $(...): cmd を入れ子で動かし、標準出力を一時ファイル（/.subN）に取って返す（malloc。終わりの改行は除く）。
// 中の cd・export・代入は外に残さない（bash のサブシェルと同じ）
static int subst_status = -1;   // このコマンドで最後に動かした $(...) の終了の値（なければ -1。代入だけの行の $?）

static char *capture(const char *cmd)
{
    char path[24];
    snprintf(path, sizeof path, "/tmp/.sub%d", nest);
    char cwd[512];
    bool have_cwd = getcwd(cwd, sizeof cwd) != nullptr;
    ShellSaved saved;
    shell_save(&saved, true, true);
    int outer_subst = subst_status;
    int st = 1;
    if (console_redirect_push()) {
        if (console_redirect_out(path, false) && console_redirect_push()) {
            nest++;
            st = run_text(cmd);
            nest--;
            console_redirect_pop();
        }
        console_redirect_pop();
    }
    shell_restore(&saved);
    if (have_cwd)
        chdir(cwd);
    subst_status = outer_subst;
    SBuf out = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[1024];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0)
            if (!sb_add(&out, buf, k))
                break;
        fclose(f);
    }
    unlink(path);
    if (!out.p && !sb_add(&out, "", 0))
        return nullptr;
    while (out.len > 0 && out.p[out.len - 1] == '\n')
        out.p[--out.len] = '\0';
    last_status = subst_status = st & 0xFF;
    return out.p;
}

static bool expand_raw(const char *s, size_t n, SBuf *out);

// ---- $((式)) ----
// 整数（long long）。演算子: ( ) 単項の + - ! ~、* / %、+ -、<< >>、< <= > >=、== !=、&、^、|、&&、||、? :。
// 名前は変数の値（空・ないものは 0）。数は 10 進・0x の 16 進・0 で始まる 8 進

typedef struct {
    const char *p;
    bool err;
} Arith;

static long long ar_cond(Arith *a);

static void ar_ws(Arith *a)
{
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n')
        a->p++;
}

static bool ar_op(Arith *a, const char *op)
{
    ar_ws(a);
    size_t n = strlen(op);
    if (strncmp(a->p, op, n) != 0)
        return false;
    // 1 文字の演算子は、2 文字の演算子の頭なら合わない（< と <<、& と && など）
    if (n == 1 && a->p[1] && strchr("<>&|=", a->p[1]) && strchr("<>&|!=", op[0]) &&
        !(op[0] == '<' && a->p[1] == '=') && !(op[0] == '>' && a->p[1] == '='))
        if (a->p[1] == op[0] || a->p[1] == '=')
            return false;
    a->p += n;
    return true;
}

static long long ar_number(Arith *a, const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return 0;
    char *end;
    long long v = strtoll(s, &end, 0);
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end)
        a->err = true;
    return v;
}

static long long ar_primary(Arith *a)
{
    ar_ws(a);
    if (ar_op(a, "(")) {
        long long v = ar_cond(a);
        if (!ar_op(a, ")"))
            a->err = true;
        return v;
    }
    if (ar_op(a, "+"))
        return ar_primary(a);
    if (ar_op(a, "-"))
        return -ar_primary(a);
    if (ar_op(a, "!"))
        return !ar_primary(a);
    if (ar_op(a, "~"))
        return ~ar_primary(a);
    if (*a->p >= '0' && *a->p <= '9') {
        char *end;
        long long v = strtoll(a->p, &end, 0);
        a->p = end;
        return v;
    }
    if (name_start(*a->p)) {
        const char *s = a->p;
        while (name_char(*a->p))
            a->p++;
        char tmp[128];
        const char *v = var_lookup(s, (size_t)(a->p - s), tmp, sizeof tmp);
        return v ? ar_number(a, v) : 0;
    }
    a->err = true;
    return 0;
}

static long long ar_mul(Arith *a)
{
    long long v = ar_primary(a);
    for (;;) {
        if (ar_op(a, "*"))
            v *= ar_primary(a);
        else if (ar_op(a, "/") || ar_op(a, "%")) {
            bool div = a->p[-1] == '/';
            long long d = ar_primary(a);
            if (d == 0) {
                a->err = true;
                return 0;
            }
            v = div ? v / d : v % d;
        } else
            return v;
    }
}

static long long ar_add(Arith *a)
{
    long long v = ar_mul(a);
    for (;;) {
        if (ar_op(a, "+"))
            v += ar_mul(a);
        else if (ar_op(a, "-"))
            v -= ar_mul(a);
        else
            return v;
    }
}

static long long ar_shift(Arith *a)
{
    long long v = ar_add(a);
    for (;;) {
        if (ar_op(a, "<<"))
            v = (long long)((unsigned long long)v << (ar_add(a) & 63));
        else if (ar_op(a, ">>"))
            v >>= ar_add(a) & 63;
        else
            return v;
    }
}

static long long ar_rel(Arith *a)
{
    long long v = ar_shift(a);
    for (;;) {
        if (ar_op(a, "<="))
            v = v <= ar_shift(a);
        else if (ar_op(a, ">="))
            v = v >= ar_shift(a);
        else if (ar_op(a, "<"))
            v = v < ar_shift(a);
        else if (ar_op(a, ">"))
            v = v > ar_shift(a);
        else
            return v;
    }
}

static long long ar_eq(Arith *a)
{
    long long v = ar_rel(a);
    for (;;) {
        if (ar_op(a, "=="))
            v = v == ar_rel(a);
        else if (ar_op(a, "!="))
            v = v != ar_rel(a);
        else
            return v;
    }
}

static long long ar_band(Arith *a)
{
    long long v = ar_eq(a);
    while (ar_op(a, "&"))
        v &= ar_eq(a);
    return v;
}

static long long ar_bxor(Arith *a)
{
    long long v = ar_band(a);
    while (ar_op(a, "^"))
        v ^= ar_band(a);
    return v;
}

static long long ar_bor(Arith *a)
{
    long long v = ar_bxor(a);
    while (ar_op(a, "|"))
        v |= ar_bxor(a);
    return v;
}

static long long ar_land(Arith *a)
{
    long long v = ar_bor(a);
    while (ar_op(a, "&&")) {
        long long r = ar_bor(a);
        v = v && r;
    }
    return v;
}

static long long ar_lor(Arith *a)
{
    long long v = ar_land(a);
    while (ar_op(a, "||")) {
        long long r = ar_land(a);
        v = v || r;
    }
    return v;
}

static long long ar_cond(Arith *a)
{
    long long c = ar_lor(a);
    if (!ar_op(a, "?"))
        return c;
    long long x = ar_cond(a);
    if (!ar_op(a, ":")) {
        a->err = true;
        return 0;
    }
    long long y = ar_cond(a);
    return c ? x : y;
}

// $((式)) の値を out に。式の中の $NAME・$(...) は先に展開する
static bool arith_value(const char *raw, size_t n, SBuf *out)
{
    SBuf e = {0};
    if (!expand_raw(raw, n, &e) || !e.p)
        sb_add(&e, "", 0);
    Arith a = {e.p ? e.p : "", false};
    long long v = ar_cond(&a);
    ar_ws(&a);
    if (a.err || *a.p) {
        fprintf(stderr, "%s: arithmetic syntax error (or division by 0)\n", e.p ? e.p : "");
        free(e.p);
        exp_failed = true;
        return false;
    }
    free(e.p);
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", v);
    return sb_adds(out, buf);
}

// ---- ${...} ----

// 頭（prefix）か後ろ（!prefix）から、パターンに合ういちばん短い（longest でなければ）・長いところを除く
static void trim_pattern(SBuf *out, const char *v, const char *pat, bool prefix, bool longest)
{
    size_t n = strlen(v);
    char *s = strdup(v);
    if (!s)
        return;
    if (prefix) {
        for (size_t k = 0; k <= n; k++) {
            size_t len = longest ? n - k : k;
            char save = s[len];
            s[len] = '\0';
            bool m = fnmatch(pat, s, 0) == 0;
            s[len] = save;
            if (m) {
                sb_adds(out, v + len);
                free(s);
                return;
            }
        }
    } else {
        for (size_t k = 0; k <= n; k++) {
            size_t pos = longest ? k : n - k;
            if (fnmatch(pat, v + pos, 0) == 0) {
                sb_add(out, v, pos);
                free(s);
                return;
            }
        }
    }
    sb_adds(out, v);
    free(s);
}

static bool expand_raw(const char *s, size_t n, SBuf *out);

// 文字（UTF-8）の数と、k 文字目のバイトの位置
static size_t utf8_count(const char *s)
{
    size_t n = 0;
    for (const unsigned char *q = (const unsigned char *)s; *q; q++)
        n += (*q & 0xC0) != 0x80;
    return n;
}

static size_t utf8_offset(const char *s, size_t k)
{
    size_t i = 0;
    for (; s[i] && k > 0; k--) {
        i++;
        while (((unsigned char)s[i] & 0xC0) == 0x80)
            i++;
    }
    return i;
}

// 式の値（$((…)) と同じ計算）。誤りなら false
static bool arith_eval(const char *raw, size_t n, long long *v)
{
    SBuf e = {0};
    if (!expand_raw(raw, n, &e))
        return false;
    Arith a = {e.p ? e.p : "", false};
    *v = ar_cond(&a);
    ar_ws(&a);
    bool ok = !a.err && !*a.p;
    if (!ok) {
        fprintf(stderr, "%s: arithmetic syntax error\n", e.p ? e.p : "");
        exp_failed = true;
    }
    free(e.p);
    return ok;
}

// ${NAME:位置[:長さ]}（文字の数で。位置・長さが負なら後ろから）
static bool substring_value(SBuf *out, const char *v, const char *spec)
{
    const char *colon = strchr(spec, ':');
    long long off, len = -1;
    if (!arith_eval(spec, colon ? (size_t)(colon - spec) : strlen(spec), &off))
        return false;
    bool has_len = colon != nullptr;
    if (has_len && !arith_eval(colon + 1, strlen(colon + 1), &len))
        return false;
    long long count = (long long)utf8_count(v);
    if (off < 0)
        off += count;
    if (off < 0 || off > count)
        return true;    // 空
    long long end = count;
    if (has_len)
        end = len < 0 ? count + len : off + len;
    if (end > count)
        end = count;
    if (end <= off)
        return true;
    size_t a = utf8_offset(v, (size_t)off), b = utf8_offset(v, (size_t)end);
    return sb_add(out, v + a, b - a);
}

// ${NAME/パターン/置き換え}: いちばん長く合うところを置き換える
static bool replace_value(SBuf *out, const char *v, const char *spec)
{
    bool all = false, head = false, tail = false;
    if (*spec == '/')
        all = true, spec++;
    else if (*spec == '#')
        head = true, spec++;
    else if (*spec == '%')
        tail = true, spec++;
    // パターンと置き換えを分ける（\/ は / として）
    const char *sl = spec;
    while (*sl && *sl != '/') {
        if (*sl == '\\' && sl[1])
            sl++;
        sl++;
    }
    SBuf pat = {0}, rep = {0};
    bool ok = expand_raw(spec, (size_t)(sl - spec), &pat);
    if (ok && *sl)
        ok = expand_raw(sl + 1, strlen(sl + 1), &rep);
    if (!ok || !pat.p || !*pat.p) {
        free(pat.p);
        free(rep.p);
        return ok && sb_adds(out, v);
    }
    const char *r = rep.p ? rep.p : "";
    size_t n = strlen(v);
    char *buf = malloc(n + 1);
    if (!buf) {
        free(pat.p);
        free(rep.p);
        return false;
    }
    size_t i = 0;
    bool done = false;
    while (i <= n && ok) {
        size_t best = (size_t)-1;
        if (!done && (!head || i == 0)) {
            // i から始まる、いちばん長く合うところ（tail なら、終わりまでだけ）
            for (size_t j = n;; j--) {
                if (tail && j != n)
                    break;
                memcpy(buf, v + i, j - i);
                buf[j - i] = '\0';
                if (j > i && fnmatch(pat.p, buf, 0) == 0) {
                    best = j;
                    break;
                }
                if (j == i)
                    break;
            }
        }
        if (best != (size_t)-1) {
            ok = sb_adds(out, r);
            i = best;
            if (!all)
                done = true;
            continue;
        }
        if (i == n)
            break;
        ok = sb_add(out, v + i, 1);
        i++;
    }
    free(buf);
    free(pat.p);
    free(rep.p);
    return ok;
}

// ${中身}（${NAME} 以外の形）の値を out に
static bool param_value(const char *raw, size_t n, SBuf *out)
{
    char *s = strndup(raw, n);
    if (!s)
        return false;
    char tmp[128];
    bool ok = true;
    if (s[0] == '#' && s[1]) {          // ${#NAME}: 長さ（文字の数。UTF-8 の続きのバイトは数えない）
        const char *v = var_value(s + 1, strlen(s + 1), tmp, sizeof tmp);
        size_t len = 0;
        for (const unsigned char *q = (const unsigned char *)v; *q; q++)
            len += (*q & 0xC0) != 0x80;
        char buf[24];
        snprintf(buf, sizeof buf, "%zu", len);
        ok = sb_adds(out, buf);
        free(s);
        return ok;
    }
    const char *q = s;
    if (*q == '?' || (*q >= '1' && *q <= '9'))
        q++;
    else
        while (name_char(*q))
            q++;
    size_t nlen = (size_t)(q - s);
    if (nlen == 0) {
        fprintf(stderr, "${%s}: bad substitution\n", s);
        exp_failed = true;
        free(s);
        return false;
    }
    char name[128];
    snprintf(name, sizeof name, "%.*s", (int)nlen, s);
    const char *v = var_lookup(s, nlen, tmp, sizeof tmp);
    if (*q == '/') {        // ${NAME/パターン/置き換え}・${NAME//…}（すべて）・${NAME/#…}（頭）・${NAME/%…}（後ろ）
        ok = replace_value(out, v ? v : "", q + 1);
        free(s);
        return ok;
    }
    if (*q == ':' && q[1] && !strchr("-=+?", q[1])) {     // ${NAME:位置[:長さ]}
        ok = substring_value(out, v ? v : "", q + 1);
        free(s);
        return ok;
    }
    bool colon = *q == ':';
    if (colon)
        q++;
    char op = *q;
    bool dbl = (op == '#' || op == '%') && q[1] == op;
    const char *word = q + (op ? 1 : 0) + (dbl ? 1 : 0);
    bool unset = !v || (colon && !*v);   // : があれば、空も「ない」とみなす
    SBuf w = {0};
    switch (op) {
    case '-':
    case '=':
    case '+':
    case '?':
        if (op == '+') {
            if (!unset)
                ok = expand_raw(word, strlen(word), out);
            break;
        }
        if (!unset) {
            ok = sb_adds(out, v);
            break;
        }
        ok = expand_raw(word, strlen(word), &w);
        if (!ok)
            break;
        if (op == '-') {
            ok = sb_adds(out, w.p ? w.p : "");
        } else if (op == '=') {
            set_var(name, w.p ? w.p : "");
            ok = sb_adds(out, w.p ? w.p : "");
        } else {
            fprintf(stderr, "%s: %s\n", name, w.p && *w.p ? w.p : "parameter null or not set");
            exp_failed = true;
            ok = false;
        }
        break;
    case '#':
    case '%':
        if (colon) {
            fprintf(stderr, "${%s}: bad substitution\n", s);
            exp_failed = true;
            ok = false;
            break;
        }
        ok = expand_raw(word, strlen(word), &w);
        if (ok)
            trim_pattern(out, v ? v : "", w.p ? w.p : "", op == '#', dbl);
        break;
    default:
        fprintf(stderr, "${%s}: bad substitution\n", s);
        exp_failed = true;
        ok = false;
        break;
    }
    free(w.p);
    free(s);
    return ok;
}

// ${...} の中の語など、引用符のある生の文字を展開して out に（分け直さない。ファイル名の展開もしない）
static bool expand_raw(const char *s, size_t n, SBuf *out)
{
    const char *end = s + n;
    bool dq = false;
    char tmp[128];
    while (s < end) {
        if (!dq && *s == '\'') {
            const char *q = memchr(s + 1, '\'', (size_t)(end - s - 1));
            if (!q)
                q = end;
            if (!sb_add(out, s + 1, (size_t)(q - s - 1)))
                return false;
            s = q < end ? q + 1 : end;
            continue;
        }
        if (*s == '"') {
            dq = !dq;
            s++;
            continue;
        }
        if (*s == '\\' && s + 1 < end) {
            if (!dq || strchr("\"\\$`", s[1]))
                s++;
            if (!sb_add(out, s, 1))
                return false;
            s++;
            continue;
        }
        if (*s == '`') {
            const char *q = memchr(s + 1, '`', (size_t)(end - s - 1));
            if (!q)
                q = end;
            char *cmd = strndup(s + 1, (size_t)(q - s - 1));
            char *v = cmd ? capture(cmd) : nullptr;
            bool ok = v && sb_adds(out, v);
            free(v);
            free(cmd);
            if (!ok)
                return false;
            s = q < end ? q + 1 : end;
            continue;
        }
        if (*s == '$' && s + 1 < end) {
            const char *p = s + 1;
            bool ok = true;
            if (p[0] == '(' && p + 1 < end && p[1] == '(') {
                const char *e = arith_end(p + 2);
                if (!e || e >= end)
                    return false;
                ok = arith_value(p + 2, (size_t)(e - p - 2), out);
                s = e + 2;
            } else if (*p == '(') {
                const char *e = paren_end(p + 1);
                if (!e || e >= end)
                    return false;
                char *cmd = strndup(p + 1, (size_t)(e - p - 1));
                char *v = cmd ? capture(cmd) : nullptr;
                ok = v && sb_adds(out, v);
                free(v);
                free(cmd);
                s = e + 1;
            } else if (*p == '{') {
                const char *e = brace_end(p + 1);
                if (!e || e >= end)
                    return false;
                ok = param_value(p + 1, (size_t)(e - p - 1), out);
                s = e + 1;
            } else if (*p == '?' || (*p >= '1' && *p <= '9')) {
                ok = sb_adds(out, var_value(p, 1, tmp, sizeof tmp));
                s = p + 1;
            } else if (name_start(*p)) {
                const char *e = p;
                while (e < end && name_char(*e))
                    e++;
                ok = sb_adds(out, var_value(p, (size_t)(e - p), tmp, sizeof tmp));
                s = e;
            } else {
                ok = sb_add(out, s, 1);
                s++;
            }
            if (!ok)
                return false;
            continue;
        }
        if (!sb_add(out, s, 1))
            return false;
        s++;
    }
    return true;
}

// ---- 語の展開（分け直しとファイル名の展開）----

typedef struct {
    SBuf *b;            // 展開した語（NUL で区切って並べる）
    SBuf pat;           // 今の語のファイル名のパターン（引用された * ? [ ] \ は \ を前に付ける）
    bool glob;          // 今の語に、引用符の外の * ? [ がある
    bool do_glob;       // この語はファイル名に展開してよい
    size_t start;       // 今の語の始まり（b の中）
    bool started;       // 今の語がある（文字か引用符があった）
    Token *out;
    size_t *offs;
    int k, max;
    bool grow;          // out・offs は malloc したもので、足りなければ広げる（max まで）
    int cap;
    const Token *src;
} Expand;

// out・offs に 1 つ足せるようにする
static bool word_room(Expand *e)
{
    if (e->k == e->max)
        return false;
    if (!e->grow || e->k < e->cap)
        return true;
    int n = e->cap ? e->cap * 2 : 64;
    if (n > e->max)
        n = e->max;
    Token *o = realloc(e->out, sizeof *o * (size_t)n);
    if (!o)
        return false;
    e->out = o;
    size_t *f = realloc(e->offs, sizeof *f * (size_t)n);
    if (!f)
        return false;
    e->offs = f;
    e->cap = n;
    return true;
}

static bool push_word(Expand *e, size_t at)
{
    if (!word_room(e))
        return false;
    e->out[e->k] = *e->src;
    e->offs[e->k] = at;
    e->k++;
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool emit_word(Expand *e)
{
    if (!e->started) {
        e->pat.len = 0;
        e->glob = false;
        return true;
    }
    bool ok = true;
    if (e->glob && e->do_glob && e->pat.p) {
        glob_t g;
        int r = glob(e->pat.p, 0, nullptr, &g);
        if (r == 0 && g.gl_pathc > 0) {
            e->b->len = e->start;           // パターンの代わりに、合ったものを並べる（名前の順。glob が並べる）
            qsort(g.gl_pathv, g.gl_pathc, sizeof(char *), cmp_str);
            for (size_t i = 0; ok && i < g.gl_pathc; i++) {
                size_t at = e->b->len;
                ok = sb_adds(e->b, g.gl_pathv[i]) && sb_add(e->b, "", 1) && push_word(e, at);
            }
            globfree(&g);
            e->start = e->b->len;
            e->started = false;
            e->pat.len = 0;
            e->glob = false;
            return ok;
        }
        if (r == 0)
            globfree(&g);
    }
    ok = sb_add(e->b, "", 1) && push_word(e, e->start);
    e->start = e->b->len;
    e->started = false;
    e->pat.len = 0;
    if (e->pat.p)
        e->pat.p[0] = '\0';
    e->glob = false;
    return ok;
}

// 1 文字を足す（quoted なら、ファイル名の展開では文字どおり）
static bool add_char(Expand *e, char c, bool quoted)
{
    e->started = true;
    if (!sb_add(e->b, &c, 1))
        return false;
    if (quoted && (glob_char(c) || c == ']' || c == '\\'))
        return sb_add(&e->pat, "\\", 1) && sb_add(&e->pat, &c, 1);
    if (!quoted && glob_char(c))
        e->glob = true;
    if (!quoted && c == '\\')
        return sb_add(&e->pat, "\\\\", 2);
    return sb_add(&e->pat, &c, 1);
}

// 展開した値を足す。split なら空白で語に分ける（引用符の外）。引用符の中なら、空でも語になる
static bool add_value(Expand *e, const char *v, bool quoted, bool split)
{
    if (quoted || !split) {
        if (quoted)
            e->started = true;
        for (; *v; v++)
            if (!add_char(e, *v, quoted))
                return false;
        return true;
    }
    for (; *v; v++) {
        if (*v == ' ' || *v == '\t' || *v == '\n') {
            if (!emit_word(e))
                return false;
        } else if (!add_char(e, *v, false)) {
            return false;
        }
    }
    return true;
}

// ---- {a,b}・{1..3} ----

// s の中の、印の中でない次の文字へ（印の中身・M_LIT の次の文字は飛ばす）
static const char *skip_mark(const char *p)
{
    char c = *p;
    if (c == M_VAR || c == M_QVAR)
        return strchr(p, M_END);
    if (c == M_CMD || c == M_QCMD || c == M_PARAM || c == M_QPARAM || c == M_ARITH)
        return strchr(p, M_CEND);
    if (c == M_LIT)
        return p + 1;
    return p;
}

#define BRACE_MAX 1024      // 1 つの語から作る語の数

static bool brace_out(char ***list, int *n, int *cap, const char *s, size_t len)
{
    if (*n == BRACE_MAX)
        return false;
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        char **p = realloc(*list, (size_t)c * sizeof(char *));
        if (!p)
            return false;
        *list = p;
        *cap = c;
    }
    char *d = strndup(s, len);
    if (!d)
        return false;
    (*list)[(*n)++] = d;
    return true;
}

static bool brace_expand_into(const char *s, char ***list, int *n, int *cap);

// {x..y[..step]} の範囲（数か 1 文字）。なっていなければ false
static bool brace_range(const char *pre, size_t plen, const char *in, size_t ilen, const char *post, char ***list,
                        int *n, int *cap, bool *ok)
{
    char body[64];
    if (ilen >= sizeof body)
        return false;
    memcpy(body, in, ilen);
    body[ilen] = '\0';
    char *d1 = strstr(body, "..");
    if (!d1)
        return false;
    *d1 = '\0';
    char *a = body, *b = d1 + 2, *st = strstr(b, "..");
    long step = 1;
    if (st) {
        *st = '\0';
        char *e;
        step = strtol(st + 2, &e, 10);
        if (*e || st[2] == '\0')
            return false;
        if (step < 0)
            step = -step;
        if (step == 0)
            step = 1;
    }
    char *ea, *eb;
    long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
    bool num = *a && *b && !*ea && !*eb;
    bool chr = !num && a[0] && !a[1] && b[0] && !b[1];
    if (!num && !chr)
        return false;
    if (chr)
        x = (unsigned char)a[0], y = (unsigned char)b[0];
    int width = 0;      // {01..10} は 0 で桁をそろえる
    if (num && ((a[0] == '0' && a[1]) || (a[0] == '-' && a[1] == '0') || (b[0] == '0' && b[1]) ||
                (b[0] == '-' && b[1] == '0')))
        width = (int)(strlen(a) > strlen(b) ? strlen(a) : strlen(b));
    for (long v = x;; v += x <= y ? step : -step) {
        if ((x <= y && v > y) || (x > y && v < y))
            break;
        char item[32];
        if (chr)
            snprintf(item, sizeof item, "%c", (char)v);
        else
            snprintf(item, sizeof item, "%0*ld", width, v);
        size_t len = plen + strlen(item) + strlen(post);
        char *t = malloc(len + 1);
        if (!t) {
            *ok = false;
            return true;
        }
        memcpy(t, pre, plen);
        strcpy(t + plen, item);
        strcat(t, post);
        *ok = brace_expand_into(t, list, n, cap);
        free(t);
        if (!*ok)
            return true;
    }
    return true;
}

// s の最初の展開できる { … } を開いて、できた語を list に（さらに中・後ろの { } も開く）
static bool brace_expand_into(const char *s, char ***list, int *n, int *cap)
{
    for (const char *p = s; *p; p++) {
        p = skip_mark(p);
        if (!p)
            break;
        if (*p != '{')
            continue;
        // 対応する } と、深さ 1 の , を探す
        int depth = 0;
        const char *commas[BRACE_MAX];
        int nc = 0;
        const char *q = p;
        for (; *q; q++) {
            q = skip_mark(q);
            if (!q)
                break;
            if (*q == '{')
                depth++;
            else if (*q == '}' && --depth == 0)
                break;
            else if (*q == ',' && depth == 1 && nc < BRACE_MAX)
                commas[nc++] = q;
        }
        if (!q || *q != '}')
            break;
        size_t plen = (size_t)(p - s);
        const char *post = q + 1;
        if (nc == 0) {
            bool ok = true;
            if (brace_range(s, plen, p + 1, (size_t)(q - p - 1), post, list, n, cap, &ok))
                return ok;
            continue;   // {x} は開かない（後ろの { を探す）
        }
        const char *start = p + 1;
        for (int i = 0; i <= nc; i++) {
            const char *end = i < nc ? commas[i] : q;
            size_t ilen = (size_t)(end - start);
            char *t = malloc(plen + ilen + strlen(post) + 1);
            if (!t)
                return false;
            memcpy(t, s, plen);
            memcpy(t + plen, start, ilen);
            strcpy(t + plen + ilen, post);
            bool ok = brace_expand_into(t, list, n, cap);
            free(t);
            if (!ok)
                return false;
            start = end + 1;
        }
        return true;
    }
    return brace_out(list, n, cap, s, strlen(s));
}

// ---- 語の展開 ----

// 1 つの語（raw は印付きの文字）を e に足す（語の区切りは emit_word。最後の語は呼んだ側が emit）
static bool expand_one(Expand *e, const char *raw, bool split)
{
    bool ok = true;
    char tmp[128];
    for (const char *p = raw; *p && ok; p++) {
        char c = *p;
        if (c == M_QUOTE) {
            e->started = true;
        } else if (c == M_LIT) {
            p++;
            ok = add_char(e, *p, true);
        } else if (c == M_VAR || c == M_QVAR) {
            const char *end = strchr(p, M_END);
            if (c == M_QVAR && end == p + 2 && p[1] == '@') {
                // "$@": 引数を 1 つずつの語に（前後の文字は最初・最後の語に付く）
                for (int i = 0; ok && i < pos_argc; i++) {
                    if (i > 0) {
                        e->started = true;
                        ok = emit_word(e);
                    }
                    if (ok)
                        ok = add_value(e, pos_argv[i], true, false);
                }
            } else {
                ok = add_value(e, var_value(p + 1, (size_t)(end - p - 1), tmp, sizeof tmp), c == M_QVAR, split);
            }
            p = end;
        } else if (c == M_CMD || c == M_QCMD || c == M_PARAM || c == M_QPARAM || c == M_ARITH) {
            const char *end = strchr(p, M_CEND);
            SBuf v = {0};
            if (c == M_CMD || c == M_QCMD) {
                char *cmd = strndup(p + 1, (size_t)(end - p - 1));
                v.p = cmd ? capture(cmd) : nullptr;
                free(cmd);
                ok = v.p != nullptr;
            } else if (c == M_ARITH) {
                ok = arith_value(p + 1, (size_t)(end - p - 1), &v);
            } else {
                ok = param_value(p + 1, (size_t)(end - p - 1), &v);
            }
            if (ok)
                ok = add_value(e, v.p ? v.p : "", c == M_QCMD || c == M_QPARAM, split);
            free(v.p);
            p = end;
        } else {
            ok = add_char(e, c, false);
        }
    }
    return ok;
}

// 語を展開して *out に（展開のない語はそのまま。*out は malloc したもので、呼んだ側が free。だめでも free する）。
// 展開した語の文字は *mem（malloc。呼んだ側が free）。語の数（-1 = だめ。exp_failed が立っていれば展開の誤り）
static int expand_tokens(const Token *in, int n, Token **out, char **mem)
{
    SBuf b = {0};
    Expand e = {.b = &b, .max = MAX_WORDS, .grow = true};
    uint8_t *from_buf = nullptr;    // 語ごとに、b の中の文字か（e.offs が正しいもの）
    int nfrom = 0;
    bool ok = true;
    *mem = nullptr;
    for (int i = 0; ok && i < n; i++) {
        if (in[i].kind != T_WORD || !in[i].has_exp) {
            if (!word_room(&e)) {
                ok = false;
                break;
            }
            e.out[e.k++] = in[i];
            continue;
        }
        // 分け直さない・ファイル名にしない・{a,b} を開かない: 代入の形、リダイレクトの行き先
        bool redirect = i > 0 && is_redirect(in[i - 1].kind) && in[i - 1].kind != T_ERR_TO_OUT;
        bool split = !in[i].assign && !redirect;
        e.do_glob = split;
        e.src = &in[i];
        int first = e.k;
        char **alts = nullptr;
        int nalt = 0, capalt = 0;
        if (in[i].brace && split)
            ok = brace_expand_into(in[i].s, &alts, &nalt, &capalt);
        else
            ok = brace_out(&alts, &nalt, &capalt, in[i].s, strlen(in[i].s));
        for (int k = 0; ok && k < nalt; k++) {
            e.start = b.len;
            e.started = false;
            e.glob = false;
            e.pat.len = 0;
            ok = expand_one(&e, alts[k], split) && emit_word(&e);
        }
        for (int k = 0; k < nalt; k++)
            free(alts[k]);
        free(alts);
        if (ok && e.k > nfrom) {
            uint8_t *f = realloc(from_buf, (size_t)e.cap);
            if (!f) {
                ok = false;
                break;
            }
            from_buf = f;
            memset(from_buf + nfrom, 0, (size_t)(e.k - nfrom));
            for (int k = first; k < e.k; k++)
                from_buf[k] = 1;
            nfrom = e.k;
        }
    }
    free(e.pat.p);
    if (!ok) {
        if (!exp_failed)
            fprintf(stderr, "too many words\n");
        free(b.p);
        free(e.offs);
        free(from_buf);
        *out = e.out;
        return -1;
    }
    for (int k = 0; k < nfrom; k++)
        if (from_buf[k])
            e.out[k].s = b.p + e.offs[k];
    free(e.offs);
    free(from_buf);
    *out = e.out;
    *mem = b.p;
    return e.k;
}

// 1 つの語を、分け直さず・ファイル名にせずに展開する（case の語とパターン）。pattern なら fnmatch の
// パターン（引用された * ? [ ] \ に \ を付けたもの）。malloc した文字列（だめなら NULL）
static char *expand_single(const Token *t, bool pattern)
{
    if (!t->has_exp)
        return strdup(t->s);
    SBuf b = {0};
    size_t off;
    Token out;
    Expand e = {.b = &b, .out = &out, .offs = &off, .max = 1, .src = t};
    char *r = nullptr;
    if (expand_one(&e, t->s, false))
        r = strdup(pattern ? (e.pat.p ? e.pat.p : "") : (b.p ? b.p : ""));
    free(e.pat.p);
    free(b.p);
    return r;
}

// ---- 実行 ----

// 1 つのコマンドを動かす（組み込みのコマンド、/apps のアプリ）。perf からも使う（shell.h）
static int call_function(int fi, int argc, char **argv);

int run_command(int argc, char **argv)
{
    int f = func_find(argv[0]);     // 関数がいちばん先（bash と同じ）
    if (f >= 0)
        return call_function(f, argc, argv);
    int i = find_builtin(argv[0]);
    if (i >= 0)
        return commands[i].fn(argc, argv);
    char path[LINE_MAX_LEN];
    if (find_app(argv[0], path, sizeof(path))) {
        argv[0] = path;     // プログラムには、パスを argv[0] として渡す
        return slimcc_run_source(path, nullptr, nullptr, argc, argv);   // ソースならその場でコンパイル（キャッシュあり）
    }
    fprintf(stderr, "%s: コマンドがありません（help で一覧）\n", argv[0]);
    return 127;
}

typedef struct {
    char **argv;                // 語の数だけ（malloc。stage_free）
    int argc;
    char **assign;              // コマンドの前の NAME=値
    int nassign;
    const char *in, *out, *err;
    const char *heredoc;        // <<WORD の本文（展開したもの）
    bool append, err_append, err_to_out;
} Stage;

// 繰り返しと中断: break・continue の残りの段数、動いている繰り返しの深さ、Ctrl-C
static int loop_depth, brk_count, cont_count;
static uint32_t intr_mark;      // この行を始めたときの、アプリのないときの Ctrl-C の数（uart_shell_interrupts）

// 行を途中でやめるか: break・continue の途中、アプリのないときの Ctrl-C（組み込みのコマンドだけの繰り返しも
// 止められるように）、Ctrl-C で止められたアプリ（130・131。bash と同じく、行の残りも動かさない）
static bool stop_now(void)
{
    return brk_count || cont_count || ret_pending || uart_shell_interrupts() != intr_mark || last_status == 130 ||
           last_status == 131;
}

// 展開した語（1 つのコマンドの分）を、引数・代入・リダイレクトに分ける。0、構文の誤りは 2
static void stage_free(Stage *s)
{
    free(s->argv);
    free(s->assign);
    s->argv = s->assign = nullptr;
}

static int build_stage(Token *t, int n, Stage *s)
{
    memset(s, 0, sizeof *s);
    s->argv = malloc(sizeof *s->argv * (size_t)(n + 1));
    s->assign = malloc(sizeof *s->assign * (size_t)(n + 1));
    if (!s->argv || !s->assign) {
        fprintf(stderr, "too many arguments\n");
        stage_free(s);
        return 2;
    }
    for (int i = 0; i < n; i++) {
        if (t[i].kind == T_WORD) {
            if (s->argc == 0 && t[i].assign) {
                s->assign[s->nassign++] = t[i].s;
                continue;
            }
            s->argv[s->argc++] = t[i].s;
        } else if (t[i].kind == T_ERR_TO_OUT) {
            s->err_to_out = true;
        } else {
            if (i + 1 == n || t[i + 1].kind != T_WORD) {
                fprintf(stderr, "syntax error near '%s'\n", token_text(&t[i]));
                return 2;
            }
            if (t[i].kind == T_IN) {
                s->in = t[++i].s;
                s->heredoc = nullptr;
            } else if (t[i].kind == T_HEREDOC) {
                s->heredoc = t[++i].s;
                s->in = nullptr;
            } else if (t[i].kind == T_ERR || t[i].kind == T_ERR_APPEND) {
                s->err_append = t[i].kind == T_ERR_APPEND;
                s->err = t[++i].s;
            } else {
                s->append = t[i].kind == T_APPEND;
                s->out = t[++i].s;
            }
        }
    }
    s->argv[s->argc] = nullptr;
    return 0;
}

static int exec_node(Parser *P, Node *n);
static int exec_compound(Parser *P, Node *n);

// パイプでつないだコマンド（N_PIPE の鎖か、1 つのコマンド）を動かす。最後のコマンドの終了状態
static int run_pipeline(Parser *P, Node *n)
{
    Node *stages[MAX_STAGES];
    int ns = 0;
    if (n->kind == N_PIPE) {
        for (Node *p = n; p; p = p->r) {
            if (ns == MAX_STAGES) {
                fprintf(stderr, "too many commands in a pipeline\n");
                return 2;
            }
            stages[ns++] = p->l;
        }
    } else {
        stages[ns++] = n;
    }
    int status = 0;
    char tmp[MAX_STAGES][16];
    for (int i = 0; i < ns; i++) {
        Node *c = stages[i];
        // 語は、その段を動かす直前に展開する
        Token *ex = nullptr;
        char *mem = nullptr;
        int a = c->kind == N_SIMPLE ? c->a : c->ra, b = c->kind == N_SIMPLE ? c->b : c->rb;
        subst_status = -1;
        exp_failed = false;
        int m = expand_tokens(P->t + a, b - a, &ex, &mem);
        Stage s = {0};
        int r = m < 0 ? 1 : build_stage(ex, m, &s);
        snprintf(tmp[i], sizeof(tmp[i]), "/tmp/.pipe%d", i + nest * MAX_STAGES);   // 入れ子の中のパイプは別の名前
        char here[24] = "";
        if (r == 0 && s.heredoc) {
            // ヒアドキュメント: 本文を一時ファイルに書いて、標準入力にする
            snprintf(here, sizeof here, "/tmp/.here%d", i + nest * MAX_STAGES);
            FILE *hf = fopen(here, "w");
            if (!hf || fputs(s.heredoc, hf) < 0) {
                fprintf(stderr, "%s: %s\n", here, strerror(errno));
                r = 1;
            }
            if (hf)
                fclose(hf);
            s.in = here;
        }
        if (r != 0) {
            if (here[0])
                unlink(here);
            stage_free(&s);
            free(mem);
            free(ex);
            status = r;
            break;
        }
        const char *in = s.in ? s.in : i > 0 ? tmp[i - 1] : nullptr;
        const char *out = s.out ? s.out : i + 1 < ns ? tmp[i] : nullptr;
        bool ok = true;
        if (in && !console_redirect_in(in)) {
            fprintf(stderr, "%s: %s\n", in, strerror(errno));
            ok = false;
        }
        if (ok && out && !console_redirect_out(out, s.out && s.append)) {
            fprintf(stderr, "%s: %s\n", out, strerror(errno));
            ok = false;
        }
        if (ok && (s.err || s.err_to_out) && !console_redirect_err(s.err_to_out ? nullptr : s.err, s.err_append)) {
            fprintf(stderr, "%s: %s\n", s.err, strerror(errno));
            ok = false;
        }
        if (!ok) {
            status = 1;
        } else if (c->kind != N_SIMPLE) {
            // if・for・while: リダイレクトを土台にして中を動かす（中のコマンドが終わっても、この向け先に戻る）
            if (console_redirect_push()) {
                status = exec_compound(P, c);
                console_redirect_pop();
            } else {
                status = 1;
            }
        } else if (s.argc == 0) {
            // 代入だけ（NAME=値）: シェルの変数にする。$? は $(...) の値（なければ 0。bash と同じ）
            for (int k = 0; k < s.nassign; k++) {
                char *eq = strchr(s.assign[k], '=');
                *eq = '\0';
                set_var(s.assign[k], eq + 1);
                *eq = '=';
            }
            status = subst_status >= 0 ? subst_status : 0;
        } else {
            // コマンドの前の NAME=値: そのコマンドの間だけ環境変数にして、終わったら戻す（bash と同じ）
            char *saved[s.nassign + 1];
            for (int k = 0; k < s.nassign; k++) {
                char *eq = strchr(s.assign[k], '=');
                *eq = '\0';
                const char *old = getenv(s.assign[k]);
                saved[k] = old ? strdup(old) : nullptr;
                setenv(s.assign[k], eq + 1, 1);
                *eq = '=';
            }
            // 向け先を変えたときは、それを土台にして動かす（関数・sh・source の中のコマンドが終わっても、
            // この向け先に戻るように）
            if (in || out || s.err || s.err_to_out) {
                if (console_redirect_push()) {
                    status = run_command(s.argc, s.argv);
                    console_redirect_pop();
                } else {
                    status = 1;
                }
            } else {
                status = run_command(s.argc, s.argv);
            }
            for (int k = s.nassign - 1; k >= 0; k--) {
                char *eq = strchr(s.assign[k], '=');
                *eq = '\0';
                if (saved[k])
                    setenv(s.assign[k], saved[k], 1);
                else
                    unsetenv(s.assign[k]);
                *eq = '=';
                free(saved[k]);
            }
        }
        console_redirect_end();
        if (here[0])
            unlink(here);
        stage_free(&s);
        free(mem);
        free(ex);
        if (status < 0)
            status = 1;     // 動かせなかった（コンパイルの誤りなど）
        status &= 0xFF;
        if (i + 1 < ns && s.out) {
            // 出力をファイルに向けた段の次は、空の入力（bash と同じ）
            int fd = open(tmp[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0)
                close(fd);
        }
        if (!ok)
            break;
    }
    for (int i = 0; i + 1 < ns; i++)
        unlink(tmp[i]);
    return status;
}

// 繰り返しの 1 回が終わった後: true なら繰り返しをやめる（break、外の繰り返しへの continue、中断）
static bool loop_after_body(void)
{
    if (brk_count) {
        brk_count--;
        return true;
    }
    if (cont_count) {
        if (--cont_count > 0)
            return true;    // continue N: 外の繰り返しを続ける
        return false;
    }
    return stop_now();
}

static int exec_node(Parser *P, Node *n)
{
    // 後ろにリダイレクトのある if・for・{ }・( ) などは、パイプラインの段として向け先を決めてから動かす
    bool compound = n->kind != N_SIMPLE && n->kind != N_PIPE && n->kind != N_SEQ && n->kind != N_AND &&
                    n->kind != N_OR && n->kind != N_NOT && n->kind != N_TIME;
    if (compound && n->rb > n->ra) {
        int status = run_pipeline(P, n);
        last_status = status;
        return status;
    }
    return exec_compound(P, n);
}

static int exec_compound(Parser *P, Node *n)
{
    int status = 0;
    switch (n->kind) {
    case N_SEQ:
        status = exec_node(P, n->l);
        if (!stop_now())
            status = exec_node(P, n->r);
        return status;
    case N_AND:
    case N_OR:
        status = exec_node(P, n->l);
        if (!stop_now() && ((n->kind == N_AND) == (status == 0)))
            status = exec_node(P, n->r);
        return status;
    case N_NOT:
        status = !exec_node(P, n->l);
        last_status = status;
        return status;
    case N_TIME: {
        TimeMark t0, t1;
        time_mark(&t0);
        status = exec_node(P, n->l);
        time_mark(&t1);
        time_report(&t0, &t1, n->a);
        last_status = status;
        return status;
    }
    case N_IF:
        status = exec_node(P, n->l);
        if (stop_now())
            return status;
        if (status == 0)
            return exec_node(P, n->r);
        if (n->x)
            return exec_node(P, n->x);
        last_status = 0;
        return 0;
    case N_FOR: {
        // 語を先に展開する（in がなければ、位置の引数（ない）を回すので何もしない）
        Token *ex = nullptr;
        char *mem = nullptr;
        exp_failed = false;
        int m = n->for_in ? expand_tokens(P->t + n->wa, n->wb - n->wa, &ex, &mem) : 0;
        if (m < 0) {
            free(ex);
            last_status = 1;
            return 1;
        }
        const char *name = P->t[n->a].s;
        loop_depth++;
        status = 0;
        last_status = 0;
        for (int i = 0; i < m; i++) {
            set_var(name, ex[i].s);
            status = exec_node(P, n->r);
            if (loop_after_body())
                break;
        }
        loop_depth--;
        free(mem);
        free(ex);
        return status;
    }
    case N_WHILE:
    case N_UNTIL:
        loop_depth++;
        status = 0;
        for (;;) {
            int c = exec_node(P, n->l);
            if (stop_now()) {
                if (loop_after_body())
                    break;
                continue;
            }
            if ((c == 0) != (n->kind == N_WHILE))
                break;
            status = exec_node(P, n->r);
            if (loop_after_body())
                break;
        }
        loop_depth--;
        last_status = status;
        return status;
    case N_GROUP:
        return exec_node(P, n->l);
    case N_SUBSHELL: {
        // ( … ): 変数・関数・環境変数・カレントディレクトリを、終わったら戻す
        char cwd[512];
        bool have_cwd = getcwd(cwd, sizeof cwd) != nullptr;
        ShellSaved saved;
        shell_save(&saved, true, true);
        int depth = loop_depth;
        loop_depth = 0;
        status = exec_node(P, n->l);
        loop_depth = depth;
        brk_count = cont_count = 0;
        shell_restore(&saved);
        if (have_cwd)
            chdir(cwd);
        last_status = status;
        return status;
    }
    case N_CASE: {
        exp_failed = false;
        char *word = expand_single(&P->t[n->a], false);
        status = 0;
        bool matched = false;
        for (Node *it = n->x; word && it && !matched; it = it->x) {
            for (int k = it->wa; k < it->wb && !matched; k++) {
                if (P->t[k].kind != T_WORD)
                    continue;
                char *pat = expand_single(&P->t[k], true);
                matched = pat && fnmatch(pat, word, 0) == 0;
                free(pat);
            }
            if (matched && it->l)
                status = exec_node(P, it->l);
        }
        free(word);
        last_status = status;
        return status;
    }
    case N_FUNCDEF: {
        // 本体を元の文の文字列で覚える
        const Token *a = &P->t[n->wa], *b = &P->t[n->wb - 1];
        func_set(P->t[n->a].s, P->text + a->off, (size_t)(b->end - a->off));
        last_status = 0;
        return 0;
    }
    default:    // N_SIMPLE、N_PIPE
        status = run_pipeline(P, n);
        last_status = status;
        return status;
    }
}

// ---- 関数を呼ぶ ----

// local で変えた変数（関数が終わったら戻す）
typedef struct {
    char *name;
    char *env_old;      // 前の環境変数の値（なければ NULL）
    char *var_old;      // 前のシェルの変数の値（なければ NULL）
} LocalSave;
static LocalSave *locals;
static int nlocals, caplocals;

static int call_function(int fi, int argc, char **argv)
{
    if (func_depth >= 100) {
        fprintf(stderr, "%s: maximum function nesting level exceeded\n", argv[0]);
        return 1;
    }
    char *body = strdup(funcs[fi].body);    // 中で定義し直しても困らないように
    if (!body)
        return 1;
    char **old_pos;
    int old_posc;
    pos_set(argc - 1, argv + 1, &old_pos, &old_posc);
    int local_mark = nlocals;
    func_depth++;
    int status = run_text(body);
    func_depth--;
    if (ret_pending) {
        status = ret_status;
        ret_pending = false;
    }
    // local を戻す（新しいものから）
    while (nlocals > local_mark) {
        LocalSave *l = &locals[--nlocals];
        if (l->env_old)
            setenv(l->name, l->env_old, 1);
        else
            unsetenv(l->name);
        if (l->var_old)
            svar_set(l->name, l->var_old);
        else
            svar_unset(l->name);
        free(l->name);
        free(l->env_old);
        free(l->var_old);
    }
    pos_free(pos_argv, pos_argc);
    pos_argv = old_pos;
    pos_argc = old_posc;
    free(body);
    last_status = status;
    return status;
}

// local NAME[=値]...: この関数の間だけの変数
static int cmd_local(int argc, char **argv)
{
    if (func_depth == 0) {
        fprintf(stderr, "local: can only be used in a function\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        size_t nl = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)nl, argv[i]);
        if (!name_start(name[0])) {
            fprintf(stderr, "local: '%s': not a valid identifier\n", argv[i]);
            return 1;
        }
        if (nlocals == caplocals) {
            int cap = caplocals ? caplocals * 2 : 8;
            LocalSave *p = realloc(locals, (size_t)cap * sizeof *p);
            if (!p)
                return 1;
            locals = p;
            caplocals = cap;
        }
        const char *e = getenv(name), *v = svar_get(name);
        locals[nlocals++] = (LocalSave){strdup(name), e ? strdup(e) : nullptr, v ? strdup(v) : nullptr};
        if (eq)
            set_var(name, eq + 1);
        else if (!e && !v)
            svar_set(name, "");
    }
    return 0;
}

// ファイルを読む（malloc。だめなら NULL を返してエラーを出す）
static char *read_script(const char *cmd, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s: %s: %s\n", cmd, path, strerror(errno));
        return nullptr;
    }
    SBuf b = {0};
    char buf[1024];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0)
        if (!sb_add(&b, buf, k))
            break;
    fclose(f);
    if (!b.p)
        sb_add(&b, "", 0);
    // 先頭の #!… の行は注釈（# なので、そのままでよい）
    return b.p;
}

// source FILE [ARG...]・. FILE: 今のシェルで動かす（変数・関数・cd が残る）
static int cmd_source(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "%s: filename argument required\n", argv[0]);
        return 2;
    }
    char *text = read_script(argv[0], argv[1]);
    if (!text)
        return 1;
    char **old_pos = nullptr;
    int old_posc = 0;
    bool args = argc > 2;
    if (args)
        pos_set(argc - 2, argv + 2, &old_pos, &old_posc);
    int status = run_text(text);
    if (args) {
        pos_free(pos_argv, pos_argc);
        pos_argv = old_pos;
        pos_argc = old_posc;
    }
    free(text);
    return status;
}

// sh FILE [ARG...]・sh -c 'コマンド' [ARG0 ARG...]: 別のシェルとして動かす（変数・関数・cd を戻す。export
// していない変数・関数は見えない。Linux の sh と同じ）
static int cmd_sh(int argc, char **argv)
{
    char *text = nullptr;
    int first = 2;
    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        text = strdup(argv[2]);
        first = 4;      // sh -c CMD ARG0 ARG1...（$1 は ARG1）
    } else if (argc >= 2 && argv[1][0] != '-') {
        text = read_script(argv[0], argv[1]);
    } else {
        fprintf(stderr, "usage: sh FILE [ARG...] / sh -c COMMAND [ARG0 ARG...]\n");
        return 2;
    }
    if (!text)
        return 127;
    char cwd[512];
    bool have_cwd = getcwd(cwd, sizeof cwd) != nullptr;
    ShellSaved saved;
    shell_save(&saved, false, true);
    if (argc > first)
        pos_set(argc - first, argv + first, nullptr, nullptr);
    int depth = func_depth;
    func_depth = 0;
    nest++;
    int status = run_text(text);
    nest--;
    func_depth = depth;
    ret_pending = false;
    shell_restore(&saved);
    if (have_cwd)
        chdir(cwd);
    free(text);
    return status;
}

// return [N]
static int cmd_return(int argc, char **argv)
{
    if (func_depth == 0) {
        fprintf(stderr, "return: can only 'return' from a function\n");
        return 1;
    }
    ret_status = argc > 1 ? atoi(argv[1]) & 0xFF : last_status;
    ret_pending = true;
    return ret_status;
}

// shift [N]
static int cmd_shift(int argc, char **argv)
{
    int k = argc > 1 ? atoi(argv[1]) : 1;
    if (k < 0 || k > pos_argc)
        return 1;
    for (int i = 0; i < k; i++)
        free(pos_argv[i]);
    memmove(pos_argv, pos_argv + k, sizeof(char *) * (size_t)(pos_argc - k));
    pos_argc -= k;
    return 0;
}

// set -- ARG...（位置の引数を変える）、set（変数の一覧）。ほかのオプション（-e など）はない
static int cmd_set(int argc, char **argv)
{
    if (argc == 1) {
        extern char **environ;
        for (char **e = environ; e && *e; e++)
            printf("%s\n", *e);
        for (int i = 0; i < nsvars; i++)
            printf("%s=%s\n", svars[i].name, svars[i].value);
        return 0;
    }
    if (!strcmp(argv[1], "--")) {
        pos_set(argc - 2, argv + 2, nullptr, nullptr);
        return 0;
    }
    fprintf(stderr, "set: %s: options are not supported (only set -- ARG...)\n", argv[1]);
    return 2;
}

// 文を切り分けて木にする。成功で 0（*root は空の文なら NULL）、TOK_ERROR・TOK_INCOMPLETE
typedef struct {
    Parser p;
    char *words;
} Parsed;

static int parse_text(const char *text, Parsed *pd, Node **root)
{
    size_t len = strlen(text);
    size_t wsize = len * 3 + 16;   // 引用符を外した語と、印と、語の終わりの NUL
    if (wsize < LINE_MAX_LEN * 2)
        wsize = LINE_MAX_LEN * 2;
    // 字句の数は文の長さを超えない（どの字句も 1 文字以上）。節は字句から作るので、字句の数の 4 倍を超えない。
    // 前は字句・節とも 512 個までで、長い sh のスクリプト（137 行）が「too many words」で動かなかった（2026-11-20）。
    // 節は互いに指し合うので、後から広げずに、字句に分けた後で要る分を取る
    int max_tok = len + 16 > MAX_TOKENS ? (int)(len + 16) : MAX_TOKENS;
    memset(pd, 0, sizeof *pd);
    pd->words = malloc(wsize);
    pd->p.t = malloc(sizeof(Token) * (size_t)max_tok);
    if (!pd->words || !pd->p.t)
        return TOK_ERROR;
    int n = tokenize_line(text, pd->p.t, max_tok, pd->words, wsize);
    if (n < 0)
        return n;
    pd->p.cap_nodes = n * 4 + 16 > MAX_NODES ? n * 4 + 16 : MAX_NODES;
    pd->p.nodes = malloc(sizeof(Node) * (size_t)pd->p.cap_nodes);
    if (!pd->p.nodes)
        return TOK_ERROR;
    pd->p.n = n;
    pd->p.text = text;
    *root = parse_list(&pd->p, nullptr);
    if (pd->p.err)
        return pd->p.err;
    return 0;
}

static void parsed_free(Parsed *pd)
{
    free(pd->words);
    free(pd->p.t);
    free(pd->p.nodes);
}

// 文（1 行か、改行を含む複数の行）を動かす。最後の終了状態
static int run_text(const char *text)
{
    Parsed pd;
    Node *root = nullptr;
    int r = parse_text(text, &pd, &root);
    int status = 0;
    if (r == TOK_INCOMPLETE) {
        if (!quiet_errors)
            fprintf(stderr, "syntax error: unexpected end of input\n");
        status = 2;
    } else if (r != 0) {
        status = 2;
    } else if (root) {
        // 繰り返し・中断の状態は、この文の中だけ（入れ子の $(...)・system() は外のものを持ち越さない）
        int depth = loop_depth, brk = brk_count, cont = cont_count;
        uint32_t mark = intr_mark;
        loop_depth = brk_count = cont_count = 0;
        intr_mark = uart_shell_interrupts();
        status = exec_node(&pd.p, root);
        if (uart_shell_interrupts() != intr_mark) {     // アプリのないところで Ctrl-C（bash と同じく 130）
            printf("^C\n");
            status = last_status = 130;
        }
        loop_depth = depth;
        brk_count = brk;
        cont_count = cont;
        intr_mark = mark;
    }
    if (r != 0)
        last_status = status;
    parsed_free(&pd);
    return status;
}

static int run_line(const char *line)
{
    return run_text(line);
}

// text の続きがいるか（if が閉じていない、引用符が閉じていない、行の終わりが | && || \ など）
static bool text_incomplete(const char *text)
{
    Parsed pd;
    Node *root;
    quiet_errors = true;
    int r = parse_text(text, &pd, &root);
    quiet_errors = false;
    parsed_free(&pd);
    return r == TOK_INCOMPLETE;
}

// ---- test・[・break・continue ----

static bool test_unary(const char *op, const char *arg, int *err)
{
    struct stat st;
    if (!strcmp(op, "-n"))
        return *arg != '\0';
    if (!strcmp(op, "-z"))
        return *arg == '\0';
    bool ok = stat(arg, &st) == 0;
    if (!strcmp(op, "-e") || !strcmp(op, "-r") || !strcmp(op, "-x"))
        return ok;
    if (!strcmp(op, "-f"))
        return ok && S_ISREG(st.st_mode);
    if (!strcmp(op, "-d"))
        return ok && S_ISDIR(st.st_mode);
    if (!strcmp(op, "-s"))
        return ok && st.st_size > 0;
    if (!strcmp(op, "-w"))
        return ok && (st.st_mode & 0200);
    if (!strcmp(op, "-L") || !strcmp(op, "-h") || !strcmp(op, "-b") || !strcmp(op, "-c") || !strcmp(op, "-p") ||
        !strcmp(op, "-S") || !strcmp(op, "-t"))
        return false;
    *err = 1;
    return false;
}

static bool is_unary_op(const char *s)
{
    static const char *const ops[] = {"-n", "-z", "-e", "-r", "-x", "-f", "-d", "-s", "-w", "-L", "-h",
                                      "-b", "-c", "-p", "-S", "-t", nullptr};
    for (int i = 0; ops[i]; i++)
        if (!strcmp(s, ops[i]))
            return true;
    return false;
}

static bool is_binary_op(const char *s)
{
    static const char *const ops[] = {"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
                                      "-nt", "-ot", nullptr};
    for (int i = 0; ops[i]; i++)
        if (!strcmp(s, ops[i]))
            return true;
    return false;
}

static bool test_int(const char *s, long long *v)
{
    while (*s == ' ')
        s++;
    char *end;
    *v = strtoll(s, &end, 10);
    while (*end == ' ')
        end++;
    return end != s && !*end;
}

static bool test_binary(const char *a, const char *op, const char *b, int *err)
{
    if (!strcmp(op, "=") || !strcmp(op, "=="))
        return !strcmp(a, b);
    if (!strcmp(op, "!="))
        return strcmp(a, b) != 0;
    if (!strcmp(op, "<"))
        return strcmp(a, b) < 0;
    if (!strcmp(op, ">"))
        return strcmp(a, b) > 0;
    if (!strcmp(op, "-nt") || !strcmp(op, "-ot")) {
        struct stat sa, sb;
        bool ha = stat(a, &sa) == 0, hb = stat(b, &sb) == 0;
        if (!strcmp(op, "-nt"))
            return ha && (!hb || sa.st_mtime > sb.st_mtime);
        return hb && (!ha || sa.st_mtime < sb.st_mtime);
    }
    long long x, y;
    if (!test_int(a, &x) || !test_int(b, &y)) {
        fprintf(stderr, "test: %s: integer expression expected\n", !test_int(a, &x) ? a : b);
        *err = 1;
        return false;
    }
    if (!strcmp(op, "-eq"))
        return x == y;
    if (!strcmp(op, "-ne"))
        return x != y;
    if (!strcmp(op, "-lt"))
        return x < y;
    if (!strcmp(op, "-le"))
        return x <= y;
    if (!strcmp(op, "-gt"))
        return x > y;
    return x >= y;
}

typedef struct {
    char **v;
    int n, i;
    int err;
} TestArgs;

static bool test_or(TestArgs *t);

static bool test_primary(TestArgs *t)
{
    if (t->i >= t->n) {
        t->err = 1;
        return false;
    }
    const char *a = t->v[t->i];
    if (!strcmp(a, "!")) {
        t->i++;
        return !test_primary(t);
    }
    if (!strcmp(a, "(") && t->n - t->i >= 3) {
        t->i++;
        bool r = test_or(t);
        if (t->i >= t->n || strcmp(t->v[t->i], ")")) {
            t->err = 1;
            return false;
        }
        t->i++;
        return r;
    }
    if (t->n - t->i >= 3 && is_binary_op(t->v[t->i + 1])) {
        bool r = test_binary(a, t->v[t->i + 1], t->v[t->i + 2], &t->err);
        t->i += 3;
        return r;
    }
    if (is_unary_op(a) && t->n - t->i >= 2) {
        bool r = test_unary(a, t->v[t->i + 1], &t->err);
        t->i += 2;
        return r;
    }
    t->i++;
    return *a != '\0';
}

static bool test_and(TestArgs *t)
{
    bool r = test_primary(t);
    while (t->i < t->n && !strcmp(t->v[t->i], "-a")) {
        t->i++;
        bool s = test_primary(t);
        r = r && s;
    }
    return r;
}

static bool test_or(TestArgs *t)
{
    bool r = test_and(t);
    while (t->i < t->n && !strcmp(t->v[t->i], "-o")) {
        t->i++;
        bool s = test_and(t);
        r = r || s;
    }
    return r;
}

// test EXPR・[ EXPR ]（POSIX の test。-e -f -d -s -n -z -r -w -x、= != < >、-eq -ne -lt -le -gt -ge、-nt -ot、
// ! -a -o ( )）。真なら 0、偽なら 1、誤りは 2
static int cmd_test(int argc, char **argv)
{
    if (!strcmp(argv[0], "[")) {
        if (argc < 2 || strcmp(argv[argc - 1], "]")) {
            fprintf(stderr, "[: missing ']'\n");
            return 2;
        }
        argc--;
    }
    TestArgs t = {argv + 1, argc - 1, 0, 0};
    if (t.n == 0)
        return 1;
    bool r = test_or(&t);
    if (t.err || t.i != t.n) {
        if (!t.err || t.i != t.n)
            fprintf(stderr, "test: syntax error\n");
        return 2;
    }
    return r ? 0 : 1;
}

// break [N]・continue [N]
static int loop_count_arg(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1) {
        fprintf(stderr, "%s: %s: loop count out of range\n", argv[0], argv[1]);
        return -1;
    }
    if (loop_depth == 0) {
        fprintf(stderr, "%s: only meaningful in a 'for', 'while', or 'until' loop\n", argv[0]);
        return 0;
    }
    return n > loop_depth ? loop_depth : n;
}

static int cmd_break(int argc, char **argv)
{
    int n = loop_count_arg(argc, argv);
    if (n > 0)
        brk_count = n;
    return n < 0 ? 1 : 0;
}

static int cmd_continue(int argc, char **argv)
{
    int n = loop_count_arg(argc, argv);
    if (n > 0)
        cont_count = n;
    return n < 0 ? 1 : 0;
}

// POSIX の system(): アプリの中からコマンドの行を動かして、終わるまで待つ（OS API 表の system）。
// シェルの行と同じ書き方（パイプ、リダイレクト、&&・||・;、組み込みのコマンド、PATH のアプリ）。
// Monocs にはプロセスがないので、アプリの中で、シェルの行の実行を入れ子で呼ぶ（アプリの入れ子は os/kernel/app.c）。
//   - 終わったら、カレントディレクトリと環境変数を元に戻す（Linux で、子のプロセスの cd・export が親に効かない
//     のと同じ。環境変数は os/libc/env.c）
//   - 戻り値は Linux と同じ形（終了の値 << 8。WEXITSTATUS で取り出す）。終了の値はシェルの $? と同じ（動かせな
//     かった 1、ないコマンド 127、例外 139、Ctrl-C 130、timeout 124）。cmd が NULL なら 1（シェルがある）
//   - 子のコマンドの解析・コンパイルの間は、Ctrl-C を後回しにする（外側のアプリが止まらないように）。子のアプリの
//     間の Ctrl-C は子だけを止める
//   - コア 0 のアプリからだけ使える（ほかのコアの仕事からは -1、errno は ENOSYS）
//   - in_path・out_path（popen。os/libc/posix.c）: 行全体の標準入力・標準出力をそのファイルに向ける（NULL なら
//     外側のまま）。行の中のリダイレクトのほうが勝つ
int shell_system_redirect(const char *cmd, const char *in_path, const char *out_path)
{
    if (!cmd)
        return 1;
    if (cpu_id() != 0 || !app_is_running()) {
        errno = ENOSYS;
        return -1;
    }
    char cwd[512];
    bool have_cwd = getcwd(cwd, sizeof cwd) != nullptr;
    fflush(stdout);
    fflush(stderr);
    abort_defer_begin();
    malloc_kernel_begin();      // シェルとコンパイラの確保は、外側のアプリのヒープに入れない
    env_app_begin();            // 行の export は、終わったら戻す
    env_snapshot_now();
    int status = 1;
    if (console_redirect_push()) {
        // 行全体のリダイレクト: もう 1 段積んで、子のコマンドの土台にする（行の中の各コマンドは、終わると土台に戻る）
        bool ok = (!out_path || console_redirect_out(out_path, false)) && (!in_path || console_redirect_in(in_path));
        bool pushed = ok && (in_path || out_path) ? console_redirect_push() : ok;
        if (pushed) {
            int outer_status = last_status;   // 子のシェルの $? は 0 から（Linux の sh -c と同じ）
            last_status = 0;
            ShellSaved vars;
            shell_save(&vars, false, false);  // export していない変数は子に見えない（環境変数は env_app_end が戻す）
            nest++;
            status = run_line(cmd);
            nest--;
            shell_restore(&vars);
            last_status = outer_status;
            if (in_path || out_path)
                console_redirect_pop();
        }
        console_redirect_pop();
    }
    env_app_end();
    if (have_cwd)
        chdir(cwd);
    malloc_kernel_end();
    abort_defer_end();
    // 外側の timeout の期限が過ぎていたら（子が止められて戻ってきた）、このアプリも止める。タイマー割り込みを
    // 待つと、その間（10ms まで）に呼んだアプリが動き続ける
    if (lock_depth(0) == 0 && app_timeout_due()) {
        curses_emergency_restore();
        app_abort_timeout();
    }
    return (status < 0 ? 1 : status & 0xFF) << 8;
}

int shell_system(const char *cmd)
{
    return shell_system_redirect(cmd, nullptr, nullptr);
}

int shell_last_status(void)
{
    return shell_status;
}

// ssh の exec（`ssh monocs 'ls /'`。os/kernel/rexec.c）: プロンプトで入力を待つ間に、頼まれた行を動かす。
// 行の標準出力・標準エラーは exec の出力の輪へ、標準入力は exec の入力の輪から（行の中のリダイレクトが勝つ）。
// サブシェル（( … )）と同じく、cd・変数・関数は終わったら戻す。$? と、打ち込んだ行の続き（行の編集）は変えない。
// 画面には何も出さない（アプリが直接書く例外の表示などを除く）
static void shell_rexec_poll(void)
{
    const char *cmd = rexec_take();
    if (!cmd)
        return;
    line_edit_set_idle(nullptr);    // 行の中で行の編集を使っても、入れ子にしない
    fflush(stdout);
    char cwd[512];
    bool have_cwd = getcwd(cwd, sizeof cwd) != nullptr;
    ShellSaved saved;
    shell_save(&saved, true, true);
    int outer_last = last_status, outer_shell = shell_status;
    last_status = 0;
    int st = 1;
    if (console_redirect_push()) {
        console_redirect_rexec();
        if (console_redirect_push()) {
            nest++;
            st = run_text(cmd);
            nest--;
            if (ret_pending) {      // exit N
                st = ret_status;
                ret_pending = false;
            }
            fflush(stdout);
            fflush(stderr);
            console_redirect_pop();
        }
        console_redirect_pop();
    }
    shell_restore(&saved);
    if (have_cwd)
        chdir(cwd);
    last_status = outer_last;
    shell_status = outer_shell;
    rexec_done(st < 0 ? 1 : st);
    line_edit_set_idle(shell_rexec_poll);
}

// path の中身を消す（フォルダは中から。path そのものは残す）。起動のときの /tmp 用
static void clear_dir(const char *path, int depth)
{
    for (;;) {  // 読みながら消すと読み飛ばしうるので、名前を少しずつ集めてから消す（開き直して繰り返す）
        DIR *d = opendir(path);
        if (!d)
            return;
        char names[32][256];    // 入れ子で呼ぶので static にしない
        int n = 0;
        struct dirent *e;
        while (n < 32 && (e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                snprintf(names[n++], sizeof names[0], "%s", e->d_name);
        closedir(d);
        if (n == 0)
            return;
        int removed = 0;
        for (int i = 0; i < n; i++) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s", path, names[i]);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (depth < 8)
                    clear_dir(p, depth + 1);
                removed += rmdir(p) == 0;
            } else {
                removed += remove(p) == 0;
            }
        }
        if (removed == 0)
            return;     // 消せないものしか残っていない（止まらないように）
    }
}

[[noreturn]] void shell_main(void)
{
    char line[LINE_MAX_LEN];
    static char text[TEXT_MAX];     // 続きの行（if・for の中、閉じていない引用符、行の終わりの \）をつないだもの

    clear_dir("/tmp", 0);  // 起動のたびに /tmp を空にする（Linux の tmpfs と同じく、前の起動の一時ファイルを残さない）
    mkdir("/tmp", 0777);   // /tmp はいつもある（パイプの一時ファイル、mkstemp("/tmp/…") など）
    mkdir("/root", 0755);  // ホーム（HOME=/root。.profile・.shell_history・.ssh など）
    // /root/.profile があれば、コマンドとして動かす（export PATH=... など。Linux の .profile と同じ。# は注釈。
    // if・for などは複数の行にまたがってよい）
    FILE *profile = fopen("/root/.profile", "r");
    if (profile) {
        char pl[LINE_MAX_LEN];
        text[0] = '\0';
        while (fgets(pl, sizeof(pl), profile)) {
            pl[strcspn(pl, "\r\n")] = '\0';
            if (strlen(text) + strlen(pl) + 2 > sizeof text) {
                fprintf(stderr, "/root/.profile: too long\n");
                text[0] = '\0';
                continue;
            }
            if (text[0])
                strcat(text, "\n");
            strcat(text, pl);
            if (!text_incomplete(text)) {
                run_line(text);
                text[0] = '\0';
            }
        }
        if (text[0])
            run_line(text);     // 閉じていないまま終わった（誤りを表示する）
        fclose(profile);
    }

    // /services.txt に書いたサービスを始める（os/kernel/svc.c）
    clock_restore();    // 前に覚えた時刻（/etc/fake-hwclock.data）から始める（os/kernel/clock.c）
    svc_autostart();

    // SD カードに /autorun.c があれば、起動時にコンパイルして動かす（実機の確かめ用のイメージ。tools/mkimg.sh）
    struct stat ast;
    if (stat("/autorun.c", &ast) == 0) {
        printf("running /autorun.c\n");
        char *args[] = { "/autorun.c", nullptr };
        slimcc_run_source("/autorun.c", nullptr, nullptr, 1, args);
    }

    line_edit_set_idle(shell_rexec_poll);   // プロンプトで待つ間に ssh の exec の行を動かす（read_text などの行の編集は
                                            // コマンドの中なので、そこでは動かさない: 下で外す）
    printf("\nMonocs shell. \"help\" でコマンドの一覧を表示します\n");
    klog_boot_done();   // ここまでの表示（起動の様子）は dmesg に残る。この先は、できごとだけを記録する
    klog_file_boot();   // SD カードの /var/log/dmesg にも残す（前の起動のものは dmesg.1。os/kernel/klog_file.c）
    for (;;) {
        char cwd[LINE_MAX_LEN], prompt[LINE_MAX_LEN + 4];
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        snprintf(prompt, sizeof(prompt), "%s> ", cwd);
        fflush(stdout);

        static const LineCompleter completer = {command_name};
        int n = line_edit(prompt, line, sizeof(line), &completer, true);
        if (n == LINE_EDIT_EOF && contap_request_hangup())
            printf("logout\n");     // 行頭の Ctrl-D: telnet・ssh なら exit と同じ（bash と同じ）。シリアルでは何もしない
        if (n < 0)
            continue;
        line_edit_add_history(line);
        snprintf(text, sizeof text, "%s", line);
        // 続きがいる（if・for が閉じていない、引用符が閉じていない、行の終わりが | && || \）: "> " で続きを読む
        while (text_incomplete(text)) {
            int m = line_edit("> ", line, sizeof(line), &completer, true);
            if (m == LINE_EDIT_CANCEL) {
                text[0] = '\0';
                break;
            }
            if (m == LINE_EDIT_EOF) {
                fprintf(stderr, "syntax error: unexpected end of file\n");
                last_status = shell_status = 2;
                text[0] = '\0';
                break;
            }
            if (strlen(text) + strlen(line) + 2 > sizeof text) {
                fprintf(stderr, "too long\n");
                text[0] = '\0';
                break;
            }
            line_edit_add_history(line);
            strcat(text, "\n");
            strcat(text, line);
        }
        if (text[0]) {
            line_edit_set_idle(nullptr);
            shell_status = run_line(text);
            line_edit_set_idle(shell_rexec_poll);
        }
    }
}
