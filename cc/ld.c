// slimld: Monocs のカーネル用の静的リンカ（API は ld.h）
//
// 手順:
//   1. .o を読み、シンボルを登録する。.a は、未定義のシンボルを定義するメンバーを読み込む
//      （GNU ld と同じく、シンボル索引を先頭から見て、読み込むものがなくなるまで繰り返す。
//      .a はすべて 1 つのグループとして扱う）
//   2. セクションを配置する（os/boot/linker.ld と同じ。下の「配置」を参照）
//   3. GOT を作り、リロケーションを解決して、イメージを書き出す
//
// GNU ld と同じ結果にしている（入力セクションの並び、境界、孤立セクションの位置、
// スクリプトで定義するシンボル）。ただし GOT の項目の並びは最初に参照された順で、
// GNU ld（内部のハッシュ表の順）とは異なる。
//
// 出典: ELF の形式は System V gABI、AArch64 のリロケーションは
// 「ELF for the Arm 64-bit Architecture (AArch64)」（IHI 0056）の 5.7 節。

#include "ld.h"
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX(x, y) ((x) < (y) ? (y) : (x))

// 配列を 2 倍に広げる
#define GROW(arr, cap)                                                   \
  do {                                                                   \
    (cap) = (cap) ? (cap) * 2 : 16;                                      \
    void *p_ = realloc((arr), (size_t)(cap) * sizeof(*(arr)));            \
    if (!p_)                                                             \
      ld_error("out of memory");                                         \
    (arr) = p_;                                                          \
  } while (0)

//
// ELF の定数（gABI）
//

// リンカの状態は、カーネルに組み込むとき（SLIMCC_LIBRARY）コアごとに持つ（cc/slimcc.h と同じ）
#ifdef SLIMCC_LIBRARY
# define TLS _Thread_local
#else
# define TLS
#endif

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_GROUP 17

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_TLS 6

#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_ABS32 258
#define R_AARCH64_ABS16 259
#define R_AARCH64_PREL64 260
#define R_AARCH64_PREL32 261
#define R_AARCH64_PREL16 262
#define R_AARCH64_LD_PREL_LO19 273
#define R_AARCH64_ADR_PREL_LO21 274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST8_ABS_LO12_NC 278
#define R_AARCH64_TSTBR14 279
#define R_AARCH64_CONDBR19 280
#define R_AARCH64_JUMP26 282
#define R_AARCH64_CALL26 283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299
#define R_AARCH64_ADR_GOT_PAGE 311
#define R_AARCH64_LD64_GOT_LO12_NC 312
#define R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 541
#define R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC 542
#define R_AARCH64_TLSLE_ADD_TPREL_HI12 549
#define R_AARCH64_TLSLE_ADD_TPREL_LO12 550
#define R_AARCH64_TLSLE_ADD_TPREL_LO12_NC 551
#define R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC 553
#define R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC 555
#define R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC 557
#define R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC 559
#define R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC 571

//
// エラー
//

TLS static jmp_buf ld_error_jb;
TLS static bool ld_verbose;

static _Noreturn void ld_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "slimld: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  longjmp(ld_error_jb, 1);
}

// 確保したものはすべてここに記録し、最後にまとめて解放する
TLS static void **ld_allocs;
TLS static int ld_nalloc, ld_capalloc;

static void *ld_track(void *p) {
  if (!p)
    ld_error("out of memory");
  if (ld_nalloc == ld_capalloc)
    GROW(ld_allocs, ld_capalloc);
  ld_allocs[ld_nalloc++] = p;
  return p;
}

static void *ld_calloc(size_t n, size_t size) {
  return ld_track(calloc(n ? n : 1, size ? size : 1));
}

static char *ld_strdup(const char *s) {
  return ld_track(strdup(s));
}

static char *ld_format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *buf = ld_calloc(1, n + 1);
  va_start(ap, fmt);
  vsnprintf(buf, n + 1, fmt, ap);
  va_end(ap);
  return buf;
}

static void ld_free_all(void) {
  for (int i = 0; i < ld_nalloc; i++)
    free(ld_allocs[i]);
  free(ld_allocs);
  ld_allocs = NULL;
  ld_nalloc = ld_capalloc = 0;
}

// 整列していないかもしれない場所を読み書きする（ar のメンバーは 2 バイト境界）
static uint64_t rd(const uint8_t *p, int n) {
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; i--)
    v = (v << 8) | p[i];
  return v;
}

static void wr(uint8_t *p, uint64_t v, int n) {
  for (int i = 0; i < n; i++)
    p[i] = v >> (8 * i);
}

static uint64_t align_up(uint64_t v, uint64_t align) {
  return align > 1 ? (v + align - 1) / align * align : v;
}

//
// 入力
//

typedef struct OutSection OutSection;
typedef struct InObj InObj;

typedef struct {
  InObj *obj;
  int idx;
  const char *name;
  uint32_t type;
  uint64_t flags;
  uint64_t size;
  uint64_t align;
  const uint8_t *data;
  OutSection *out;
  uint64_t addr;
  bool placed;
  int rela_idx; // このセクションのリロケーション（SHT_RELA）のセクション番号。なければ 0
} InSection;

typedef struct LdSym LdSym;
struct LdSym {
  char *name;
  bool defined;
  bool weak_def;
  bool strong_ref; // 弱くない参照がある（.a のメンバーを読み込む理由になる）
  InObj *obj;      // 定義したファイル
  int shndx;
  uint64_t value;  // shndx が SHN_ABS ならアドレスそのもの
  int got;         // GOT の項目の番号（-1 は未割り当て）
};

struct InObj {
  char *name;
  uint8_t *buf;
  size_t len;
  int nsect;
  InSection *sects;
  const uint8_t *symtab;
  int nsym;
  int first_global;
  const char *strtab;
  LdSym **gsyms; // 大域シンボル（番号 first_global 以降）
  int *local_got; // 局所シンボルの GOT の項目の番号（-1 は未割り当て）
};

// 文字列をキーにする表（開番地法）
typedef struct {
  const char **keys;
  void **vals;
  int cap, used;
} StrMap;

static uint32_t str_hash(const char *s) {
  uint32_t h = 2166136261u; // FNV-1a
  for (; *s; s++)
    h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

static void *map_get(StrMap *m, const char *key) {
  if (!m->cap)
    return NULL;
  for (uint32_t i = str_hash(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (!m->keys[i])
      return NULL;
    if (!strcmp(m->keys[i], key))
      return m->vals[i];
  }
}

static void map_put(StrMap *m, const char *key, void *val);

static void map_grow(StrMap *m) {
  StrMap n = {.cap = m->cap ? m->cap * 2 : 256};
  n.keys = calloc(n.cap, sizeof(char *));
  n.vals = calloc(n.cap, sizeof(void *));
  if (!n.keys || !n.vals)
    ld_error("out of memory");
  for (int i = 0; i < m->cap; i++)
    if (m->keys[i])
      map_put(&n, m->keys[i], m->vals[i]);
  free(m->keys);
  free(m->vals);
  *m = n;
}

// key は呼び出し側が保持する
static void map_put(StrMap *m, const char *key, void *val) {
  if ((m->used + 1) * 2 > m->cap)
    map_grow(m);
  for (uint32_t i = str_hash(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (!m->keys[i]) {
      m->keys[i] = key;
      m->vals[i] = val;
      m->used++;
      return;
    }
    if (!strcmp(m->keys[i], key)) {
      m->vals[i] = val;
      return;
    }
  }
}

static void map_free(StrMap *m) {
  free(m->keys);
  free(m->vals);
  *m = (StrMap){0};
}

TLS static InObj **objs;
TLS static int nobj, capobj;
TLS static StrMap symtab;
TLS static LdSym **all_syms;
TLS static int nsyms, capsyms;

typedef struct {
  uint32_t name;
  uint8_t info;
  uint16_t shndx;
  uint64_t value;
} ElfSym;

static ElfSym get_sym(InObj *o, int i) {
  const uint8_t *p = o->symtab + i * 24;
  return (ElfSym){
    .name = rd(p, 4), .info = p[4], .shndx = rd(p + 6, 2), .value = rd(p + 8, 8),
  };
}

static LdSym *intern(const char *name) {
  LdSym *s = map_get(&symtab, name);
  if (s)
    return s;
  s = ld_calloc(1, sizeof(LdSym));
  s->name = ld_strdup(name);
  s->got = -1;
  map_put(&symtab, s->name, s);
  if (nsyms == capsyms)
    GROW(all_syms, capsyms);
  all_syms[nsyms++] = s;
  return s;
}

// ELF の .o を読み、シンボルを登録する（buf は呼び出し側が確保し、所有権を移す）
static void add_object(char *name, uint8_t *buf, size_t len) {
  InObj *o = ld_calloc(1, sizeof(InObj));
  o->name = name;
  o->buf = buf;
  o->len = len;

  if (len < 64 || memcmp(buf, "\x7f" "ELF", 4) || buf[4] != 2 || buf[5] != 1)
    ld_error("%s: not a 64-bit little-endian ELF file", name);
  if (rd(buf + 16, 2) != 1)
    ld_error("%s: not a relocatable file", name);
  if (rd(buf + 18, 2) != 183)
    ld_error("%s: not an AArch64 file", name);

  uint64_t shoff = rd(buf + 0x28, 8);
  int shentsize = rd(buf + 0x3A, 2);
  int shnum = rd(buf + 0x3C, 2);
  int shstrndx = rd(buf + 0x3E, 2);
  if (shoff + (uint64_t)shnum * shentsize > len || shentsize < 64)
    ld_error("%s: bad section header table", name);

  o->nsect = shnum;
  o->sects = ld_calloc(shnum, sizeof(InSection));
  const uint8_t *sh_strs = buf + rd(buf + shoff + shstrndx * shentsize + 0x18, 8);
  for (int i = 0; i < shnum; i++) {
    const uint8_t *h = buf + shoff + i * shentsize;
    InSection *s = &o->sects[i];
    s->obj = o;
    s->idx = i;
    s->name = (const char *)sh_strs + rd(h, 4);
    s->type = rd(h + 4, 4);
    s->flags = rd(h + 8, 8);
    uint64_t off = rd(h + 0x18, 8);
    s->size = rd(h + 0x20, 8);
    s->align = rd(h + 0x30, 8);
    if (s->align == 0)
      s->align = 1;
    if (s->type != SHT_NOBITS && off + s->size > len)
      ld_error("%s: section %s is out of the file", name, s->name);
    s->data = buf + off;
    if (s->type == SHT_SYMTAB) {
      o->symtab = s->data;
      o->nsym = s->size / 24;
      o->first_global = rd(h + 0x2C, 4); // sh_info
      int link = rd(h + 0x28, 4);
      o->strtab = (const char *)buf + rd(buf + shoff + link * shentsize + 0x18, 8);
    }
  }
  // リロケーションのセクションを、対象のセクションに結びつける
  for (int i = 0; i < shnum; i++) {
    const uint8_t *h = buf + shoff + i * shentsize;
    if (o->sects[i].type == SHT_RELA) {
      int target = rd(h + 0x2C, 4);
      if (target <= 0 || target >= shnum)
        ld_error("%s: bad relocation section", name);
      o->sects[target].rela_idx = i;
    }
  }

  // シンボル
  o->gsyms = ld_calloc(o->nsym, sizeof(LdSym *));
  o->local_got = ld_calloc(o->nsym, sizeof(int));
  for (int i = 0; i < o->nsym; i++)
    o->local_got[i] = -1;
  for (int i = o->first_global; i < o->nsym; i++) {
    ElfSym es = get_sym(o, i);
    int bind = es.info >> 4;
    const char *sname = o->strtab + es.name;
    if (bind != STB_GLOBAL && bind != STB_WEAK)
      ld_error("%s: bad symbol binding of %s", name, sname);
    if (es.shndx == SHN_COMMON)
      ld_error("%s: common symbol %s is not supported (use -fno-common)", name, sname);
    LdSym *s = intern(sname);
    o->gsyms[i] = s;
    if (es.shndx == SHN_UNDEF) {
      if (bind == STB_GLOBAL)
        s->strong_ref = true;
      continue;
    }
    bool weak = bind == STB_WEAK;
    if (s->defined) {
      if (weak || s->weak_def == false) {
        if (!weak && !s->weak_def)
          ld_error("multiple definition of '%s' (%s and %s)", sname, s->obj->name, name);
        continue;
      }
    }
    s->defined = true;
    s->weak_def = weak;
    s->obj = o;
    s->shndx = es.shndx;
    s->value = es.value;
  }

  if (nobj == capobj)
    GROW(objs, capobj);
  objs[nobj++] = o;
  if (ld_verbose)
    fprintf(stderr, "slimld: loaded %s\n", name);
}

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    ld_error("%s: cannot open: %s", path, strerror(errno));
  size_t cap = 1 << 16, n = 0;
  uint8_t *buf = malloc(cap);
  for (;;) {
    if (!buf)
      ld_error("out of memory");
    size_t r = fread(buf + n, 1, cap - n, fp);
    n += r;
    if (n < cap)
      break;
    buf = realloc(buf, cap *= 2);
  }
  fclose(fp);
  *len = n;
  return ld_track(buf);
}

//
// .a（ar 形式。GNU のシンボル索引 "/" と長い名前の表 "//"）
//

typedef struct {
  char *path;
  uint8_t *buf;
  size_t len;
  int nsym;
  const char **sym_names;
  uint64_t *sym_member; // メンバーのヘッダーの位置
  const char *longnames;
  size_t longnames_len;
  StrMap loaded;        // 読み込んだメンバー（位置の文字列）
} Archive;

static uint64_t ar_dec(const uint8_t *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++)
    v = v * 10 + (p[i] - '0');
  return v;
}

static Archive *open_archive(const char *path, uint8_t *buf, size_t len) {
  Archive *ar = ld_calloc(1, sizeof(Archive));
  ar->path = ld_strdup(path);
  ar->buf = buf;
  ar->len = len;
  bool have_index = false;
  for (uint64_t ofs = 8; ofs + 60 <= len;) {
    const uint8_t *h = buf + ofs;
    uint64_t size = ar_dec(h + 48, 10);
    const uint8_t *data = h + 60;
    if (ofs + 60 + size > len)
      ld_error("%s: broken archive", path);
    if (!memcmp(h, "/               ", 16) || !memcmp(h, "/SYM64/         ", 16)) {
      int w = h[1] == 'S' ? 8 : 4;
      // 個数とメンバーの位置は大きい方を先に置く（ビッグエンディアン）
      uint64_t n = 0;
      for (int i = 0; i < w; i++)
        n = (n << 8) | data[i];
      ar->nsym = n;
      ar->sym_names = ld_calloc(n, sizeof(char *));
      ar->sym_member = ld_calloc(n, sizeof(uint64_t));
      const char *names = (const char *)data + w + n * w;
      for (uint64_t i = 0; i < n; i++) {
        uint64_t m = 0;
        for (int j = 0; j < w; j++)
          m = (m << 8) | data[w + i * w + j];
        ar->sym_member[i] = m;
        ar->sym_names[i] = names;
        names += strlen(names) + 1;
      }
      have_index = true;
    } else if (!memcmp(h, "//              ", 16)) {
      ar->longnames = (const char *)data;
      ar->longnames_len = size;
    }
    ofs += 60 + size + (size & 1);
  }
  if (!have_index)
    ld_error("%s: archive has no symbol index (run ranlib)", path);
  return ar;
}

static char *member_name(Archive *ar, const uint8_t *h) {
  char name[17];
  if (h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
    uint64_t ofs = ar_dec(h + 1, 15);
    if (!ar->longnames || ofs >= ar->longnames_len)
      ld_error("%s: bad long member name", ar->path);
    const char *p = ar->longnames + ofs;
    const char *e = p;
    while (e < ar->longnames + ar->longnames_len && *e != '/' && *e != '\n')
      e++;
    return ld_format("%s(%.*s)", ar->path, (int)(e - p), p);
  }
  memcpy(name, h, 16);
  name[16] = '\0';
  for (int i = 0; i < 16; i++)
    if (name[i] == '/' || name[i] == ' ') {
      name[i] = '\0';
      break;
    }
  return ld_format("%s(%s)", ar->path, name);
}

static void load_member(Archive *ar, uint64_t ofs) {
  if (ofs + 60 > ar->len)
    ld_error("%s: bad member offset", ar->path);
  const uint8_t *h = ar->buf + ofs;
  uint64_t size = ar_dec(h + 48, 10);
  if (ofs + 60 + size > ar->len)
    ld_error("%s: broken archive", ar->path);
  // ELF の構造を整列した位置で読めるよう、コピーする
  uint8_t *buf = ld_track(malloc(size ? size : 1));
  memcpy(buf, h + 60, size);
  add_object(member_name(ar, h), buf, size);
}

// 未定義のシンボルを定義するメンバーを読み込む。読み込んだら true
static bool scan_archive(Archive *ar) {
  bool any = false;
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < ar->nsym; i++) {
      char key[32];
      snprintf(key, sizeof(key), "%llu", (unsigned long long)ar->sym_member[i]);
      if (map_get(&ar->loaded, key))
        continue;
      LdSym *s = map_get(&symtab, ar->sym_names[i]);
      if (!s || s->defined || !s->strong_ref)
        continue;
      map_put(&ar->loaded, ld_strdup(key), (void *)1);
      load_member(ar, ar->sym_member[i]);
      again = any = true;
    }
  }
  return any;
}

//
// 配置（os/boot/linker.ld と同じにする）
//
//   . = 0x80000
//   .text   : { KEEP(*(.text.boot)) *(.text .text.*) }
//   .rodata : ALIGN(16) { *(.rodata .rodata.*) }
//   .tdata  : ALIGN(16) { __tdata_start = .; *(.tdata .tdata.*) __tdata_end = .; }
//   .tbss   : ALIGN(16) { __tbss_start = .; *(.tbss .tbss.*) __tbss_end = .; }
//   __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));
//   .data   : ALIGN(16) { *(.data .data.*) }
//   （孤立セクション: .got、.got.plt、そのほかの書き込める PROGBITS。GNU ld と同じく .data の後）
//   .bss (NOLOAD) : ALIGN(16) { __bss_start = .; *(.bss .bss.*) *(COMMON) . = ALIGN(16); __bss_end = .; }
//   .stack (NOLOAD) : ALIGN(16) { . += 0x800000; __stack_top = .; }
//   .stack_smp (NOLOAD) : ALIGN(16) { __stack_smp_base = .; . += 3 * 0x800000; }
//   .tls_space (NOLOAD) : ALIGN(4096) { __tls_space = .; . += 4 * 0x100000; }
//   . = ALIGN(4096); __heap_start = .;
//   /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }

#define LOAD_ADDR 0x80000
#define STACK_SIZE 0x800000
#define TLS_SPACE_SIZE (4 * 0x100000)

struct OutSection {
  const char *name;
  uint32_t type;
  uint64_t addr;
  uint64_t size;
  uint64_t align;
  uint8_t *data; // PROGBITS の中身（配置の後で確保）
};

TLS static OutSection *outs[64];
TLS static int nout;

static OutSection *new_out(const char *name, uint32_t type) {
  if (nout == (int)(sizeof(outs) / sizeof(*outs)))
    ld_error("too many output sections");
  OutSection *o = ld_calloc(1, sizeof(OutSection));
  o->name = name;
  o->type = type;
  o->align = 1;
  outs[nout++] = o;
  return o;
}

static bool match(const char *name, const char *pat) {
  size_t n = strlen(pat);
  if (n && pat[n - 1] == '*')
    return !strncmp(name, pat, n - 1);
  return !strcmp(name, pat);
}

static bool match_any(const char *name, const char *const *pats) {
  for (; *pats; pats++)
    if (match(name, *pats))
      return true;
  return false;
}

static bool is_discarded(InSection *s) {
  static const char *const pats[] = {".comment", ".note", ".note.*", ".eh_frame", ".eh_frame_hdr",
                                     NULL};
  return !(s->flags & SHF_ALLOC) || match_any(s->name, pats);
}

TLS static uint64_t dot;

// パターンに合う入力セクションを、ファイルの順・ファイルの中のセクションの順に置く
static void place(OutSection *out, const char *const *pats) {
  for (int i = 0; i < nobj; i++) {
    InObj *o = objs[i];
    for (int j = 1; j < o->nsect; j++) {
      InSection *s = &o->sects[j];
      if (s->placed || is_discarded(s) || s->type == SHT_GROUP || !match_any(s->name, pats))
        continue;
      if (s->type != SHT_PROGBITS && s->type != SHT_NOBITS)
        ld_error("%s: unsupported section type %u of %s", o->name, s->type, s->name);
      dot = align_up(dot, s->align);
      s->addr = dot;
      s->out = out;
      s->placed = true;
      out->align = MAX(out->align, s->align);
      dot += s->size;
    }
  }
}

// 出力セクションの先頭の位置を決める（ALIGN(16) と、中身の境界の大きい方）
static uint64_t max_align_of(const char *const *pats) {
  uint64_t a = 1;
  for (int i = 0; i < nobj; i++)
    for (int j = 1; j < objs[i]->nsect; j++) {
      InSection *s = &objs[i]->sects[j];
      if (!s->placed && !is_discarded(s) && s->type != SHT_GROUP && match_any(s->name, pats))
        a = MAX(a, s->align);
    }
  return a;
}

static void begin_out(OutSection *out, uint64_t align_expr, const char *const *pats) {
  dot = align_up(dot, align_expr);
  dot = align_up(dot, max_align_of(pats));
  out->addr = dot;
}

static void end_out(OutSection *out) {
  out->size = dot - out->addr;
}

// スクリプトで定義するシンボル
static void define_abs(const char *name, uint64_t addr) {
  LdSym *s = map_get(&symtab, name);
  if (!s)
    return; // 参照されていなければ作らない（PROVIDE と同じ扱い。GNU ld は常に作るが結果は同じ）
  if (s->defined && !s->weak_def)
    ld_error("'%s' is defined by both the linker and %s", name, s->obj->name);
  s->defined = true;
  s->weak_def = false;
  s->obj = NULL;
  s->shndx = SHN_ABS;
  s->value = addr;
}

// GOT
typedef struct {
  LdSym *gsym;    // 大域シンボル
  InObj *obj;     // または局所シンボル（obj と番号）
  int sym;
} GotEntry;

TLS static GotEntry *gots;
TLS static int ngot, capgot;

static int *got_slot(InObj *o, int symidx) {
  if (symidx >= o->first_global)
    return &o->gsyms[symidx]->got;
  return &o->local_got[symidx];
}

static void collect_got(void) {
  for (int i = 0; i < nobj; i++) {
    InObj *o = objs[i];
    for (int j = 1; j < o->nsect; j++) {
      InSection *s = &o->sects[j];
      if (!s->placed || !s->rela_idx)
        continue;
      InSection *r = &o->sects[s->rela_idx];
      for (uint64_t k = 0; k < r->size / 24; k++) {
        const uint8_t *p = r->data + k * 24;
        uint64_t info = rd(p + 8, 8);
        int type = info & 0xFFFFFFFF;
        int symidx = info >> 32;
        if (type != R_AARCH64_ADR_GOT_PAGE && type != R_AARCH64_LD64_GOT_LO12_NC)
          continue;
        if (rd(p + 16, 8))
          ld_error("%s: GOT relocation with an addend is not supported", o->name);
        int *slot = got_slot(o, symidx);
        if (*slot >= 0)
          continue;
        if (ngot == capgot)
          GROW(gots, capgot);
        *slot = ngot;
        gots[ngot++] = (GotEntry){
          .gsym = symidx >= o->first_global ? o->gsyms[symidx] : NULL, .obj = o, .sym = symidx,
        };
      }
    }
  }
}

TLS static OutSection *got_out, *gotplt_out;

// TLS（GNU ld と同じ。AArch64 の TCB は 16 バイトで、TP からのオフセットは
// 「TLS の先頭（.tdata）からの位置 + align_up(16, TLS の境界)」）
TLS static uint64_t tls_start, tls_tcb;

static void layout(void) {
  static const char *const text_boot[] = {".text.boot", NULL};
  static const char *const text[] = {".text", ".text.*", NULL};
  static const char *const rodata[] = {".rodata", ".rodata.*", NULL};
  static const char *const tdata[] = {".tdata", ".tdata.*", NULL};
  static const char *const tbss[] = {".tbss", ".tbss.*", NULL};
  static const char *const data[] = {".data", ".data.*", NULL};
  static const char *const bss[] = {".bss", ".bss.*", NULL};
  static const char *const none[] = {NULL};

  nout = 0;
  dot = LOAD_ADDR;
  OutSection *o = new_out(".text", SHT_PROGBITS);
  o->addr = dot;
  place(o, text_boot);
  place(o, text);
  end_out(o);

  o = new_out(".rodata", SHT_PROGBITS);
  begin_out(o, 16, rodata);
  place(o, rodata);
  end_out(o);

  // TLS。GNU ld は .tdata の境界を .tbss の境界まで上げる。.tbss はアドレスを使わない
  // （次のセクションは .tbss の先頭から）
  uint64_t tls_align = MAX(16, MAX(max_align_of(tdata), max_align_of(tbss)));
  o = new_out(".tdata", SHT_PROGBITS);
  begin_out(o, tls_align, tdata);
  tls_start = dot;
  define_abs("__tdata_start", dot);
  place(o, tdata);
  define_abs("__tdata_end", dot);
  end_out(o);
  o->align = MAX(o->align, tls_align);
  o = new_out(".tbss", SHT_NOBITS);
  begin_out(o, 16, tbss);
  define_abs("__tbss_start", dot);
  place(o, tbss);
  define_abs("__tbss_end", dot);
  end_out(o);
  o->align = MAX(o->align, 16);
  tls_align = MAX(outs[nout - 2]->align, o->align);
  define_abs("__tls_align", tls_align);
  tls_tcb = align_up(16, tls_align);
  dot = o->addr;

  o = new_out(".data", SHT_PROGBITS);
  begin_out(o, 16, data);
  place(o, data);
  end_out(o);

  // GOT（GNU ld は最初の入力ファイルに .got と .got.plt を作る。
  // .got の先頭は予約（_DYNAMIC の位置。静的リンクなので 0）、.got.plt は 3 項目の予約）。
  // GOT の項目も _GLOBAL_OFFSET_TABLE_ への参照もなければ作らない（GNU ld と同じ）
  collect_got();
  if (ngot || map_get(&symtab, "_GLOBAL_OFFSET_TABLE_")) {
    got_out = new_out(".got", SHT_PROGBITS);
    dot = align_up(dot, 8);
    got_out->addr = dot;
    got_out->align = 8;
    dot += 8 * (1 + ngot);
    end_out(got_out);
    gotplt_out = new_out(".got.plt", SHT_PROGBITS);
    dot = align_up(dot, 8);
    gotplt_out->addr = dot;
    gotplt_out->align = 8;
    dot += 24;
    end_out(gotplt_out);
    define_abs("_GLOBAL_OFFSET_TABLE_", got_out->addr);
  }

  // そのほかの孤立セクション（書き込める PROGBITS だけ。現れた順に、同じ名前をまとめる）
  for (int i = 0; i < nobj; i++) {
    for (int j = 1; j < objs[i]->nsect; j++) {
      InSection *s = &objs[i]->sects[j];
      if (s->placed || is_discarded(s) || s->type == SHT_GROUP || match_any(s->name, bss))
        continue;
      if (s->type != SHT_PROGBITS || !(s->flags & SHF_WRITE) || (s->flags & SHF_EXECINSTR))
        ld_error("%s: section %s is not handled by the layout", objs[i]->name, s->name);
      const char *pats[] = {s->name, NULL};
      o = new_out(s->name, SHT_PROGBITS);
      begin_out(o, 1, pats);
      place(o, pats);
      end_out(o);
    }
  }

  o = new_out(".bss", SHT_NOBITS);
  begin_out(o, 16, bss);
  define_abs("__bss_start", dot);
  place(o, bss);
  dot = align_up(dot, 16);
  define_abs("__bss_end", dot);
  end_out(o);

  o = new_out(".stack", SHT_NOBITS);
  begin_out(o, 16, none);
  dot += STACK_SIZE;
  define_abs("__stack_top", dot);
  end_out(o);

  // コア 1〜3 のスタック
  o = new_out(".stack_smp", SHT_NOBITS);
  begin_out(o, 16, none);
  define_abs("__stack_smp_base", dot);
  dot += 3 * STACK_SIZE;
  end_out(o);

  // コアごとの TLS の領域（os/kernel/tls.c）
  o = new_out(".tls_space", SHT_NOBITS);
  begin_out(o, 4096, none);
  define_abs("__tls_space", dot);
  dot += TLS_SPACE_SIZE;
  end_out(o);

  dot = align_up(dot, 4096);
  define_abs("__heap_start", dot);

  (void)none;
}

//
// リロケーション
//

static uint64_t section_addr(InObj *o, int shndx) {
  if (shndx <= 0 || shndx >= o->nsect)
    ld_error("%s: bad section index %d", o->name, shndx);
  InSection *s = &o->sects[shndx];
  if (!s->placed)
    ld_error("%s: reference to discarded section %s", o->name, s->name);
  return s->addr;
}

static uint64_t ldsym_addr(LdSym *s) {
  if (!s->defined)
    return 0; // 未定義の弱いシンボル
  if (s->shndx == SHN_ABS)
    return s->value;
  return section_addr(s->obj, s->shndx) + s->value;
}

// シンボルのアドレス。未定義の弱いシンボルなら *undef_weak を true にして 0
static uint64_t sym_addr(InObj *o, int symidx, bool *undef_weak) {
  *undef_weak = false;
  if (symidx >= o->nsym)
    ld_error("%s: bad symbol index %d", o->name, symidx);
  if (symidx >= o->first_global) {
    LdSym *s = o->gsyms[symidx];
    if (!s->defined) {
      if (s->strong_ref)
        ld_error("undefined reference to '%s'", s->name);
      *undef_weak = true;
    }
    return ldsym_addr(s);
  }
  ElfSym es = get_sym(o, symidx);
  if (es.shndx == SHN_ABS)
    return es.value;
  return section_addr(o, es.shndx) + es.value;
}

static uint64_t page(uint64_t v) {
  return v & ~(uint64_t)0xFFF;
}

static void check_range(InObj *o, const char *what, int64_t v, int64_t lo, int64_t hi) {
  if (v < lo || v > hi)
    ld_error("%s: %s out of range (%lld)", o->name, what, (long long)v);
}

static void patch_insn(uint8_t *p, uint32_t mask, uint32_t bits) {
  uint32_t insn = rd(p, 4);
  wr(p, (insn & ~mask) | (bits & mask), 4);
}

static uint64_t got_addr(int idx) {
  return got_out->addr + 8 * (1 + idx);
}

static void relocate_section(InSection *s) {
  InObj *o = s->obj;
  InSection *r = &o->sects[s->rela_idx];
  uint8_t *base = s->out->data + (s->addr - s->out->addr);
  for (uint64_t k = 0; k < r->size / 24; k++) {
    const uint8_t *rp = r->data + k * 24;
    uint64_t off = rd(rp, 8);
    uint64_t info = rd(rp + 8, 8);
    int64_t A = rd(rp + 16, 8);
    int type = info & 0xFFFFFFFF;
    int symidx = info >> 32;
    if (type == R_AARCH64_NONE)
      continue;
    if (off + 4 > s->size && !(type == R_AARCH64_ABS16 || type == R_AARCH64_PREL16))
      ld_error("%s: relocation outside of %s", o->name, s->name);
    bool weak;
    uint64_t S = sym_addr(o, symidx, &weak);
    uint64_t P = s->addr + off;
    uint8_t *loc = base + off;
    int64_t v;

    // 未定義の弱いシンボル（GNU ld と同じ扱い）: bl・b は nop にする。
    // PC 相対のものはシンボルを自分の位置とみなす（オフセットは加数だけ）。絶対値は 0
    if (weak) {
      switch (type) {
      case R_AARCH64_CALL26:
      case R_AARCH64_JUMP26:
        wr(loc, 0xD503201F, 4);
        continue;
      case R_AARCH64_PREL64:
      case R_AARCH64_PREL32:
      case R_AARCH64_PREL16:
      case R_AARCH64_CONDBR19:
      case R_AARCH64_LD_PREL_LO19:
      case R_AARCH64_TSTBR14:
      case R_AARCH64_ADR_PREL_LO21:
      case R_AARCH64_ADR_PREL_PG_HI21:
        S = P;
        break;
      }
    }

    switch (type) {
    case R_AARCH64_ABS64:
      wr(loc, S + A, 8);
      break;
    case R_AARCH64_ABS32:
      v = S + A;
      check_range(o, "R_AARCH64_ABS32", v, INT32_MIN, UINT32_MAX);
      wr(loc, v, 4);
      break;
    case R_AARCH64_ABS16:
      v = S + A;
      check_range(o, "R_AARCH64_ABS16", v, INT16_MIN, UINT16_MAX);
      wr(loc, v, 2);
      break;
    case R_AARCH64_PREL64:
      wr(loc, S + A - P, 8);
      break;
    case R_AARCH64_PREL32:
      v = S + A - P;
      check_range(o, "R_AARCH64_PREL32", v, INT32_MIN, UINT32_MAX);
      wr(loc, v, 4);
      break;
    case R_AARCH64_PREL16:
      v = S + A - P;
      check_range(o, "R_AARCH64_PREL16", v, INT16_MIN, UINT16_MAX);
      wr(loc, v, 2);
      break;
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26:
      v = S + A - P;
      if (v & 3)
        ld_error("%s: misaligned branch target", o->name);
      check_range(o, "branch", v, -(1LL << 27), (1LL << 27) - 1);
      patch_insn(loc, 0x03FFFFFF, v >> 2);
      break;
    case R_AARCH64_CONDBR19:
    case R_AARCH64_LD_PREL_LO19:
      v = S + A - P;
      if (v & 3)
        ld_error("%s: misaligned target", o->name);
      check_range(o, "19-bit offset", v, -(1LL << 20), (1LL << 20) - 1);
      patch_insn(loc, 0x7FFFF << 5, (uint32_t)(v >> 2) << 5);
      break;
    case R_AARCH64_TSTBR14:
      v = S + A - P;
      if (v & 3)
        ld_error("%s: misaligned branch target", o->name);
      check_range(o, "test-and-branch", v, -(1LL << 15), (1LL << 15) - 1);
      patch_insn(loc, 0x3FFF << 5, (uint32_t)(v >> 2) << 5);
      break;
    case R_AARCH64_ADR_PREL_LO21:
      v = S + A - P;
      check_range(o, "adr", v, -(1LL << 20), (1LL << 20) - 1);
      patch_insn(loc, (3u << 29) | (0x7FFFF << 5), ((v & 3) << 29) | (((v >> 2) & 0x7FFFF) << 5));
      break;
    case R_AARCH64_ADR_PREL_PG_HI21:
    case R_AARCH64_ADR_GOT_PAGE:
      if (type == R_AARCH64_ADR_GOT_PAGE)
        S = got_addr(*got_slot(o, symidx)), A = 0;
      v = (int64_t)(page(S + A) - page(P)) >> 12;
      check_range(o, "adrp", v, -(1LL << 20), (1LL << 20) - 1);
      patch_insn(loc, (3u << 29) | (0x7FFFF << 5), ((v & 3) << 29) | (((v >> 2) & 0x7FFFF) << 5));
      break;
    case R_AARCH64_ADD_ABS_LO12_NC:
      patch_insn(loc, 0xFFF << 10, ((S + A) & 0xFFF) << 10);
      break;
    case R_AARCH64_LDST8_ABS_LO12_NC:
    case R_AARCH64_LDST16_ABS_LO12_NC:
    case R_AARCH64_LDST32_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:
    case R_AARCH64_LDST128_ABS_LO12_NC:
    case R_AARCH64_LD64_GOT_LO12_NC: {
      int scale = type == R_AARCH64_LDST8_ABS_LO12_NC    ? 0
                : type == R_AARCH64_LDST16_ABS_LO12_NC   ? 1
                : type == R_AARCH64_LDST32_ABS_LO12_NC   ? 2
                : type == R_AARCH64_LDST128_ABS_LO12_NC  ? 4
                                                         : 3;
      if (type == R_AARCH64_LD64_GOT_LO12_NC)
        S = got_addr(*got_slot(o, symidx)), A = 0;
      uint64_t lo = (S + A) & 0xFFF;
      if (lo & ((1 << scale) - 1))
        ld_error("%s: misaligned low 12 bits for a scaled load/store", o->name);
      patch_insn(loc, 0xFFF << 10, (lo >> scale) << 10);
      break;
    }
    // TLS（local-exec）。値は TP からのオフセット
    case R_AARCH64_TLSLE_ADD_TPREL_HI12:
      v = S + A - tls_start + tls_tcb;
      check_range(o, "TLS offset", v, 0, (1LL << 24) - 1);
      patch_insn(loc, 0xFFF << 10, ((v >> 12) & 0xFFF) << 10);
      break;
    case R_AARCH64_TLSLE_ADD_TPREL_LO12:
    case R_AARCH64_TLSLE_ADD_TPREL_LO12_NC:
      v = S + A - tls_start + tls_tcb;
      if (type == R_AARCH64_TLSLE_ADD_TPREL_LO12)
        check_range(o, "TLS offset", v, 0, 0xFFF);
      patch_insn(loc, 0xFFF << 10, (v & 0xFFF) << 10);
      break;
    case R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC:
    case R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC:
    case R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC:
    case R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC:
    case R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC: {
      int scale = type == R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC    ? 0
                : type == R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC   ? 1
                : type == R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC   ? 2
                : type == R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC  ? 4
                                                                 : 3;
      uint64_t lo = (S + A - tls_start + tls_tcb) & 0xFFF;
      if (lo & ((1 << scale) - 1))
        ld_error("%s: misaligned low 12 bits for a scaled TLS load/store", o->name);
      patch_insn(loc, 0xFFF << 10, (lo >> scale) << 10);
      break;
    }
    // TLS（initial-exec）。静的リンクなので GNU ld と同じく local-exec に書き換える:
    //   adrp xd, :gottprel:sym          → movz xd, #:tprel_g1:sym, lsl #16
    //   ldr  xd, [xn, :gottprel_lo12:sym] → movk xd, #:tprel_g0_nc:sym
    case R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21:
    case R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC: {
      v = S + A - tls_start + tls_tcb;
      check_range(o, "TLS offset", v, 0, UINT32_MAX);
      uint32_t reg = rd(loc, 4) & 0x1F;
      if (type == R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21)
        wr(loc, 0xD2A00000 | (((v >> 16) & 0xFFFF) << 5) | reg, 4);
      else
        wr(loc, 0xF2800000 | ((v & 0xFFFF) << 5) | reg, 4);
      break;
    }
    default:
      ld_error("%s: unsupported relocation type %d in %s", o->name, type, s->name);
    }
  }
}

//
// 出力
//

static void write_output(const SlimldOptions *opt) {
  // 中身を作る
  uint64_t end = LOAD_ADDR;
  for (int i = 0; i < nout; i++) {
    OutSection *o = outs[i];
    if (o->type == SHT_PROGBITS) {
      o->data = ld_calloc(1, o->size);
      if (o->size)
        end = MAX(end, o->addr + o->size);
    }
  }
  for (int i = 0; i < nobj; i++)
    for (int j = 1; j < objs[i]->nsect; j++) {
      InSection *s = &objs[i]->sects[j];
      if (s->placed && s->type == SHT_PROGBITS)
        memcpy(s->out->data + (s->addr - s->out->addr), s->data, s->size);
    }
  for (int i = 0; i < ngot; i++) {
    bool weak;
    uint64_t v = gots[i].gsym ? ldsym_addr(gots[i].gsym) : sym_addr(gots[i].obj, gots[i].sym, &weak);
    if (gots[i].gsym && !gots[i].gsym->defined && gots[i].gsym->strong_ref)
      ld_error("undefined reference to '%s'", gots[i].gsym->name);
    wr(got_out->data + 8 * (1 + i), v, 8);
  }
  for (int i = 0; i < nobj; i++)
    for (int j = 1; j < objs[i]->nsect; j++) {
      InSection *s = &objs[i]->sects[j];
      if (s->placed && s->rela_idx && s->type == SHT_PROGBITS)
        relocate_section(s);
    }

  // イメージ（objcopy -O binary と同じく、読み込み先から最後の PROGBITS の終わりまで）
  uint8_t *img = ld_calloc(1, end - LOAD_ADDR);
  for (int i = 0; i < nout; i++) {
    OutSection *o = outs[i];
    if (o->type == SHT_PROGBITS && o->size)
      memcpy(img + (o->addr - LOAD_ADDR), o->data, o->size);
  }
  FILE *fp = fopen(opt->output, "wb");
  if (!fp)
    ld_error("%s: cannot open: %s", opt->output, strerror(errno));
  if (fwrite(img, 1, end - LOAD_ADDR, fp) != end - LOAD_ADDR || fclose(fp))
    ld_error("%s: write error", opt->output);

  if (opt->map) {
    fp = fopen(opt->map, "w");
    if (!fp)
      ld_error("%s: cannot open: %s", opt->map, strerror(errno));
    for (int i = 0; i < nout; i++)
      fprintf(fp, "section %016llx %08llx %s\n", (unsigned long long)outs[i]->addr,
              (unsigned long long)outs[i]->size, outs[i]->name);
    for (int i = 0; i < nsyms; i++) {
      LdSym *s = all_syms[i];
      if (s->defined)
        fprintf(fp, "%016llx %s\n", (unsigned long long)ldsym_addr(s), s->name);
    }
    // ファイルの中だけの関数（static）も（perf record の関数名。tools/perf/mksym.py）
    for (int k = 0; k < nobj; k++) {
      InObj *o = objs[k];
      for (int i = 1; i < o->first_global; i++) {
        ElfSym es = get_sym(o, i);
        if ((es.info & 0xF) != STT_FUNC || es.shndx == 0 || es.shndx >= o->nsect || !o->sects[es.shndx].placed)
          continue;
        fprintf(fp, "%016llx %s\n", (unsigned long long)(o->sects[es.shndx].addr + es.value),
                o->strtab + es.name);
      }
    }
    fclose(fp);
  }
}

static void reset(void) {
  free(objs);
  objs = NULL;
  nobj = capobj = 0;
  map_free(&symtab);
  free(all_syms);
  all_syms = NULL;
  nsyms = capsyms = 0;
  free(gots);
  gots = NULL;
  ngot = capgot = 0;
  nout = 0;
  got_out = gotplt_out = NULL;
}

TLS static Archive *ars[64];
TLS static int nar;

int slimld_link(const SlimldOptions *opt) {
  ld_verbose = opt->verbose;
  nar = 0;

  if (setjmp(ld_error_jb)) {
    for (int i = 0; i < nar; i++) {
      map_free(&ars[i]->loaded);
    }
    reset();
    ld_free_all();
    return 1;
  }

  for (const char *const *p = opt->inputs; *p; p++) {
    size_t len;
    uint8_t *buf = read_file(*p, &len);
    if (len >= 8 && !memcmp(buf, "!<arch>\n", 8)) {
      if (nar == (int)(sizeof(ars) / sizeof(*ars)))
        ld_error("too many archives");
      ars[nar++] = open_archive(*p, buf, len);
    } else {
      add_object(ld_strdup(*p), buf, len);
    }
  }
  // .a はまとめて 1 つのグループ（--start-group ... --end-group）
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < nar; i++)
      if (scan_archive(ars[i]))
        again = true;
  }

  layout();
  for (int i = 0; i < nsyms; i++)
    if (!all_syms[i]->defined && all_syms[i]->strong_ref)
      ld_error("undefined reference to '%s'", all_syms[i]->name);

  write_output(opt);

  for (int i = 0; i < nar; i++)
    map_free(&ars[i]->loaded);
  reset();
  ld_free_all();
  return 0;
}
