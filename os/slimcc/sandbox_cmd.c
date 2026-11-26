// シェルの sandbox（2026-11-26。docs/sandbox.md の 8 の 3）: C のソースをサンドボックス（EL0）で動かす。
//   sandbox FILE.c [引数...]
// FILE.c と、サンドボックスの中の C ライブラリ（/usr/lib/sandbox/*.c。lib/sandbox）をコンパイルし、サンドボックスの
// 領域の中でつなぐ（OS の関数にはつながない: 足りない関数は undefined symbol になる）。入口は __sandbox_start
// （lib/sandbox/start.c）。今のライブラリは、画面に書く（printf など）・文字列・malloc・exit だけ。
// 終了の値: プログラムの値、止められたら 139（不正なアクセス）・130（Ctrl-C）・124（時間の上限）、動かせなければ 1。
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libslimcc.h"
#include "../kernel/sandbox.h"
#include "../drivers/uart.h"

#define LIB_DIR     "/usr/lib/sandbox"
#define MAX_OBJS    32

static uint8_t *link_region;
static bool link_used;

// つなぐ部品（cc/memlink.c）は、プログラム全体を 1 回で取る。それをサンドボックスの領域の頭に置く
static void *alloc_in_region(size_t size)
{
    if (link_used || size > SANDBOX_IMAGE_SIZE)
        return nullptr;
    link_used = true;
    return link_region;
}

static void free_in_region(void *, size_t) {}

// OS の関数にはつながない（ライブラリの中で見つからない名前は、どれも足りない関数）
static void *no_os_functions(const char *, void *)
{
    return nullptr;
}

int sandbox_cmd(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sandbox FILE.c [ARGS...]\n");
        return 2;
    }
    SlimccObj *objs[MAX_OBJS];
    int n = 0, r = 1;
    static const char *const incs[] = { LIB_DIR, nullptr };
    static const char *const defs[] = { "_POSIX_MONOTONIC_CLOCK", nullptr };   // ふつうのアプリと同じ（run.c の app_defines）
    SlimccOptions opt = { .std = "gnu23", .defines = defs };
    SlimccOptions lib_opt = { .std = "gnu23", .include_paths = incs, .defines = defs };
    SlimccImage *img = nullptr;
    uint8_t *region = nullptr;
    uint32_t intr0 = uart_shell_interrupts();     // コンパイルの途中の Ctrl-C でも、動かさずに止める

    if (!(objs[n++] = slimcc_compile(argv[1], nullptr, &opt)))
        goto done;
    // ライブラリの .c: 頭のフォルダと、curses/（OS の curses の写し）・libc/（wcwidth）
    static const char *const dirs[] = { LIB_DIR, LIB_DIR "/curses", LIB_DIR "/libc" };
    for (size_t k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
        DIR *d = opendir(dirs[k]);
        if (!d) {
            fprintf(stderr, "sandbox: %s: cannot open the sandbox's C library\n", dirs[k]);
            goto done;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len < 3 || strcmp(e->d_name + len - 2, ".c"))
                continue;
            char path[300];
            snprintf(path, sizeof path, "%s/%s", dirs[k], e->d_name);
            if (n == MAX_OBJS || !(objs[n] = slimcc_compile(path, nullptr, &lib_opt))) {
                closedir(d);
                goto done;
            }
            n++;
        }
        closedir(d);
    }

    region = sandbox_region_alloc();
    if (!region) {
        fprintf(stderr, "sandbox: out of memory\n");
        goto done;
    }
    link_region = region;
    link_used = false;
    img = slimcc_link_many(objs, n, no_os_functions, nullptr, alloc_in_region, free_in_region);
    if (!img)
        goto done;      // 足りない関数（サンドボックスで使えないもの）は、つなぐ部品が表示する
    void *entry = slimcc_symbol(img, "__sandbox_start");
    if (!entry) {
        fprintf(stderr, "sandbox: no __sandbox_start (the sandbox's C library is missing?)\n");
        goto done;
    }
    if (uart_shell_interrupts() != intr0) {
        r = 130;
        goto done;
    }
    int64_t status = 0;
    switch (sandbox_enter(region, (uint64_t)entry, true, argc - 1, argv + 1, 0, &status)) {
    case SANDBOX_EXITED:      r = (int)(status & 0xFF); break;
    case SANDBOX_FAULTED:     r = 139; break;
    case SANDBOX_INTERRUPTED: r = 130; break;
    case SANDBOX_TIMEDOUT:    r = 124; break;
    case SANDBOX_NOT_STARTED: fprintf(stderr, "sandbox: cannot start\n"); r = 1; break;
    }
done:
    if (img)
        slimcc_free_image(img);
    for (int i = 0; i < n; i++)
        if (objs[i])
            slimcc_free_obj(objs[i]);
    if (region)
        sandbox_region_free(region);
    return r;
}
