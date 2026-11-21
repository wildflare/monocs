// カーネルの関数名（/boot/kernel8.sym。ビルドで作る。tools/perf/mksym.py。1 行目に今のカーネルの識別があり、違えば
// 使わない）。perf record（os/shell/perf.c）と、アプリが落ちたときの表示（os/slimcc/run.c）が使う
#define _DEFAULT_SOURCE      // strdup
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ksym.h"
// カーネルの識別: .text・.rodata（0x80000〜__tdata_start）の FNV-1a（tools/perf/mksym.py も同じ計算）
extern char __tdata_start[];
static uint64_t kernel_id;

uint64_t ksym_kernel_id(void)
{
    if (!kernel_id) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (const unsigned char *p = (const unsigned char *)0x80000; p < (const unsigned char *)__tdata_start; p++)
            h = (h ^ *p) * 0x100000001b3ULL;
        kernel_id = h | 1;
    }
    return kernel_id;
}


typedef struct {
    uint64_t addr;
    char *name;
} KSym;

static KSym *ksyms;
static int nksyms;
static uint64_t ksyms_id;       // 読んだ表のカーネルの識別（0 = 読んでいない）

static int cmp_ksym(const void *a, const void *b)
{
    const KSym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

// 今のカーネルの表を読む。なければ・違うカーネルのものなら false（理由を why に）
bool ksym_load(const char **why)
{
    uint64_t id = ksym_kernel_id();
    if (ksyms_id == id)
        return true;
    FILE *f = fopen("/boot/kernel8.sym", "r");
    if (!f)
        f = fopen("/kernel8.sym", "r");     // 2026-11-13 より前の SD カード（ルートに置いていた）
    if (!f) {
        *why = "/boot/kernel8.sym がありません";
        return false;
    }
    char line[256];
    unsigned long long fid = 0;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "# monocs-kernel-sym %llx", &fid) != 1 || fid != id) {
        fclose(f);
        *why = "/boot/kernel8.sym は今のカーネルのものではありません";
        return false;
    }
    for (int i = 0; i < nksyms; i++)
        free(ksyms[i].name);
    free(ksyms);
    ksyms = nullptr;
    nksyms = 0;
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long a;
        char name[200];
        if (sscanf(line, "%llx %199s", &a, name) != 2)
            continue;
        if (nksyms == cap) {
            cap = cap ? cap * 2 : 1024;
            KSym *p = realloc(ksyms, sizeof(KSym) * (size_t)cap);
            if (!p)
                break;
            ksyms = p;
        }
        ksyms[nksyms++] = (KSym){a, strdup(name)};
    }
    fclose(f);
    qsort(ksyms, (size_t)nksyms, sizeof(KSym), cmp_ksym);
    ksyms_id = id;
    return true;
}

const char *ksym_at(uint64_t addr)
{
    return ksym_at_off(addr, nullptr);
}

const char *ksym_at_off(uint64_t addr, uint64_t *off)
{
    int lo = 0, hi = nksyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ksyms[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0)
        return nullptr;
    if (off)
        *off = addr - ksyms[best].addr;
    return ksyms[best].name;
}

