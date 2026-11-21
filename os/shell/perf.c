// シェルの perf: CPU の性能カウンタ（PMU。os/kernel/pmu.c）で数える
//   perf [-e EV,...] [-c] CMD...     命令の数、キャッシュのアクセスとミスなどを数える
//   perf record [-e EV] [-p N] [-g] CMD...  EV が N 回起きるごとに、動いていた番地を記録し、関数ごとに多い順に並べる
//                                    （-g: 関数ごとに、多い呼び出し元の並び。x30 とフレームポインタをたどったもの）
//
// 関数の名前: アプリのものは、アプリが終わる前に os/slimcc/run.c が付ける。カーネルのものは /boot/kernel8.sym
// （ビルドで作る。tools/perf/mksym.py。1 行目に今のカーネルの識別があり、違えば番地のまま出す）で引く。

// strdup の宣言のため
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell.h"
#include "../kernel/pmu.h"
#include "../kernel/ksym.h"

#define LINE_MAX_LEN 1024

int cmd_perf(int argc, char **argv);

// 3 けたごとにコンマを入れる
static const char *with_commas(uint64_t v, char *buf)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0)
            buf[j++] = ',';
        buf[j++] = tmp[i];
    }
    buf[j] = 0;
    return buf;
}

static int perf_usage(void)
{
    fprintf(stderr, "usage: perf [-e EVENT,...] [-c] COMMAND [ARGS...]        数える（-l でイベントの一覧）\n"
                    "       perf record [-e EVENT] [-p N] [-n TOP] [-g] [-o FILE] COMMAND [ARGS...]\n"
                    "                                   EVENT が N 回起きるごとに、動いていた関数を記録する（-g で呼び出し元も）\n");
    return 2;
}

static int perf_stat(int argc, char **argv)
{
    static const char *const defaults[] = {"inst", "l1d", "l1d-miss", "l1i-miss", "l2", "l2-miss"};
    const PmuEvent *sel[PMU_MAX_EVENTS];
    int nsel = 0;
    bool per_core = false;
    int max = pmu_counters();
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-l")) {
            int n;
            const PmuEvent *ev = pmu_events(&n);
            printf("%d counters (and the cycle counter)\n", max);
            for (int k = 0; k < n; k++)
                printf("  %-10s %-3s %s\n", ev[k].name, pmu_event_supported(ev[k].num) ? "" : "(-)",
                       ev[k].desc);
            printf("(-): this CPU does not count it. default: inst,l1d,l1d-miss,l1i-miss,l2,l2-miss\n");
            return 0;
        } else if (!strcmp(argv[i], "-c")) {
            per_core = true;
        } else if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            char list[LINE_MAX_LEN];
            snprintf(list, sizeof(list), "%s", argv[++i]);
            for (char *tok = strtok(list, ","); tok; tok = strtok(nullptr, ",")) {
                const PmuEvent *e = pmu_find_event(tok);
                if (!e) {
                    fprintf(stderr, "perf: unknown event '%s' (perf -l)\n", tok);
                    return 2;
                }
                if (nsel == max || nsel == PMU_MAX_EVENTS) {
                    fprintf(stderr, "perf: at most %d events\n", max);
                    return 2;
                }
                sel[nsel++] = e;
            }
        } else {
            return perf_usage();
        }
    }
    if (i == argc)
        return perf_usage();
    if (!nsel)
        for (int k = 0; k < (int)(sizeof(defaults) / sizeof(defaults[0])) && nsel < max; k++)
            sel[nsel++] = pmu_find_event(defaults[k]);
    uint16_t nums[PMU_MAX_EVENTS];
    for (int k = 0; k < nsel; k++)
        nums[k] = sel[k]->num;

    pmu_begin(nums, nsel);
    int status = run_command(argc - i, argv + i);
    PmuResult r;
    pmu_end(&r);

    // 表示（stderr。コマンドの出力をリダイレクトしても見えるように）
    int ncores = 0;
    uint64_t cycles = 0;
    for (int c = 0; c < 4; c++)
        if (r.used_core[c]) {
            ncores++;
            cycles += r.per_core[c][r.n];
        }
    uint64_t inst = 0;
    bool have_inst = false;
    for (int k = 0; k < nsel; k++)
        if (!strcmp(sel[k]->name, "inst")) {
            inst = r.counts[k];
            have_inst = true;
        }
    char b[40];
    fprintf(stderr, "\nperf: %s, %d core%s, %llu.%03llu s\n",
            r.app_only ? "while the app ran" : "the whole command", ncores, ncores > 1 ? "s" : "",
            (unsigned long long)(r.elapsed_us / 1000000), (unsigned long long)(r.elapsed_us / 1000 % 1000));
    fprintf(stderr, "  %18s  cycles\n", with_commas(cycles, b));
    for (int k = 0; k < nsel; k++) {
        fprintf(stderr, "  %18s  %-10s", with_commas(r.counts[k], b), sel[k]->name);
        if (!pmu_event_supported(sel[k]->num))
            fprintf(stderr, "  (not counted by this CPU)");
        else if (!strcmp(sel[k]->name, "inst") && cycles)
            fprintf(stderr, "  %.2f per cycle", (double)r.counts[k] / (double)cycles);
        else {
            // 「X-miss」は X に対する割合と、1000 命令あたり
            size_t len = strlen(sel[k]->name);
            if (len > 5 && !strcmp(sel[k]->name + len - 5, "-miss")) {
                for (int m = 0; m < nsel; m++)
                    if (strlen(sel[m]->name) == len - 5 && !strncmp(sel[m]->name, sel[k]->name, len - 5) &&
                        r.counts[m])
                        fprintf(stderr, "  %.2f%% of %s", 100.0 * (double)r.counts[k] / (double)r.counts[m],
                                sel[m]->name);
                if (have_inst && inst)
                    fprintf(stderr, "  %.2f per 1000 inst", 1000.0 * (double)r.counts[k] / (double)inst);
            }
        }
        fprintf(stderr, "\n");
    }
    if (per_core)
        for (int c = 0; c < 4; c++) {
            if (!r.used_core[c])
                continue;
            fprintf(stderr, "  core %d: cycles %s", c, with_commas(r.per_core[c][r.n], b));
            for (int k = 0; k < nsel; k++)
                fprintf(stderr, ", %s %s", sel[k]->name, with_commas(r.per_core[c][k], b));
            fprintf(stderr, "\n");
        }
    return status;
}


// カーネルの関数名は os/kernel/ksym.c（/boot/kernel8.sym）

// ---- perf record ----

typedef struct {
    const char *name;   // 関数名（番地のままなら buf）
    char buf[24];
    bool app;
    int count;
    int cores[4];
} Hit;

static int cmp_hit(const void *a, const void *b)
{
    const Hit *x = a, *y = b;
    if (x->count != y->count)
        return x->count > y->count ? -1 : 1;
    return strcmp(x->name, y->name);
}

// サンプルの関数名（アプリ、カーネル、どちらでもなければ番地を buf に）
static const char *sample_name(const PmuSample *s, bool have_k, char *buf, size_t size, bool *app)
{
    *app = s->name != nullptr;
    const char *name = s->name;
    if (!name && have_k)
        name = ksym_at(s->pc);
    if (!name) {
        snprintf(buf, size, "%#llx", (unsigned long long)s->pc);
        name = buf;
    }
    return name;
}

// 呼び出し元の並び（「← A ← B」。同じ名前が続くもの、関数自身は省く）
static void caller_chain(const PmuSample *s, const char *self, bool have_k, char *buf, size_t size)
{
    size_t len = 0;
    const char *prev = self;
    buf[0] = 0;
    for (int c = 0; c < PMU_CALLS && s->calls[c]; c++) {
        const char *name = s->call_names[c];
        char tmp[24];
        if (!name && have_k)
            name = ksym_at(s->calls[c] - 4);
        if (!name) {
            snprintf(tmp, sizeof(tmp), "%#llx", (unsigned long long)s->calls[c]);
            name = tmp;
        }
        if (!strcmp(name, prev))
            continue;
        int w = snprintf(buf + len, size - len, " <- %s", name);
        if (w < 0 || (size_t)w >= size - len)
            break;
        len += (size_t)w;
        prev = s->call_names[c] ? s->call_names[c] : (have_k ? ksym_at(s->calls[c] - 4) : "");
        if (!prev)
            prev = "";
    }
}

typedef struct {
    char chain[160];
    int count;
} Chain;

// 関数 name（app）のサンプルの、多い呼び出し元の並びを 3 つまで表示する
static void print_callers(FILE *f, const PmuSample *s, int n, const char *name, bool app, bool have_k)
{
    enum { MAXC = 64 };
    static Chain chains[MAXC];
    int nc = 0, other = 0, total = 0;
    for (int k = 0; k < n; k++) {
        char buf[24];
        bool a;
        if (strcmp(sample_name(&s[k], have_k, buf, sizeof(buf), &a), name) || a != app)
            continue;
        total++;
        char chain[160];
        caller_chain(&s[k], name, have_k, chain, sizeof(chain));
        int c = 0;
        while (c < nc && strcmp(chains[c].chain, chain))
            c++;
        if (c == nc) {
            if (nc == MAXC) {
                other++;
                continue;
            }
            snprintf(chains[nc].chain, sizeof(chains[nc].chain), "%s", chain);
            chains[nc++].count = 0;
        }
        chains[c].count++;
    }
    for (int shown = 0; shown < 3; shown++) {
        int best = -1;
        for (int c = 0; c < nc; c++)
            if (chains[c].count && (best < 0 || chains[c].count > chains[best].count))
                best = c;
        if (best < 0)
            break;
        fprintf(f, "                 %5.1f%%%s\n", 100.0 * chains[best].count / total,
                chains[best].chain[0] ? chains[best].chain : " (no caller)");
        chains[best].count = 0;
    }
    (void)other;
}

static int perf_record(int argc, char **argv)
{
    const PmuEvent *ev = pmu_find_event("l2-miss");
    long every = 10000;
    int top = 20;
    bool callers = false;
    const char *out = nullptr;
    int i = 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            ev = pmu_find_event(argv[++i]);
            if (!ev) {
                fprintf(stderr, "perf: unknown event '%s' (perf -l)\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            every = strtol(argv[++i], nullptr, 0);
            if (every < 100 || every > 1000000000) {
                fprintf(stderr, "perf: -p は 100 から 1000000000 まで\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            top = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "-g")) {
            callers = true;
        } else {
            return perf_usage();
        }
    }
    if (i == argc)
        return perf_usage();
    if (!pmu_event_supported(ev->num))
        fprintf(stderr, "perf: this CPU does not count '%s'; nothing will be recorded\n", ev->name);

    pmu_record_begin(ev->num, (uint32_t)every);
    int status = run_command(argc - i, argv + i);
    PmuResult r;
    pmu_end(&r);

    int n, lost;
    const PmuSample *s = pmu_samples(&n, &lost);
    const char *why = nullptr;
    bool have_k = ksym_load(&why);

    // 関数ごとに数える（関数の数は多くないので、名前で線形に探す）
    Hit *hits = calloc((size_t)n + 1, sizeof(Hit));
    int nhits = 0;
    for (int k = 0; hits && k < n; k++) {
        char tmp[24];
        bool app;
        const char *name = sample_name(&s[k], have_k, tmp, sizeof(tmp), &app);
        int h = 0;
        while (h < nhits && (hits[h].app != app || strcmp(hits[h].name, name)))
            h++;
        if (h == nhits) {
            if (name == tmp) {
                snprintf(hits[h].buf, sizeof(hits[h].buf), "%s", tmp);
                hits[h].name = hits[h].buf;
            } else {
                hits[h].name = name;
            }
            hits[h].app = app;
            nhits++;
        }
        hits[h].count++;
        if (s[k].core < 4)
            hits[h].cores[s[k].core]++;
    }
    if (hits)
        qsort(hits, (size_t)nhits, sizeof(Hit), cmp_hit);

    char b[40];
    fprintf(stderr, "\nperf record: %s every %s, %d samples%s, %s, %llu.%03llu s\n", ev->name,
            with_commas((uint64_t)every, b), n, lost ? " (some were dropped)" : "",
            r.app_only ? "while the app ran" : "the whole command",
            (unsigned long long)(r.elapsed_us / 1000000), (unsigned long long)(r.elapsed_us / 1000 % 1000));
    if (!have_k)
        fprintf(stderr, "  (kernel functions are shown as addresses: %s。tools/uart/nos.py kernel が一緒に送ります)\n",
                why);
    FILE *f = out ? fopen(out, "w") : nullptr;
    if (out && !f)
        fprintf(stderr, "perf: %s: cannot write\n", out);
    for (int h = 0; h < nhits; h++) {
        char line[256];
        int len = snprintf(line, sizeof(line), "  %5.1f%%  %6d  %-6s %s", 100.0 * hits[h].count / n,
                           hits[h].count, hits[h].app ? "app" : "kernel", hits[h].name);
        if (r.used_core[1] || r.used_core[2] || r.used_core[3]) {
            len += snprintf(line + len, sizeof(line) - (size_t)len, "  (core");
            for (int c = 0; c < 4; c++)
                if (hits[h].cores[c])
                    len += snprintf(line + len, sizeof(line) - (size_t)len, " %d:%d", c, hits[h].cores[c]);
            snprintf(line + len, sizeof(line) - (size_t)len, ")");
        }
        if (h < top)
            fprintf(stderr, "%s\n", line);
        if (f)
            fprintf(f, "%s\n", line);
        if (callers && h < top)
            print_callers(stderr, s, n, hits[h].name, hits[h].app, have_k);
    }
    if (nhits > top)
        fprintf(stderr, "  ... %d more%s\n", nhits - top, out ? "" : " (-n で増やす、-o FILE で全部を書く)");
    if (f)
        fclose(f);
    free(hits);
    return status;
}

int cmd_perf(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "record"))
        return perf_record(argc, argv);
    return perf_stat(argc, argv);
}
