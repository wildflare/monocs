// 静的ライブラリ（.a）を書く。GNU ar の「ar rcs」（決定的モード: 日時・UID・GID は 0、モード 644）と
// 同じバイト列にする。Monocs のセルフホストのビルド（buildlibc.c）で使う。Linux でも同じ結果を確かめられる
// （tools/selfhost/ar_test.c）。
//
// 形式（System V / GNU の ar 形式。出典: binutils の bfd/archive.c、include/aout/ar.h）:
//   "!<arch>\n"
//   "/"  シンボル表: 数（ビッグエンディアン 32 ビット）、各シンボルを定義するメンバーのヘッダーの位置、
//        NUL で終わる名前の並び
//   "//" 長い名前の表（16 文字以上の名前を "name/\n" で並べる。メンバーは "/位置" で参照する）
//   メンバー: 60 バイトのヘッダー（名前 16、日時 12、UID 6、GID 6、モード 8、大きさ 10、"`\n"）と中身
//   （奇数の大きさなら "\n" で 2 バイト境界にそろえる）
// シンボル表には、各 .o の ELF のシンボル表のうち、定義されている（未定義でない）グローバル・弱いシンボルを、
// メンバーの順、シンボル表の順に入れる。
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;   // アーカイブの中の名前（ディレクトリを除いたファイル名）
    unsigned char *data;
    size_t size;
} ArMember;

static uint64_t ar_rd(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

// ELF64（リトルエンディアン）の .o から、シンボル表に入れる名前を順に fn に渡す
static void ar_symbols(const ArMember *m, void (*fn)(const char *name, void *ctx), void *ctx)
{
    const unsigned char *d = m->data;
    if (m->size < 64 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 2)
        return;
    uint64_t shoff = ar_rd(d + 0x28, 8);
    unsigned shentsize = (unsigned)ar_rd(d + 0x3A, 2), shnum = (unsigned)ar_rd(d + 0x3C, 2);
    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *sh = d + shoff + (uint64_t)i * shentsize;
        if (ar_rd(sh + 4, 4) != 2) // SHT_SYMTAB
            continue;
        uint64_t off = ar_rd(sh + 0x18, 8), size = ar_rd(sh + 0x20, 8);
        uint64_t entsize = ar_rd(sh + 0x38, 8);
        const unsigned char *strsh = d + shoff + ar_rd(sh + 0x28, 4) * shentsize;
        const char *strtab = (const char *)d + ar_rd(strsh + 0x18, 8);
        for (uint64_t k = 1; k < size / entsize; k++) {
            const unsigned char *s = d + off + k * entsize;
            int bind = s[4] >> 4;
            unsigned shndx = (unsigned)ar_rd(s + 6, 2);
            if ((bind == 1 || bind == 2 || bind == 10) && shndx != 0) // GLOBAL・WEAK・GNU_UNIQUE、定義あり
                fn(strtab + ar_rd(s, 4), ctx);
        }
    }
}

typedef struct {
    size_t count, names_len;
    uint32_t *offsets;      // 書くとき: シンボルごとのメンバーの位置
    size_t member_offset;
    char *names;
} ArSymCtx;

static void ar_count_sym(const char *name, void *ctx)
{
    ArSymCtx *c = ctx;
    c->count++;
    c->names_len += strlen(name) + 1;
}

static void ar_put_sym(const char *name, void *ctx)
{
    ArSymCtx *c = ctx;
    size_t n = strlen(name) + 1;
    c->offsets[c->count++] = (uint32_t)c->member_offset;
    memcpy(c->names + c->names_len, name, n);
    c->names_len += n;
}

static void ar_header(FILE *f, const char *name, const char *mode, size_t size, int blank)
{
    char h[128];
    if (blank) // 長い名前の表: 名前と大きさのほかは空白
        snprintf(h, sizeof(h), "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name, "", "", "", "", size);
    else
        snprintf(h, sizeof(h), "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name, "0", "0", "0", mode, size);
    fwrite(h, 1, 60, f);
}

// 成功なら 0
static int ar_write(const char *path, const ArMember *m, size_t n)
{
    // 長い名前の表
    size_t ltab_len = 0;
    for (size_t i = 0; i < n; i++)
        if (strlen(m[i].name) > 15)
            ltab_len += strlen(m[i].name) + 2;
    // シンボル表の大きさ
    ArSymCtx c = {0};
    for (size_t i = 0; i < n; i++)
        ar_symbols(&m[i], ar_count_sym, &c);
    size_t nsyms = c.count, names_len = c.names_len;
    // GNU ar はシンボル表の大きさを偶数にそろえ、そろえた分（NUL）も大きさに含める
    size_t symtab_size = 4 + 4 * nsyms + names_len;
    symtab_size += symtab_size & 1;
    size_t symtab_padded = symtab_size;

    // メンバーの位置
    size_t *pos = malloc(sizeof(size_t) * (n ? n : 1));
    size_t off = 8 + 60 + symtab_padded;
    size_t ltab_size = ltab_len + (ltab_len & 1); // 長い名前の表も、そろえた分（"\n"）を大きさに含める
    if (ltab_len)
        off += 60 + ltab_size;
    for (size_t i = 0; i < n; i++) {
        pos[i] = off;
        off += 60 + m[i].size + (m[i].size & 1);
    }

    // シンボル表の中身
    c = (ArSymCtx){0};
    c.offsets = malloc(sizeof(uint32_t) * (nsyms ? nsyms : 1));
    c.names = malloc(names_len ? names_len : 1);
    for (size_t i = 0; i < n; i++) {
        c.member_offset = pos[i];
        ar_symbols(&m[i], ar_put_sym, &c);
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(pos);
        free(c.offsets);
        free(c.names);
        return 1;
    }
    fwrite("!<arch>\n", 1, 8, f);
    ar_header(f, "/", "0", symtab_size, 0);
    unsigned char be[4];
    for (size_t k = 0; k <= nsyms; k++) {
        uint32_t v = k == 0 ? (uint32_t)nsyms : c.offsets[k - 1];
        be[0] = v >> 24, be[1] = v >> 16, be[2] = v >> 8, be[3] = v;
        fwrite(be, 1, 4, f);
    }
    fwrite(c.names, 1, names_len, f);
    if ((4 + 4 * nsyms + names_len) & 1)
        fputc(0, f);
    // 長い名前の表と、メンバーが参照する位置
    size_t *lpos = malloc(sizeof(size_t) * (n ? n : 1));
    if (ltab_len) {
        ar_header(f, "//", "", ltab_size, 1);
        size_t lo = 0;
        for (size_t i = 0; i < n; i++)
            if (strlen(m[i].name) > 15) {
                lpos[i] = lo;
                fprintf(f, "%s/\n", m[i].name);
                lo += strlen(m[i].name) + 2;
            }
        if (ltab_len & 1)
            fputc('\n', f);
    }
    for (size_t i = 0; i < n; i++) {
        char name[32];
        if (strlen(m[i].name) > 15)
            snprintf(name, sizeof(name), "/%zu", lpos[i]);
        else
            snprintf(name, sizeof(name), "%s/", m[i].name);
        ar_header(f, name, "644", m[i].size, 0);
        fwrite(m[i].data, 1, m[i].size, f);
        if (m[i].size & 1)
            fputc('\n', f);
    }
    int err = ferror(f);
    if (fclose(f) != 0)
        err = 1;
    free(pos);
    free(lpos);
    free(c.offsets);
    free(c.names);
    return err ? 1 : 0;
}
