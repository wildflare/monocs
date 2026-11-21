// slimcc（ファイル単位のコンパイル）と slimld（リンク）のコマンド。シェルでは cc・ld の名前で使う
// （2026-11-07 までは slimcc・slimld）。アプリからは monocs_cc・monocs_ld。メッセージの名前は argv[0]
//
// Linux 上のクロスビルド（make kernel-slimcc）と同じ .o・kernel8.img を作る。
// コンパイルは cc/libslimcc.c の slimcc_compile_file、リンクは cc/ld.c の slimld_link。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libc/heap.h"
#include "../kernel/lock.h"
#include "cmd.h"
#include "ld.h"
#include "libslimcc.h"
#include "run.h"

#define MAX_ARGS 64

static const char *value(int argc, char **argv, int *i, const char *opt)
{
    const char *me = argv[0];
    // -I dir と -Idir の両方を受け付ける
    size_t n = strlen(opt);
    if (argv[*i][n])
        return argv[*i] + n;
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s: %s needs an argument\n", me, opt);
        return nullptr;
    }
    return argv[++*i];
}

// -c のない cc（gcc と同じ）: .c をコンパイルして実行ファイルを作る（既定の名前は a.out）。
// -D・-U・-I・-std=・-l名前・-f… は run と同じに渡し、gcc でよく書く -O・-W・-g・-pipe などは読み捨てる
static int cc_executable(int argc, char **argv)
{
    // 入れ物は引数の数だけ取る（前は 64 個で、それより後のファイル・オプションを黙って捨てていた。2026-11-20）
    const char **files = malloc(sizeof *files * (size_t)argc), **flags = malloc(sizeof *flags * (size_t)(argc + 1));
    char **joined = calloc((size_t)argc, sizeof *joined);
    const char *out = "a.out";
    int nfiles = 0, nflags = 0, njoined = 0, r = 1;
    if (!files || !flags || !joined) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        goto done;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if ((!strcmp(a, "-I") || !strcmp(a, "-D") || !strcmp(a, "-U") || !strcmp(a, "-l")) && i + 1 < argc) {
            // -I DIR のように分けて書いたもの（run は -IDIR の形だけ読む）
            size_t n = strlen(a) + strlen(argv[i + 1]) + 1;
            char *j = malloc(n);
            if (!j) {
                fprintf(stderr, "%s: out of memory\n", argv[0]);
                goto done;
            }
            snprintf(j, n, "%s%s", a, argv[++i]);
            joined[njoined++] = j;
            flags[nflags++] = j;
        } else if (!strncmp(a, "-I", 2) || !strncmp(a, "-D", 2) || !strncmp(a, "-U", 2) || !strncmp(a, "-l", 2) ||
                   !strncmp(a, "-std=", 5) || !strncmp(a, "-f", 2)) {
            flags[nflags++] = a;
        } else if (a[0] == '-') {
            continue;   // -O2・-Wall・-g・-pipe・-L など
        } else {
            files[nfiles++] = a;
        }
    }
    flags[nflags] = nullptr;
    if (!nfiles) {
        fprintf(stderr, "%s: no input files\n", argv[0]);
        goto done;
    }
    r = slimcc_make_executable_files(files, nfiles, flags, out) ? 0 : 1;
done:
    for (int i = 0; i < njoined; i++)
        free(joined[i]);
    free(joined);
    free(flags);
    free(files);
    return r;
}

int slimcc_cmd(int argc, char **argv)
{
    bool compile_only = false;
    for (int i = 1; i < argc; i++)
        compile_only |= !strcmp(argv[i], "-c");
    if (!compile_only)
        return cc_executable(argc, argv);

    const char *incs[MAX_ARGS + 1];
    const char *defs[MAX_ARGS + 1];
    const char *undefs[MAX_ARGS + 1];
    const char *sysincs[MAX_ARGS + 1];
    int ninc = 0, ndef = 0, nundef = 0, nsysinc = 0;
    bool nostdinc = false, func_sections = false, no_schedule = false, no_unroll = false, no_alias = false;
    const char *out = nullptr, *in = nullptr, *std = nullptr;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-c"))
            continue;
        if (!strcmp(a, "-o")) {
            if (++i >= argc)
                goto usage;
            out = argv[i];
        } else if (!strncmp(a, "-I", 2)) {
            if (ninc == MAX_ARGS || !(incs[ninc++] = value(argc, argv, &i, "-I")))
                return 1;
        } else if (!strncmp(a, "-D", 2)) {
            if (ndef == MAX_ARGS || !(defs[ndef++] = value(argc, argv, &i, "-D")))
                return 1;
        } else if (!strncmp(a, "-U", 2)) {
            if (nundef == MAX_ARGS || !(undefs[nundef++] = value(argc, argv, &i, "-U")))
                return 1;
        } else if (!strncmp(a, "-isystem", 8)) {
            if (nsysinc == MAX_ARGS || !(sysincs[nsysinc++] = value(argc, argv, &i, "-isystem")))
                return 1;
        } else if (!strcmp(a, "-nostdinc")) {
            nostdinc = true;
        } else if (!strcmp(a, "-ffunction-sections")) {
            func_sections = true;
        } else if (!strcmp(a, "-fno-schedule-insns2") || !strcmp(a, "-fschedule-insns2")) {
            no_schedule = a[2] == 'n';  // 命令の並べ替え（cc/sched.inc。既定でする）
        } else if (!strcmp(a, "-fno-unroll-loops") || !strcmp(a, "-funroll-loops")) {
            no_unroll = a[2] == 'n';    // ループの展開（cc/ir.inc。既定でする）
        } else if (!strcmp(a, "-fno-strict-aliasing") || !strcmp(a, "-fstrict-aliasing")) {
            no_alias = a[2] == 'n';     // 型による別名の判定（cc/ir.inc。既定でする）
        } else if (!strncmp(a, "-std=", 5)) {
            std = a + 5;
        } else if (a[0] == '-') {
            fprintf(stderr, "%s: unknown option: %s\n", argv[0], a);
            return 1;
        } else if (in) {
            fprintf(stderr, "%s: only one input file is supported\n", argv[0]);
            return 1;
        } else {
            in = a;
        }
    }
    if (!in)
        goto usage;
    incs[ninc] = nullptr;
    defs[ndef] = nullptr;
    undefs[nundef] = nullptr;
    sysincs[nsysinc] = nullptr;

    char *tmp = nullptr;
    if (!out) {
        // FILE.c → FILE.o（ディレクトリはそのまま）
        size_t n = strlen(in);
        const char *dot = strrchr(in, '.');
        if (dot && !strchr(dot, '/'))
            n = dot - in;
        tmp = malloc(n + 3);
        if (!tmp)
            return 1;
        memcpy(tmp, in, n);
        strcpy(tmp + n, ".o");
        out = tmp;
    }

    SlimccOptions opt = {.include_paths = incs, .defines = defs, .std = std, .undefines = undefs,
                         .system_include_paths = sysincs, .nostdinc = nostdinc,
                         .function_sections = func_sections, .no_schedule = no_schedule,
                         .no_unroll = no_unroll, .no_strict_aliasing = no_alias};
    int r = slimcc_compile_file(in, out, &opt);
    free(tmp);
    return r;

usage:
    fprintf(stderr, "usage: %s [-c] [-o OUT.o] [-I DIR] [-D NAME[=VAL]] [-U NAME] [-isystem DIR] "
                    "[-nostdinc] [-ffunction-sections] [-fno-schedule-insns2] [-fno-unroll-loops] [-fno-strict-aliasing] [-std=STD] FILE.c\n", argv[0]);
    return 1;
}

int slimld_cmd(int argc, char **argv)
{
    const char **inputs = calloc(argc + 1, sizeof(char *));
    if (!inputs)
        return 1;
    int n = 0;
    SlimldOptions opt = {};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            opt.output = argv[++i];
        else if (!strcmp(argv[i], "-Map") && i + 1 < argc)
            opt.map = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            opt.verbose = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option: %s\n", argv[0], argv[i]);
            free(inputs);
            return 1;
        } else
            inputs[n++] = argv[i];
    }
    if (!opt.output || !n) {
        fprintf(stderr, "usage: %s [-v] [-Map FILE] -o kernel8.img FILE.o... LIB.a...（Monocs のカーネルを作る）\n", argv[0]);
        free(inputs);
        return 1;
    }
    opt.inputs = inputs;
    int r = slimld_link(&opt);
    free(inputs);
    return r;
}

// アプリから呼ぶとき（monocs_cc・monocs_ld）。
//   - コンパイラは状態を次のコンパイルまで持つことがあるので、アプリの実行中でもカーネル用のヒープを使う
//     （os/libc/malloc.c）。
//   - 途中で止めると、コンパイラの状態が壊れて次のコンパイルがおかしくなる（セルフホストのビルドを Ctrl-C で
//     止めた後、次のビルドで例外になった）。止めてよいように後始末をそろえるのは確かめきれないので、
//     1 回のコンパイル・リンクの終わりまで中断（Ctrl-C、ほかのコアの仕事を止める知らせ）を後回しにする。
//     同期例外で止まったときは、深さは malloc_app_stop・lock_release_all（コア 0）、次の仕事の始め（コア 1〜3）で戻る
int slimcc_cmd_app(int argc, char **argv)
{
    abort_defer_begin();
    malloc_kernel_begin();
    int r = slimcc_cmd(argc, argv);
    malloc_kernel_end();
    abort_defer_end();
    return r;
}

int slimld_cmd_app(int argc, char **argv)
{
    abort_defer_begin();
    malloc_kernel_begin();
    int r = slimld_cmd(argc, argv);
    malloc_kernel_end();
    abort_defer_end();
    return r;
}
