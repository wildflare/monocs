// Monocs 上でカーネルをビルドする（セルフホスト）。SD カードの /src で実行する:
//   cd /src
//   cc -o build build.c && ./build
//
// Linux 上のクロスビルド（make kernel-slimcc）と同じ順・同じオプションで、
// すべてのソースを slimcc でコンパイルし、slimld で build/kslim/kernel8.img を作る。
// コンパイルは使えるコアすべてで分担する（組み込みの slimcc はコアごとに状態を持つ）。
// ファイルの一覧（build_files.h）は make sdimage-self が Makefile から作る。
// C ライブラリは、Monocs 上でビルドしたもの（buildlibc.c の build/picolibc-slimcc/libc.a）があればそれを、
// なければ Linux 上でビルドしたもの（lib/libc.a）を使う。

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <monocs.h>

enum { KIND_OS, KIND_FATFS, KIND_LWIP, KIND_SLIMCC, KIND_RT, KIND_ASM };

static const struct {
    const char *src;
    const char *obj;
    int kind;
} files[] = {
#include "build_files.h"
};

#define NFILES (sizeof(files) / sizeof(files[0]))

// path のディレクトリを（親から順に）作る
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

static int compile(const char *src, const char *obj, int kind)
{
    char *argv[16];
    int argc = 0;
    argv[argc++] = "slimcc";
    switch (kind) {
    case KIND_OS:
        argv[argc++] = "-std=c23";
        argv[argc++] = "-Ibuild/fatfs";
        argv[argc++] = "-Icc";
        argv[argc++] = "-Ithird_party/lwip/src/include";
        argv[argc++] = "-Ios/net/port";
        break;
    case KIND_LWIP:
        argv[argc++] = "-std=c23";
        argv[argc++] = "-Ithird_party/lwip/src/include";
        argv[argc++] = "-Ios/net/port";
        break;
    case KIND_FATFS:
        argv[argc++] = "-std=c23";
        argv[argc++] = "-Ibuild/fatfs";
        break;
    case KIND_SLIMCC:
        argv[argc++] = "-std=gnu17";
        argv[argc++] = "-DSLIMCC_LIBRARY";
        break;
    case KIND_RT:
        argv[argc++] = "-std=gnu17";
        break;
    case KIND_ASM:
        break;
    }
    argv[argc++] = "-c";
    argv[argc++] = "-o";
    argv[argc++] = (char *)obj;
    argv[argc++] = (char *)src;
    argv[argc] = NULL;
    return monocs_cc(argc, argv);
}

// 4 つのコアで分担する。大きいファイルから順に並べ、空いたコアが次のファイルを取る
// （コンパイルの順番は .o の中身に影響しない）
static size_t order[NFILES];
static size_t next_job;
static int failed;
static monocs_mutex lock = MONOCS_MUTEX_INIT;

static void worker(void *arg)
{
    int core = (int)(long)arg;
    for (;;) {
        monocs_mutex_lock(&lock);
        size_t k = next_job < NFILES && !failed ? next_job++ : NFILES;
        monocs_mutex_unlock(&lock);
        if (k == NFILES)
            return;
        size_t i = order[k];
        uint64_t t = timer_now_us();
        make_parent_dirs(files[i].obj);
        int r = compile(files[i].src, files[i].obj, files[i].kind);
        monocs_mutex_lock(&lock);
        if (r) {
            printf("[%2u/%u] %s: build failed\n", (unsigned)(k + 1), (unsigned)NFILES, files[i].src);
            failed = 1;
        } else {
            printf("[%2u/%u] %s (%u ms, core %d)\n", (unsigned)(k + 1), (unsigned)NFILES,
                   files[i].src, (unsigned)((timer_now_us() - t) / 1000), core);
        }
        monocs_mutex_unlock(&lock);
    }
}

int main(void)
{
    uint64_t start = timer_now_us();
    long size[NFILES];
    for (size_t i = 0; i < NFILES; i++) {
        struct stat st;
        size[i] = stat(files[i].src, &st) == 0 ? (long)st.st_size : 0;
        order[i] = i;
    }
    for (size_t i = 1; i < NFILES; i++)
        for (size_t j = i; j > 0 && size[order[j]] > size[order[j - 1]]; j--) {
            size_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }

    int cores = monocs_cores();
    printf("%d cores\n", cores);
    for (int c = 1; c < cores; c++)
        monocs_core_start(c, worker, (void *)(long)c);
    worker(0);
    for (int c = 1; c < cores; c++)
        if (!monocs_core_wait(c))
            failed = 1;
    if (failed) {
        printf("build failed\n");
        return 1;
    }

    char *argv[NFILES + 8];
    int argc = 0;
    argv[argc++] = "slimld";
    argv[argc++] = "-Map";
    argv[argc++] = "build/kslim/kernel.map";
    argv[argc++] = "-o";
    argv[argc++] = "build/kslim/kernel8.img";
    for (size_t i = 0; i < NFILES; i++)
        argv[argc++] = (char *)files[i].obj;
    struct stat st;
    const char *libc = stat("build/picolibc-slimcc/libc.a", &st) == 0 ? "build/picolibc-slimcc/libc.a"
                                                                       : "lib/libc.a";
    argv[argc++] = (char *)libc;
    argv[argc] = NULL;
    printf("link build/kslim/kernel8.img (%s)\n", libc);
    if (monocs_ld(argc, argv)) {
        printf("build failed\n");
        return 1;
    }
    printf("done (%u s)\n", (unsigned)((timer_now_us() - start) / 1000000));
    return 0;
}
