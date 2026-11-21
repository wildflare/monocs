// Monocs 上で C ライブラリ（picolibc）をビルドする（セルフホスト）。SD カードの /src で実行する:
//   cd /src
//   cc -o buildlibc buildlibc.c && ./buildlibc
//
// Linux 上のビルド（make picolibc-slimcc、tools/picolibc/build.py）と同じソース・同じオプションで、
// build/picolibc（meson のビルドディレクトリ）からすべてのソースを slimcc でコンパイルし、
// build/picolibc-slimcc/libc.a を作る（同じメンバーを同じ順に。ar.h は GNU ar と同じ形式で書く）。
// できた libc.a は、カーネルのビルド（build.c）が lib/libc.a の代わりに使う。
// コンパイルは使えるコアすべてで分担する。ファイルの一覧（libc_files.h）は make sdimage-self が作る。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <monocs.h>
#include "ar.h"
#include "libc_files.h"

#define NFILES (sizeof(libc_files) / sizeof(libc_files[0]))
#define NMEMBERS (sizeof(libc_members) / sizeof(libc_members[0]))
#define LIBC_OUT "../picolibc-slimcc/libc.a"

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

static int compile(size_t i)
{
    char *argv[64];
    int argc = 0;
    argv[argc++] = "slimcc";
    for (const char *const *f = libc_flags[libc_files[i].flags]; *f; f++)
        argv[argc++] = (char *)*f;
    argv[argc++] = "-c";
    argv[argc++] = "-o";
    argv[argc++] = (char *)libc_files[i].obj;
    argv[argc++] = (char *)libc_files[i].src;
    argv[argc] = NULL;
    return monocs_cc(argc, argv);
}

// 4 つのコアで分担する。大きいファイルから順に並べ、空いたコアが次のファイルを取る
static size_t order[NFILES];
static size_t next_job, done_jobs;
static int failed;
static monocs_mutex lock = MONOCS_MUTEX_INIT;

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        monocs_mutex_lock(&lock);
        size_t k = next_job < NFILES && !failed ? next_job++ : NFILES;
        monocs_mutex_unlock(&lock);
        if (k == NFILES)
            return;
        size_t i = order[k];
        make_parent_dirs(libc_files[i].obj);
        int r = compile(i);
        monocs_mutex_lock(&lock);
        done_jobs++;
        if (r) {
            printf("%s: build failed\n", libc_files[i].src);
            failed = 1;
        } else if (done_jobs % 100 == 0 || done_jobs == NFILES) {
            printf("[%4u/%u]\n", (unsigned)done_jobs, (unsigned)NFILES);
        }
        monocs_mutex_unlock(&lock);
    }
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(n ? n : 1);
    if (p && fread(p, 1, n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *size = n;
    return p;
}

int main(void)
{
    uint64_t start = timer_now_us();
    if (chdir("build/picolibc") != 0) {
        printf("cannot chdir to build/picolibc (run in /src)\n");
        return 1;
    }
    static long size[NFILES];
    for (size_t i = 0; i < NFILES; i++) {
        struct stat st;
        size[i] = stat(libc_files[i].src, &st) == 0 ? (long)st.st_size : 0;
        order[i] = i;
    }
    // 大きい順に並べる（シェルソート。1000 ファイルあるので挿入ソートでは遅い）
    for (size_t gap = NFILES / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < NFILES; i++)
            for (size_t j = i; j >= gap && size[order[j]] > size[order[j - gap]]; j -= gap) {
                size_t t = order[j];
                order[j] = order[j - gap];
                order[j - gap] = t;
            }

    int cores = monocs_cores();
    printf("%d cores, %u files\n", cores, (unsigned)NFILES);
    for (int c = 1; c < cores; c++)
        monocs_core_start(c, worker, NULL);
    worker(NULL);
    for (int c = 1; c < cores; c++)
        if (!monocs_core_wait(c))
            failed = 1;
    if (failed) {
        printf("build failed\n");
        chdir("../..");
        return 1;
    }

    // libc.a（GNU ar と同じ形式）
    static ArMember m[NMEMBERS];
    for (size_t k = 0; k < NMEMBERS; k++) {
        const char *obj = libc_files[libc_members[k]].obj;
        const char *base = strrchr(obj, '/');
        m[k].name = base ? base + 1 : obj;
        m[k].data = read_file(obj, &m[k].size);
        if (!m[k].data) {
            printf("cannot read %s\n", obj);
            chdir("../..");
            return 1;
        }
    }
    int r = ar_write(LIBC_OUT, m, NMEMBERS);
    for (size_t k = 0; k < NMEMBERS; k++)
        free(m[k].data);
    chdir("../..");
    if (r) {
        printf("cannot write build/picolibc-slimcc/libc.a\n");
        return 1;
    }
    printf("build/picolibc-slimcc/libc.a: %u members\n", (unsigned)NMEMBERS);
    printf("done (%u s)\n", (unsigned)((timer_now_us() - start) / 1000000));
    return 0;
}
