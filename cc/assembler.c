// slimcc-a64 の内蔵アセンブラ
//
// codegen.c が出力するアセンブリ（GNU as の書き方）と、インラインアセンブリでよく使う命令を、
// AArch64 の機械語に変換する。結果はセクション・シンボル・リロケーションの表（AsmObj）で返す。
// ELF の .o への書き出しは別に行う。
//
// 出典: Arm Architecture Reference Manual for A-profile、C4 章（A64 命令の符号化）、
//       ELF for the Arm 64-bit Architecture（AAELF64）5.7 節（リロケーション）
//
// GNU as と同じ結果になるように、次の規則に合わせている（同じ .s を GNU as と比べて確かめる）
// - 同じセクションの、外部に公開していないシンボルへの PC 相対の分岐（b、bl、b.cond、cbz、tbz、adr）は、
//   アセンブル時に解決する。それ以外はリロケーションとして残す
// - 外部に公開していないシンボルへのリロケーション（GOT と TLS を除く）は、「セクション＋オフセット」にする
// - ldr/str の即値が、大きさの倍数でないか負なら、ldur/stur にする

#define SLIMCC_NO_ZONE
#include "slimcc.h"
#include <strings.h>

// R_AARCH64_*（AAELF64 5.7 節）
#define R_ABS64 257
#define R_ABS32 258
#define R_ABS16 259
#define R_PREL64 260
#define R_PREL32 261
#define R_ADR_PREL_LO21 274
#define R_ADR_PREL_PG_HI21 275
#define R_ADD_ABS_LO12_NC 277
#define R_LDST8_ABS_LO12_NC 278
#define R_TSTBR14 279
#define R_CONDBR19 280
#define R_JUMP26 282
#define R_CALL26 283
#define R_LDST16_ABS_LO12_NC 284
#define R_LDST32_ABS_LO12_NC 285
#define R_LDST64_ABS_LO12_NC 286
#define R_LDST128_ABS_LO12_NC 299
#define R_ADR_GOT_PAGE 311
#define R_LD64_GOT_LO12_NC 312
#define R_TLSIE_ADR_GOTTPREL_PAGE21 541
#define R_TLSIE_LD64_GOTTPREL_LO12_NC 542
#define R_TLSLE_ADD_TPREL_HI12 549
#define R_TLSLE_ADD_TPREL_LO12_NC 551
#define R_TLSDESC_ADR_PAGE21 562
#define R_TLSDESC_LD64_LO12 563
#define R_TLSDESC_ADD_LO12 564
#define R_TLSDESC_CALL 569

static bool has_prefix(const char *s, const char *prefix) {
  return !strncmp(s, prefix, strlen(prefix));
}

static char Tolower(char c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}


//
// 一時的なメモリ（asm_assemble の終わりにまとめて解放する）
//

// 一時的な確保は大きな塊から切り出し、最後にまとめて捨てる（文の解析で小さな確保が非常に多く、
// 1 つずつ malloc すると遅い。複数のコアが同時にアセンブルすると、共有のヒープのロックで特に遅くなる）
#define TMP_CHUNK (64 * 1024)

typedef struct TmpBlock TmpBlock;
struct TmpBlock {
  TmpBlock *next;
  size_t used, cap;
  _Alignas(16) char data[];
};

TLS static TmpBlock *tmp_blocks;

static void *tmp_alloc(size_t size) {
  size = (size + 15) & ~(size_t)15;
  if (!tmp_blocks || tmp_blocks->used + size > tmp_blocks->cap) {
    size_t cap = size > TMP_CHUNK / 4 ? size : TMP_CHUNK;
    TmpBlock *b = malloc(sizeof(TmpBlock) + cap);
    if (!b)
      error("out of memory");
    b->used = 0;
    b->cap = cap;
    if (cap != TMP_CHUNK && tmp_blocks) {
      // 大きいものは専用の塊にし、今の塊の残りは使い続ける
      b->next = tmp_blocks->next;
      tmp_blocks->next = b;
      b->used = size;
      return b->data;
    }
    b->next = tmp_blocks;
    tmp_blocks = b;
  }
  void *p = tmp_blocks->data + tmp_blocks->used;
  tmp_blocks->used += size;
  return p;
}

static void tmp_free_all(void) {
  while (tmp_blocks) {
    TmpBlock *next = tmp_blocks->next;
    free(tmp_blocks);
    tmp_blocks = next;
  }
}

static char *tmp_strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *p = tmp_alloc(len + 1);
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

static char *tmp_strdup(const char *s) {
  return tmp_strndup(s, strlen(s));
}

FMTCHK(1, 2)
static char *tmp_format(const char *fmt, ...) {
  // ふつうは slimcc_vformat で 1 回（picolibc の vsnprintf を 2 回通さない。2026-11-16）
  char buf[256];
  bool ok;
  va_list ap;
  va_start(ap, fmt);
  int n = slimcc_vformat(buf, sizeof(buf), fmt, ap, &ok);
  va_end(ap);
  if (ok && n < (int)sizeof(buf)) {
    char *p = tmp_alloc(n + 1);
    memcpy(p, buf, n + 1);
    return p;
  }
  va_start(ap, fmt);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *p = tmp_alloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(p, n + 1, fmt, ap);
  va_end(ap);
  return p;
}

TLS static AsmObj *obj;
TLS static AsmSection *cur_sect;
TLS static const char *src_name;
TLS static int src_line;
TLS static HashMap symmap;
TLS static HashMap sectmap;

FMTCHK(1, 2)
static NORETURN void asm_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s:%d: assembler: ", src_name, src_line);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  cleanup_exit(1);
}

//
// セクション
//

static AsmSection *find_section(const char *name) {
  return hashmap_get(&sectmap, name);
}

// 名前から決まる既定の型・フラグ（GNU as の「特別なセクション」と同じ）
static void section_defaults(const char *name, uint32_t *type, uint64_t *flags) {
  *type = ASM_SHT_PROGBITS;
  *flags = 0;
  if (!strcmp(name, ".text") || has_prefix(name, ".text."))
    *flags = ASM_SHF_ALLOC | ASM_SHF_EXECINSTR;
  else if (!strcmp(name, ".data") || has_prefix(name, ".data."))
    *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE;
  else if (!strcmp(name, ".bss") || has_prefix(name, ".bss."))
    *type = ASM_SHT_NOBITS, *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE;
  else if (!strcmp(name, ".rodata") || has_prefix(name, ".rodata."))
    *flags = ASM_SHF_ALLOC;
  else if (!strcmp(name, ".tdata") || has_prefix(name, ".tdata."))
    *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE | ASM_SHF_TLS;
  else if (!strcmp(name, ".tbss") || has_prefix(name, ".tbss."))
    *type = ASM_SHT_NOBITS, *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE | ASM_SHF_TLS;
  else if (!strcmp(name, ".init_array") || has_prefix(name, ".init_array."))
    *type = ASM_SHT_INIT_ARRAY, *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE;
  else if (!strcmp(name, ".fini_array") || has_prefix(name, ".fini_array."))
    *type = ASM_SHT_FINI_ARRAY, *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE;
  else if (!strcmp(name, ".preinit_array"))
    *type = ASM_SHT_PREINIT_ARRAY, *flags = ASM_SHF_ALLOC | ASM_SHF_WRITE;
  else if (has_prefix(name, ".note"))
    *type = ASM_SHT_NOTE;
}

static AsmSection *get_section(const char *name) {
  AsmSection *sec = find_section(name);
  if (sec)
    return sec;
  sec = calloc(1, sizeof(AsmSection));
  sec->name = strdup(name);
  sec->align = 1;
  section_defaults(name, &sec->type, &sec->flags);
  if (sec->flags & ASM_SHF_EXECINSTR)
    sec->align = 4;
  if (obj->nsect == obj->capsect)
    GrowArr(obj->sects, &obj->capsect);
  sec->index = obj->nsect;
  obj->sects[obj->nsect++] = sec;
  hashmap_put(&sectmap, sec->name, sec);
  return sec;
}

static void reserve(AsmSection *sec, int64_t n) {
  if (sec->type == ASM_SHT_NOBITS)
    return;
  if (sec->size + n > sec->cap) {
    int64_t cap = MAX(sec->cap * 2, sec->size + n + 256);
    sec->data = realloc(sec->data, cap);
    memset(sec->data + sec->cap, 0, cap - sec->cap);
    sec->cap = cap;
  }
}

static void emit_bytes(const void *p, int64_t n) {
  if (cur_sect->type == ASM_SHT_NOBITS) {
    for (int64_t i = 0; i < n; i++)
      if (((const uint8_t *)p)[i])
        asm_error("non-zero data in a nobits section");
    cur_sect->size += n;
    return;
  }
  reserve(cur_sect, n);
  memcpy(cur_sect->data + cur_sect->size, p, n);
  cur_sect->size += n;
}

static void emit_zero(int64_t n) {
  if (n < 0)
    asm_error("negative size");
  reserve(cur_sect, n);
  cur_sect->size += n;
}

static void emit_le(uint64_t v, int n) {
  uint8_t b[8];
  for (int i = 0; i < n; i++)
    b[i] = v >> (8 * i);
  emit_bytes(b, n);
}

static void emit32(uint32_t w) {
  if (!(cur_sect->flags & ASM_SHF_EXECINSTR) && cur_sect->type != ASM_SHT_NOBITS)
    ; // 命令をデータのセクションに置くことも許す
  emit_le(w, 4);
}

static void patch32(AsmSection *sec, int64_t ofs, uint32_t bits) {
  uint32_t w;
  memcpy(&w, sec->data + ofs, 4);
  w |= bits;
  memcpy(sec->data + ofs, &w, 4);
}

static void do_align(int64_t align, bool fill_nop) {
  if (align <= 0 || (align & (align - 1)))
    asm_error("alignment must be a power of 2");
  cur_sect->align = MAX(cur_sect->align, align);
  int64_t pad = align_to(cur_sect->size, align) - cur_sect->size;
  if (fill_nop && (cur_sect->flags & ASM_SHF_EXECINSTR) && cur_sect->size % 4 == 0) {
    for (; pad >= 4; pad -= 4)
      emit32(0xD503201F);
  }
  emit_zero(pad);
}

//
// シンボル
//

static AsmSymbol *get_symbol(const char *name) {
  AsmSymbol *sym = hashmap_get(&symmap, name);
  if (sym)
    return sym;
  sym = calloc(1, sizeof(AsmSymbol));
  sym->name = strdup(name);
  if (obj->nsym == obj->capsym)
    GrowArr(obj->syms, &obj->capsym);
  obj->syms[obj->nsym++] = sym;
  hashmap_put(&symmap, sym->name, sym);
  return sym;
}

static void define_label(const char *name) {
  AsmSymbol *sym = get_symbol(name);
  if (sym->defined)
    asm_error("symbol '%s' is already defined", name);
  sym->defined = true;
  sym->sect = cur_sect;
  sym->value = cur_sect->size;
}

// .set で別名にしたシンボルをたどる
static AsmSymbol *resolve_symbol(AsmSymbol *sym) {
  for (int i = 0; sym->equ && i < 100; i++) {
    AsmSymbol *t = get_symbol(sym->equ);
    sym = t;
  }
  return sym;
}

// 数字のローカルラベル（1: と 1b、1f）
TLS static int numlabel_count[10];

static char *numlabel_name(int n, int idx) {
  return tmp_format(".L.asm.num.%d.%d", n, idx);
}

//
// フィックスアップ（アセンブルの最後に、解決するかリロケーションにする）
//

typedef enum {
  FX_ABS64,
  FX_ABS32,
  FX_ABS16,
  FX_PREL32,
  FX_PREL64,
  FX_B26,      // b
  FX_CALL26,   // bl
  FX_COND19,   // b.cond、cbz、cbnz
  FX_TST14,    // tbz、tbnz
  FX_LD19,     // ldr Rt, label（PC 相対のリテラル）
  FX_ADR21,    // adr
  FX_ADRP,     // adrp sym
  FX_OTHER,    // そのままリロケーションにする（:lo12: など）
} FixKind;

typedef struct {
  AsmSection *sec;
  int64_t ofs;
  FixKind kind;
  int rtype;        // FX_OTHER のときのリロケーションの型
  bool adjustable;  // ローカルなシンボルをセクション＋オフセットにしてよい
  char *sym;
  int64_t addend;
  int line;
} Fixup;

TLS static Fixup *fixups;
TLS static int32_t nfix, capfix;

static void add_fixup(FixKind kind, int rtype, bool adjustable, const char *sym, int64_t addend) {
  if (nfix == capfix)
    GrowArr(fixups, &capfix);
  fixups[nfix++] = (Fixup){
    .sec = cur_sect, .ofs = cur_sect->size, .kind = kind, .rtype = rtype,
    .adjustable = adjustable, .sym = tmp_strdup(sym), .addend = addend, .line = src_line,
  };
  AsmSymbol *s = get_symbol(sym);
  s->referenced = true;
  // TLS のリロケーション（R_AARCH64_TLS*。512〜573）で参照したシンボルは STT_TLS（GNU as と同じ。
  // 未定義の外部の TLS 変数も TLS になり、リンカが TLS の参照と確かめられる）
  if (rtype >= 512 && rtype <= 573 && s->type == ASM_STT_NOTYPE)
    s->type = ASM_STT_TLS;
}

static void add_reloc(AsmSection *sec, int64_t ofs, int type, const char *sym, bool is_section,
                      int64_t addend) {
  if (sec->nrel == sec->caprel)
    GrowArr(sec->rels, &sec->caprel);
  sec->rels[sec->nrel++] =
    (AsmReloc){.offset = ofs, .type = type, .sym = sym, .is_section = is_section, .addend = addend};
}

static bool is_local_defined(AsmSymbol *s) {
  return s->defined && !s->is_common && s->bind == ASM_STB_LOCAL;
}

static void resolve_fixups(void) {
  for (int i = 0; i < nfix; i++) {
    Fixup *f = &fixups[i];
    src_line = f->line;
    AsmSymbol *s = resolve_symbol(get_symbol(f->sym));
    bool pcrel_branch = f->kind == FX_B26 || f->kind == FX_CALL26 || f->kind == FX_COND19 ||
                        f->kind == FX_TST14 || f->kind == FX_ADR21 || f->kind == FX_LD19;

    if (pcrel_branch && is_local_defined(s) && s->sect == f->sec) {
      int64_t d = s->value + f->addend - f->ofs;
      switch (f->kind) {
      case FX_B26:
      case FX_CALL26:
        if (d & 3 || d < -(1 << 27) || d >= (1 << 27))
          asm_error("branch out of range");
        patch32(f->sec, f->ofs, (d >> 2) & 0x3FFFFFF);
        break;
      case FX_COND19:
      case FX_LD19:
        if (d & 3 || d < -(1 << 20) || d >= (1 << 20))
          asm_error(f->kind == FX_LD19 ? "literal out of range" : "conditional branch out of range");
        patch32(f->sec, f->ofs, ((d >> 2) & 0x7FFFF) << 5);
        break;
      case FX_TST14:
        if (d & 3 || d < -(1 << 15) || d >= (1 << 15))
          asm_error("test-and-branch out of range");
        patch32(f->sec, f->ofs, ((d >> 2) & 0x3FFF) << 5);
        break;
      case FX_ADR21:
        if (d < -(1 << 20) || d >= (1 << 20))
          asm_error("adr out of range");
        patch32(f->sec, f->ofs, ((d & 3) << 29) | (((d >> 2) & 0x7FFFF) << 5));
        break;
      default:
        internal_error();
      }
      continue;
    }

    int type;
    switch (f->kind) {
    case FX_ABS64:  type = R_ABS64; break;
    case FX_ABS32:  type = R_ABS32; break;
    case FX_ABS16:  type = R_ABS16; break;
    case FX_PREL32: type = R_PREL32; break;
    case FX_PREL64: type = R_PREL64; break;
    case FX_B26:    type = R_JUMP26; break;
    case FX_CALL26: type = R_CALL26; break;
    case FX_COND19: type = R_CONDBR19; break;
    case FX_TST14:  type = R_TSTBR14; break;
    case FX_LD19:   type = 273; /* R_AARCH64_LD_PREL_LO19 */ break;
    case FX_ADR21:  type = R_ADR_PREL_LO21; break;
    case FX_ADRP:   type = R_ADR_PREL_PG_HI21; break;
    default:        type = f->rtype; break;
    }
    bool adjustable = f->kind != FX_OTHER || f->adjustable;
    if (adjustable && is_local_defined(s))
      add_reloc(f->sec, f->ofs, type, s->sect->name, true, s->value + f->addend);
    else
      add_reloc(f->sec, f->ofs, type, s->name, false, f->addend);
  }
}

//
// 字句の処理
//

static bool is_sym_char(char c) {
  return Isalnum(c) || c == '_' || c == '.' || c == '$';
}

static const char *skip_space(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

// シンボル名（"..." で囲んだものも）を読む。読めなければ NULL
static char *read_symbol(const char **rest, const char *p) {
  p = skip_space(p);
  if (*p == '"') {
    const char *q = p + 1;
    while (*q && *q != '"')
      q++;
    if (*q != '"')
      asm_error("unterminated symbol name");
    *rest = q + 1;
    return tmp_strndup(p + 1, q - p - 1);
  }
  if (!is_sym_char(*p) || Isdigit(*p))
    return NULL;
  const char *q = p;
  while (is_sym_char(*q))
    q++;
  *rest = q;
  return tmp_strndup(p, q - p);
}

// 式の値: 定数、またはシンボル + 定数
typedef struct {
  char *sym;
  int64_t val;
} Expr;

static Expr parse_expr(const char **rest, const char *p);

static Expr parse_primary(const char **rest, const char *p) {
  p = skip_space(p);
  if (*p == '#')
    p = skip_space(p + 1);
  if (*p == '(') {
    Expr e = parse_expr(&p, p + 1);
    p = skip_space(p);
    if (*p != ')')
      asm_error("expected ')'");
    *rest = p + 1;
    return e;
  }
  if (*p == '-') {
    Expr e = parse_primary(rest, p + 1);
    if (e.sym)
      asm_error("cannot negate a symbol");
    e.val = -e.val;
    return e;
  }
  if (*p == '+')
    return parse_primary(rest, p + 1);
  if (*p == '~') {
    Expr e = parse_primary(rest, p + 1);
    if (e.sym)
      asm_error("cannot invert a symbol");
    e.val = ~e.val;
    return e;
  }
  if (*p == '\'') {
    if (p[1] && p[2] == '\'') {
      *rest = p + 3;
      return (Expr){.val = (unsigned char)p[1]};
    }
    asm_error("bad character constant");
  }
  if (Isdigit(*p)) {
    // 数字のローカルラベルの参照（1b、1f）
    if (p[1] == 'b' || p[1] == 'f') {
      if (!is_sym_char(p[2])) {
        int n = *p - '0';
        int idx = p[1] == 'b' ? numlabel_count[n] - 1 : numlabel_count[n];
        if (idx < 0)
          asm_error("undefined local label %cb", *p);
        *rest = p + 2;
        return (Expr){.sym = numlabel_name(n, idx)};
      }
    }
    char *end;
    uint64_t v = strtoull(p, &end, 0);
    // C の整数の接尾辞（GNU as と同じく読み飛ばす）
    while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
      end++;
    *rest = end;
    return (Expr){.val = (int64_t)v};
  }
  if (*p == '.' && !is_sym_char(p[1])) {
    *rest = p + 1;
    return (Expr){.sym = tmp_format(".L.asm.dot.%d.%" PRIi64, cur_sect->index, cur_sect->size)};
  }
  char *name = read_symbol(rest, p);
  if (!name)
    asm_error("expected an expression: '%s'", p);
  return (Expr){.sym = name};
}

static int64_t expr_const(Expr e) {
  if (e.sym)
    asm_error("expected a constant, but got symbol '%s'", e.sym);
  return e.val;
}

// "." を参照したとき用に、現在位置にシンボルを置く
static void bind_dot(Expr *e) {
  if (e->sym && has_prefix(e->sym, ".L.asm.dot.")) {
    AsmSymbol *s = get_symbol(e->sym);
    if (!s->defined) {
      s->defined = true;
      s->sect = cur_sect;
      s->value = cur_sect->size;
    }
  }
}

static Expr parse_term(const char **rest, const char *p) {
  Expr e = parse_primary(&p, p);
  bind_dot(&e);
  for (;;) {
    p = skip_space(p);
    if (*p == '*' || *p == '/' || *p == '%' || (p[0] == '<' && p[1] == '<') ||
        (p[0] == '>' && p[1] == '>') || *p == '&' || *p == '|' || *p == '^') {
      char op = *p;
      p += (op == '<' || op == '>') ? 2 : 1;
      Expr r = parse_primary(&p, p);
      int64_t a = expr_const(e), b = expr_const(r);
      switch (op) {
      case '*': e.val = a * b; break;
      case '/': if (!b) asm_error("division by zero"); e.val = a / b; break;
      case '%': if (!b) asm_error("division by zero"); e.val = a % b; break;
      case '<': e.val = (uint64_t)a << b; break;
      case '>': e.val = a >> b; break;
      case '&': e.val = a & b; break;
      case '|': e.val = a | b; break;
      case '^': e.val = a ^ b; break;
      }
      continue;
    }
    break;
  }
  *rest = p;
  return e;
}

static Expr parse_expr(const char **rest, const char *p) {
  Expr e = parse_term(&p, p);
  for (;;) {
    p = skip_space(p);
    if (*p != '+' && *p != '-')
      break;
    char op = *p;
    Expr r = parse_term(&p, p + 1);
    if (op == '+') {
      if (e.sym && r.sym)
        asm_error("cannot add two symbols");
      if (r.sym)
        e.sym = r.sym;
      e.val += r.val;
      continue;
    }
    // シンボル - シンボル（同じセクションで定義済みなら定数）
    if (r.sym) {
      if (!e.sym)
        asm_error("cannot subtract a symbol from a constant");
      AsmSymbol *a = resolve_symbol(get_symbol(e.sym));
      AsmSymbol *b = resolve_symbol(get_symbol(r.sym));
      if (!a->defined || !b->defined || a->sect != b->sect)
        asm_error("cannot subtract symbols in different sections");
      e.val = a->value + e.val - b->value - r.val;
      e.sym = NULL;
      continue;
    }
    e.val -= r.val;
  }
  *rest = p;
  return e;
}

static Expr parse_expr_all(const char *p) {
  Expr e = parse_expr(&p, p);
  if (*skip_space(p))
    asm_error("junk at end of expression: '%s'", p);
  return e;
}

//
// オペランド
//

#define MAX_OPS 8

// 文を、カッコの外のカンマで区切る
static int split_operands(char *s, char **ops) {
  int n = 0;
  int depth = 0;
  bool in_str = false;
  char *start = s;
  for (char *p = s;; p++) {
    if (*p == '"')
      in_str = !in_str;
    if (!in_str) {
      if (*p == '[' || *p == '{' || *p == '(')
        depth++;
      else if (*p == ']' || *p == '}' || *p == ')')
        depth--;
    }
    if (*p == '\0' || (*p == ',' && depth == 0 && !in_str)) {
      bool end = *p == '\0';
      char *b = (char *)skip_space(start);
      char *e = p;
      while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
      if (e > b || !end) {
        if (n == MAX_OPS)
          asm_error("too many operands");
        // s の中で区切る（呼び出し元は s を写したものを渡し、後で使わない。前は 1 つずつ写していた。2026-11-15）
        *e = '\0';
        ops[n++] = b;
      }
      if (end)
        break;
      start = p + 1;
    }
  }
  return n;
}

typedef enum {
  RK_NONE,
  RK_X,   // x0〜x30、xzr
  RK_W,   // w0〜w30、wzr
  RK_SP,  // sp
  RK_WSP, // wsp
  RK_B,
  RK_H,
  RK_S,
  RK_D,
  RK_Q,
  RK_V,   // v0.16b などの配置指定つき
  RK_VE,  // v0.d[1] などの要素
} RegKind;

typedef struct {
  RegKind kind;
  int num;
  char arr[8]; // RK_V: "16b"、"2d" など。RK_VE: 要素の型（"d"、"s" など）
  int index;   // RK_VE の要素番号
} Reg;

static bool parse_reg(const char *s, Reg *r) {
  *r = (Reg){0};
  s = skip_space(s);
  // x12・w9・d16 などは、特別な名前（sp・xzr など）より先に、数字を自分で読む（strcmp・strtol を
  // 呼ばない。実機で内蔵アセンブラの時間の多くがここだった。2026-11-15）
  if (Isdigit(s[1])) {
    char c = Tolower(s[0]);
    if (!strchr("xwbhsdqv", c))
      return false;
    const char *e = s + 1;
    long n = 0;
    while (Isdigit(*e) && n <= 31)
      n = n * 10 + (*e++ - '0');
    if (c != 'v' && *e == '\0' && n <= 31) {
      r->num = n;
      switch (c) {
      case 'x': r->kind = RK_X; break;
      case 'w': r->kind = RK_W; break;
      case 'b': r->kind = RK_B; break;
      case 'h': r->kind = RK_H; break;
      case 's': r->kind = RK_S; break;
      case 'd': r->kind = RK_D; break;
      case 'q': r->kind = RK_Q; break;
      }
      return !((c == 'x' || c == 'w') && n > 30);
    }
    // v のレジスタ・後ろに続きがあるものは下の今までの読み方で
  }
  if (!strcmp(s, "sp")) {
    r->kind = RK_SP, r->num = 31;
    return true;
  }
  if (!strcmp(s, "wsp")) {
    r->kind = RK_WSP, r->num = 31;
    return true;
  }
  if (!strcmp(s, "xzr")) {
    r->kind = RK_X, r->num = 31;
    return true;
  }
  if (!strcmp(s, "wzr")) {
    r->kind = RK_W, r->num = 31;
    return true;
  }
  if (!strcmp(s, "fp")) {
    r->kind = RK_X, r->num = 29;
    return true;
  }
  if (!strcmp(s, "lr")) {
    r->kind = RK_X, r->num = 30;
    return true;
  }
  char c = Tolower(s[0]);
  if (!strchr("xwbhsdqv", c) || !Isdigit(s[1]))
    return false;
  char *end;
  long n = strtol(s + 1, &end, 10);
  if (n > 31)
    return false;
  r->num = n;
  switch (c) {
  case 'x': r->kind = RK_X; break;
  case 'w': r->kind = RK_W; break;
  case 'b': r->kind = RK_B; break;
  case 'h': r->kind = RK_H; break;
  case 's': r->kind = RK_S; break;
  case 'd': r->kind = RK_D; break;
  case 'q': r->kind = RK_Q; break;
  case 'v': r->kind = RK_V; break;
  }
  if ((c == 'x' || c == 'w') && n > 30)
    return false;
  if (c == 'v') {
    if (*end != '.')
      return *end == '\0';
    end++;
    int i = 0;
    while (Isalnum(*end) && i < 7)
      r->arr[i++] = Tolower(*end++);
    r->arr[i] = '\0';
    if (*end == '[') {
      r->kind = RK_VE;
      r->index = strtol(end + 1, &end, 10);
      if (*end != ']')
        return false;
      end++;
    }
    return *end == '\0';
  }
  return *end == '\0';
}

static bool is_gpr(Reg *r) {
  return r->kind == RK_X || r->kind == RK_W;
}

static bool is_gpr_or_sp(Reg *r) {
  return is_gpr(r) || r->kind == RK_SP || r->kind == RK_WSP;
}

static bool is64(Reg *r) {
  return r->kind == RK_X || r->kind == RK_SP;
}

static Reg need_gpr(const char *s) {
  Reg r;
  if (!parse_reg(s, &r) || !is_gpr(&r))
    asm_error("expected a general register: '%s'", s);
  return r;
}

static Reg need_gpr_sp(const char *s) {
  Reg r;
  if (!parse_reg(s, &r) || !is_gpr_or_sp(&r))
    asm_error("expected a general register or sp: '%s'", s);
  return r;
}

static int64_t parse_imm(const char *s) {
  return expr_const(parse_expr_all(s));
}

// :lo12: などのリロケーション指定
typedef struct {
  char mod[24]; // "lo12"、"got_lo12" など。なければ空
  Expr e;
} RelExpr;

static RelExpr parse_relexpr(const char *s) {
  RelExpr r = {0};
  s = skip_space(s);
  if (*s == '#')
    s = skip_space(s + 1);
  if (*s == ':') {
    const char *q = strchr(s + 1, ':');
    if (!q || q - s - 1 >= (int)sizeof(r.mod))
      asm_error("bad relocation operator: '%s'", s);
    memcpy(r.mod, s + 1, q - s - 1);
    for (char *c = r.mod; *c; c++)
      *c = Tolower(*c);
    s = q + 1;
  }
  r.e = parse_expr_all(s);
  return r;
}

// シフトと拡張（lsl #3、sxtw など）
typedef enum {
  SH_NONE = -1,
  SH_LSL = 0,
  SH_LSR,
  SH_ASR,
  SH_ROR,
  EX_UXTB = 16,
  EX_UXTH,
  EX_UXTW,
  EX_UXTX,
  EX_SXTB,
  EX_SXTH,
  EX_SXTW,
  EX_SXTX,
} ShiftKind;

typedef struct {
  ShiftKind kind;
  int amount;
  bool has_amount;
} Shift;

static bool parse_shift(const char *s, Shift *sh) {
  static const char *const names[] = {"lsl", "lsr", "asr", "ror"};
  static const char *const ext[] = {"uxtb", "uxth", "uxtw", "uxtx",
                                    "sxtb", "sxth", "sxtw", "sxtx"};
  s = skip_space(s);
  *sh = (Shift){.kind = SH_NONE};
  for (int i = 0; i < 4; i++) {
    size_t n = strlen(names[i]);
    if (!strncasecmp(s, names[i], n) && (s[n] == ' ' || s[n] == '\t' || s[n] == '#')) {
      sh->kind = i;
      sh->amount = parse_imm(s + n);
      sh->has_amount = true;
      return true;
    }
  }
  for (int i = 0; i < 8; i++) {
    size_t n = strlen(ext[i]);
    if (!strncasecmp(s, ext[i], n) && (s[n] == '\0' || s[n] == ' ' || s[n] == '\t' || s[n] == '#')) {
      sh->kind = EX_UXTB + i;
      const char *q = skip_space(s + n);
      if (*q) {
        sh->amount = parse_imm(q);
        sh->has_amount = true;
      }
      return true;
    }
  }
  return false;
}

// 条件コード
static int parse_cond(const char *s) {
  static const char *const names[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                      "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
  for (int i = 0; i < 16; i++)
    if (!strcasecmp(s, names[i]))
      return i;
  if (!strcasecmp(s, "cs"))
    return 2;
  if (!strcasecmp(s, "cc"))
    return 3;
  return -1;
}

// メモリオペランド [base], [base, #imm], [base, #imm]!, [base, xM{, ext}], [base, :lo12:sym]
typedef enum {
  AM_OFFSET,   // [base, #imm]
  AM_PRE,      // [base, #imm]!
  AM_POST,     // [base], #imm（後のオペランドで指定）
  AM_REG,      // [base, xM, ...]
  AM_RELOC,    // [base, :lo12:sym]
} AddrMode;

typedef struct {
  AddrMode mode;
  Reg base;
  int64_t imm;
  Reg index;
  Shift shift;
  RelExpr rel;
} Mem;

static bool parse_mem(const char *s, Mem *m) {
  *m = (Mem){.mode = AM_OFFSET};
  s = skip_space(s);
  if (*s != '[')
    return false;
  const char *close = strrchr(s, ']');
  if (!close)
    asm_error("expected ']'");
  char *inner = tmp_strndup(s + 1, close - s - 1);
  const char *after = skip_space(close + 1);
  char *parts[MAX_OPS];
  int n = split_operands(inner, parts);
  if (n < 1)
    asm_error("empty memory operand");
  m->base = need_gpr_sp(parts[0]);
  if (!is64(&m->base))
    asm_error("base register must be 64-bit");
  if (n >= 2) {
    Reg idx;
    if (parse_reg(parts[1], &idx) && is_gpr(&idx)) {
      m->mode = AM_REG;
      m->index = idx;
      m->shift = (Shift){.kind = SH_NONE};
      if (n >= 3 && !parse_shift(parts[2], &m->shift))
        asm_error("bad index shift: '%s'", parts[2]);
    } else {
      const char *q = skip_space(parts[1]);
      if (*q == '#')
        q = skip_space(q + 1);
      if (*q == ':') {
        m->mode = AM_RELOC;
        m->rel = parse_relexpr(q);
      } else {
        m->imm = parse_imm(parts[1]);
      }
    }
  }
  if (*after == '!') {
    if (m->mode != AM_OFFSET)
      asm_error("bad pre-index addressing");
    m->mode = AM_PRE;
  } else if (*after) {
    asm_error("junk after memory operand: '%s'", after);
  }
  return true;
}

//
// 命令の符号化（Arm ARM C4 章）
//

// slimcc-a64 自身でコンパイルできるよう、__builtin_popcountll などは使わない
static int popcount64(uint64_t v) {
  int n = 0;
  for (; v; v &= v - 1)
    n++;
  return n;
}

static int ctz32(uint32_t v) {
  int n = 0;
  while (v && !(v & 1))
    v >>= 1, n++;
  return n;
}

static uint32_t sf_bit(Reg *r) {
  return is64(r) ? 1u << 31 : 0;
}

// 論理演算の即値（ビットマスク即値、C4.1.4 節）。表せなければ false
static bool enc_bitmask(uint64_t imm, bool is64bit, uint32_t *out) {
  if (!is64bit) {
    imm &= 0xFFFFFFFF;
    imm |= imm << 32;
  }
  if (imm == 0 || imm == ~0ULL)
    return false;

  int size = 64;
  while (size > 2) {
    int h = size / 2;
    uint64_t m = (1ULL << h) - 1;
    if ((imm & m) != ((imm >> h) & m))
      break;
    size = h;
  }
  uint64_t mask = size == 64 ? ~0ULL : (1ULL << size) - 1;
  uint64_t e = imm & mask;
  int ones = popcount64(e);
  uint64_t target = ones == 64 ? ~0ULL : (1ULL << ones) - 1;

  for (int r = 0; r < size; r++) {
    uint64_t rot = r ? ((e >> r) | (e << (size - r))) & mask : e;
    if (rot == target) {
      uint32_t immr = (size - r) % size;
      uint32_t imms = ((~(uint32_t)(size * 2 - 1)) & 0x3F) | (ones - 1);
      uint32_t n = size == 64;
      if (!is64bit && n)
        return false;
      *out = (n << 22) | (immr << 16) | (imms << 10);
      return true;
    }
  }
  return false;
}

// 即値の加減算（ADD/SUB immediate）。:lo12: などのリロケーションにも対応する
static void enc_addsub_immv(uint32_t base, bool sub, int64_t v0, uint32_t shift12);

// 即値の add・sub（cmp・cmn も）では、レジスタ番号 31 は xzr ではなく sp（Rn と、フラグを立てないときの Rd）。
// GNU as と同じく xzr・wzr を誤りにする（2026-11-21。黙って sp として組み立て、slimcc の誤った「add w10, wzr, #1」
// が sp + 1 になっていた）
static void check_imm_zr(const Reg *rd, const Reg *rn, bool setflags) {
  if (((rn->kind == RK_X || rn->kind == RK_W) && rn->num == 31) ||
      (rd && !setflags && (rd->kind == RK_X || rd->kind == RK_W) && rd->num == 31))
    asm_error("xzr/wzr is not allowed in add/sub with an immediate (register 31 is sp there)");
}

static void enc_addsub_imm(bool sub, bool setflags, Reg *rd, Reg *rn, const char *op, Shift *sh) {
  check_imm_zr(rd, rn, setflags);
  RelExpr re = parse_relexpr(op);
  uint32_t shift12 = 0;
  if (sh && sh->kind != SH_NONE) {
    if (sh->kind != SH_LSL || (sh->amount != 0 && sh->amount != 12))
      asm_error("shift must be lsl #0 or lsl #12");
    shift12 = sh->amount == 12;
  }
  uint32_t base = sf_bit(rd) | (sub << 30) | (setflags << 29) | 0x11000000 | (rn->num << 5) | rd->num;

  if (re.mod[0] || re.e.sym) {
    int type;
    bool adj = true;
    if (!strcmp(re.mod, "lo12"))
      type = R_ADD_ABS_LO12_NC;
    else if (!strcmp(re.mod, "tprel_hi12"))
      type = R_TLSLE_ADD_TPREL_HI12, adj = false;
    else if (!strcmp(re.mod, "tprel_lo12_nc"))
      type = R_TLSLE_ADD_TPREL_LO12_NC, adj = false;
    else if (!strcmp(re.mod, "tlsdesc_lo12"))
      type = R_TLSDESC_ADD_LO12, adj = false;
    else
      asm_error("unsupported relocation for add/sub: '%s'", op);
    if (sub)
      asm_error("relocation in sub is not supported");
    if (!re.e.sym)
      asm_error("relocation needs a symbol");
    add_fixup(FX_OTHER, type, adj, re.e.sym, re.e.val);
    emit32(base | (shift12 << 22));
    return;
  }

  enc_addsub_immv(base, sub, re.e.val, shift12);
}

// add・sub の即値の本体（base はレジスタと sub・setflags を入れた命令）
static void enc_addsub_immv(uint32_t base, bool sub, int64_t v0, uint32_t shift12) {
  int64_t v = v0;
  if (v < 0 && !shift12) {
    // add x0, x1, #-1 は sub x0, x1, #1
    v = -v;
    sub = !sub;
    base ^= 1u << 30;
  }
  if (!shift12 && v >= 4096 && (v & 0xFFF) == 0 && (v >> 12) < 4096) {
    v >>= 12;
    shift12 = 1;
  }
  if (v < 0 || v > 4095)
    asm_error("immediate out of range for add/sub: %" PRIi64, v0);
  emit32(base | (shift12 << 22) | ((uint32_t)v << 10));
}

static uint32_t ext_option(ShiftKind k) {
  return k - EX_UXTB;
}

// ADD/SUB（即値・シフトつきレジスタ・拡張レジスタ）
static void enc_addsub(bool sub, bool setflags, char **ops, int n) {
  if (n < 3)
    asm_error("add/sub needs 3 operands");
  Reg rd = need_gpr_sp(ops[0]);
  Reg rn = need_gpr_sp(ops[1]);
  Shift sh = {.kind = SH_NONE};
  if (n >= 4 && !parse_shift(ops[3], &sh))
    asm_error("bad shift: '%s'", ops[3]);

  Reg rm;
  if (!parse_reg(ops[2], &rm)) {
    enc_addsub_imm(sub, setflags, &rd, &rn, ops[2], &sh);
    return;
  }
  if (!is_gpr(&rm))
    asm_error("bad operand: '%s'", ops[2]);

  bool use_ext = (sh.kind >= EX_UXTB) || rd.kind == RK_SP || rd.kind == RK_WSP ||
                 rn.kind == RK_SP || rn.kind == RK_WSP;
  if (use_ext) {
    uint32_t option;
    int amount = 0;
    if (sh.kind == SH_NONE || sh.kind == SH_LSL) {
      option = is64(&rd) ? 3 : 2; // UXTX / UXTW
      amount = sh.kind == SH_LSL ? sh.amount : 0;
    } else if (sh.kind >= EX_UXTB) {
      option = ext_option(sh.kind);
      amount = sh.amount;
    } else {
      asm_error("bad shift with sp");
    }
    if (amount > 4)
      asm_error("extend amount out of range");
    emit32(sf_bit(&rd) | (sub << 30) | (setflags << 29) | 0x0B200000 | (rm.num << 16) |
           (option << 13) | (amount << 10) | (rn.num << 5) | rd.num);
    return;
  }
  uint32_t shift = sh.kind == SH_NONE ? 0 : sh.kind;
  if (shift > 2)
    asm_error("bad shift for add/sub");
  emit32(sf_bit(&rd) | (sub << 30) | (setflags << 29) | 0x0B000000 | (shift << 22) |
         (rm.num << 16) | ((uint32_t)sh.amount << 10) | (rn.num << 5) | rd.num);
}

// 論理演算。opc: 0 = and、1 = orr、2 = eor、3 = ands。invert: bic、orn、eon、bics
static void enc_logical2(int opc, bool invert, Reg *rd, Reg *rn, const char *op2, const char *shop) {
  Reg rm;
  if (parse_reg(op2, &rm)) {
    if (!is_gpr(&rm))
      asm_error("bad operand: '%s'", op2);
    Shift sh = {.kind = SH_NONE};
    if (shop && !parse_shift(shop, &sh))
      asm_error("bad shift: '%s'", shop);
    uint32_t shift = sh.kind == SH_NONE ? 0 : sh.kind;
    if (shift > 3)
      asm_error("bad shift for a logical instruction");
    emit32(sf_bit(rd) | (opc << 29) | 0x0A000000 | (shift << 22) | (invert << 21) |
           (rm.num << 16) | ((uint32_t)sh.amount << 10) | (rn->num << 5) | rd->num);
    return;
  }
  uint64_t imm = parse_imm(op2);
  if (invert)
    imm = ~imm;
  uint32_t bits;
  if (!enc_bitmask(imm, is64(rd), &bits))
    asm_error("immediate cannot be encoded as a bitmask: '%s'", op2);
  emit32(sf_bit(rd) | (opc << 29) | 0x12000000 | bits | (rn->num << 5) | rd->num);
}

static void enc_logical(int opc, bool invert, char **ops, int n) {
  if (n < 3)
    asm_error("logical instruction needs 3 operands");
  Reg rd = opc == 3 ? need_gpr(ops[0]) : need_gpr_sp(ops[0]);
  Reg rn = need_gpr(ops[1]);
  enc_logical2(opc, invert, &rd, &rn, ops[2], n >= 4 ? ops[3] : NULL);
}

// MOVZ/MOVN/MOVK。opc: 0 = movn、2 = movz、3 = movk
static void enc_movwide(int opc, char **ops, int n) {
  Reg rd = need_gpr(ops[0]);
  int64_t imm = parse_imm(ops[1]);
  int hw = 0;
  if (n >= 3) {
    Shift sh;
    if (!parse_shift(ops[2], &sh) || sh.kind != SH_LSL || sh.amount % 16)
      asm_error("bad shift for a move wide instruction");
    hw = sh.amount / 16;
  }
  if (imm < 0 || imm > 0xFFFF || hw > (is64(&rd) ? 3 : 1))
    asm_error("immediate out of range: '%s'", ops[1]);
  emit32(sf_bit(&rd) | (opc << 29) | 0x12800000 | (hw << 21) | ((uint32_t)imm << 5) | rd.num);
}

// mov Rd, #imm（GNU as と同じく、movz、movn、ビットマスク即値の orr の順に試す）
static void enc_mov_imm(Reg *rd, int64_t v) {
  bool b64 = is64(rd);
  uint64_t u = b64 ? (uint64_t)v : (uint32_t)v;
  int nhw = b64 ? 4 : 2;
  for (int hw = 0; hw < nhw; hw++) {
    if ((u & ~(0xFFFFULL << (16 * hw))) == 0) {
      emit32(sf_bit(rd) | 0x52800000 | (hw << 21) | ((uint32_t)((u >> (16 * hw)) & 0xFFFF) << 5) |
             rd->num);
      return;
    }
  }
  uint64_t nu = b64 ? ~u : (uint32_t)~u;
  for (int hw = 0; hw < nhw; hw++) {
    if ((nu & ~(0xFFFFULL << (16 * hw))) == 0) {
      emit32(sf_bit(rd) | 0x12800000 | (hw << 21) |
             ((uint32_t)((nu >> (16 * hw)) & 0xFFFF) << 5) | rd->num);
      return;
    }
  }
  uint32_t bits;
  if (enc_bitmask(u, b64, &bits)) {
    emit32(sf_bit(rd) | (1u << 29) | 0x12000000 | bits | (31 << 5) | rd->num);
    return;
  }
  asm_error("immediate cannot be moved by a single instruction: %" PRIi64, v);
}

// ビットフィールド（SBFM/BFM/UBFM）。opc: 0 = sbfm、1 = bfm、2 = ubfm
static void emit_bitfield(int opc, Reg *rd, Reg *rn, int immr, int imms) {
  uint32_t n = is64(rd) ? 1 : 0;
  emit32(sf_bit(rd) | (opc << 29) | 0x13000000 | (n << 22) | ((uint32_t)immr << 16) |
         ((uint32_t)imms << 10) | (rn->num << 5) | rd->num);
}

static int reg_width(Reg *r) {
  return is64(r) ? 64 : 32;
}

// データ処理（2 ソース）: udiv、sdiv、lslv、lsrv、asrv、rorv
static void emit_dp2(uint32_t opcode, Reg *rd, Reg *rn, Reg *rm) {
  emit32(sf_bit(rd) | 0x1AC00000 | (rm->num << 16) | (opcode << 10) | (rn->num << 5) | rd->num);
}

// シフト（lsl、lsr、asr、ror）。即値ならビットフィールドの別名
static void enc_shift(int kind, char **ops, int n) {
  if (n != 3)
    asm_error("shift needs 3 operands");
  Reg rd = need_gpr(ops[0]);
  Reg rn = need_gpr(ops[1]);
  Reg rm;
  if (parse_reg(ops[2], &rm)) {
    static const uint32_t opc[] = {0x8, 0x9, 0xA, 0xB};
    emit_dp2(opc[kind], &rd, &rn, &rm);
    return;
  }
  int w = reg_width(&rd);
  int64_t s = parse_imm(ops[2]);
  if (s < 0 || s >= w)
    asm_error("shift amount out of range");
  switch (kind) {
  case SH_LSL: emit_bitfield(2, &rd, &rn, (w - s) % w, w - 1 - s); return;
  case SH_LSR: emit_bitfield(2, &rd, &rn, s, w - 1); return;
  case SH_ASR: emit_bitfield(0, &rd, &rn, s, w - 1); return;
  case SH_ROR:
    // EXTR Rd, Rn, Rn, #s
    emit32(sf_bit(&rd) | 0x13800000 | (is64(&rd) << 22) | (rn.num << 16) | ((uint32_t)s << 10) |
           (rn.num << 5) | rd.num);
    return;
  }
}

// 条件つき選択（csel など）。op: 0 or 1、op2: 0 or 1
static void emit_condsel(uint32_t op, uint32_t op2, Reg *rd, Reg *rn, Reg *rm, int cond) {
  emit32(sf_bit(rd) | (op << 30) | 0x1A800000 | (rm->num << 16) | ((uint32_t)cond << 12) |
         (op2 << 10) | (rn->num << 5) | rd->num);
}

static int need_cond(const char *s) {
  int c = parse_cond(skip_space(s));
  if (c < 0)
    asm_error("bad condition: '%s'", s);
  return c;
}

//
// ロード・ストア
//

typedef struct {
  uint32_t size; // 0〜3
  uint32_t v;    // SIMD/FP
  uint32_t opc;
  int scale;     // アクセスの大きさ（バイト）
} LdSt;

// 転送するレジスタと命令の種類から、size・V・opc を決める
// 命令の名前 mn が定数の文字列 lit か（1 文字目が違えば strcmp を呼ばない。assemble_insn は名前を
// strcmp の連なりで判定するので、実機で全部の命令の 1 割以上を strcmp で使っていた。2026-11-15）
#define MN_IS(lit) (mn[0] == (lit)[0] && strcmp(mn, (lit)) == 0)

static LdSt ldst_info(const char *mn, Reg *rt, bool is_load) {
  LdSt l = {0};
  if (MN_IS("b") || MN_IS("sb") || MN_IS("h") || MN_IS("sh") ||
      MN_IS("sw")) {
    if (!is_gpr(rt))
      asm_error("expected a general register");
    bool sign = mn[0] == 's';
    char w = sign ? mn[1] : mn[0];
    l.size = w == 'b' ? 0 : w == 'h' ? 1 : 2;
    l.scale = 1 << l.size;
    if (!sign)
      l.opc = is_load ? 1 : 0;
    else
      l.opc = is64(rt) ? 2 : 3;
    if (sign && w == 'w' && !is64(rt))
      asm_error("ldrsw needs a 64-bit register");
    if (sign && !is_load)
      asm_error("bad store");
    return l;
  }
  switch (rt->kind) {
  case RK_X: l.size = 3, l.opc = is_load; break;
  case RK_W: l.size = 2, l.opc = is_load; break;
  case RK_B: l.size = 0, l.v = 1, l.opc = is_load; break;
  case RK_H: l.size = 1, l.v = 1, l.opc = is_load; break;
  case RK_S: l.size = 2, l.v = 1, l.opc = is_load; break;
  case RK_D: l.size = 3, l.v = 1, l.opc = is_load; break;
  case RK_Q: l.size = 0, l.v = 1, l.opc = 2 + is_load, l.scale = 16; return l;
  default:   asm_error("bad register for load/store");
  }
  l.scale = 1 << l.size;
  return l;
}

static int lo12_reloc_for(int scale) {
  switch (scale) {
  case 1:  return R_LDST8_ABS_LO12_NC;
  case 2:  return R_LDST16_ABS_LO12_NC;
  case 4:  return R_LDST32_ABS_LO12_NC;
  case 8:  return R_LDST64_ABS_LO12_NC;
  case 16: return R_LDST128_ABS_LO12_NC;
  }
  internal_error();
}

// ldr/str の各形式。suffix は "b"、"sh" など（ldr なら ""）。unscaled は ldur/stur
// リテラルプール（ldr Rt, =式）。GNU as と同じく、セクションごと・大きさ（4 と 8）ごとにまとめ、
// .ltorg か、アセンブルの終わりにセクションの末尾へ置く。同じ値は 1 つにまとめる
typedef struct {
  AsmSection *sec;
  int size;
  char *sym;
  int64_t val;
  char *label;
} Literal;

TLS static Literal *literals;
TLS static int nlit, caplit;
TLS static int literal_id;

static const char *literal_label(int size, Expr e) {
  for (int i = 0; i < nlit; i++) {
    Literal *l = &literals[i];
    if (l->sec == cur_sect && l->size == size && l->val == e.val &&
        (l->sym ? e.sym && !strcmp(l->sym, e.sym) : !e.sym))
      return l->label;
  }
  if (nlit == caplit)
    GrowArr(literals, &caplit);
  literals[nlit++] = (Literal){
    .sec = cur_sect, .size = size, .sym = e.sym ? tmp_strdup(e.sym) : NULL, .val = e.val,
    .label = tmp_format(".Lliteral%d", literal_id++),
  };
  return literals[nlit - 1].label;
}

static void dir_data(int size, const char *args);

// sec のリテラルを置く（4 バイトのもの、8 バイトのものの順。境界はそれぞれの大きさ）
static void flush_literals(AsmSection *sec) {
  AsmSection *save = cur_sect;
  cur_sect = sec;
  for (int size = 4; size <= 8; size *= 2) {
    bool aligned = false;
    for (int i = 0; i < nlit; i++) {
      Literal *l = &literals[i];
      if (l->sec != sec || l->size != size)
        continue;
      if (!aligned) {
        int64_t pad = align_to(sec->size, size) - sec->size;
        sec->align = MAX(sec->align, size);
        emit_zero(pad);
        aligned = true;
      }
      define_label(l->label);
      if (l->sym) {
        add_fixup(size == 8 ? FX_ABS64 : FX_ABS32, 0, true, l->sym, l->val);
        emit_le(0, size);
      } else {
        emit_le(l->val, size);
      }
    }
  }
  int j = 0;
  for (int i = 0; i < nlit; i++)
    if (literals[i].sec != sec)
      literals[j++] = literals[i];
  nlit = j;
  cur_sect = save;
}

static void enc_ldst_mem(LdSt *lp, bool unscaled, Reg *rtp, Mem *mp);

static void enc_ldst(const char *suffix, bool is_load, bool unscaled, char **ops, int n) {
  if (n < 2)
    asm_error("load/store needs 2 operands");
  Reg rt;
  if (!parse_reg(ops[0], &rt))
    asm_error("bad register: '%s'", ops[0]);
  LdSt l = ldst_info(suffix, &rt, is_load);
  Mem m;
  if (is_load && !suffix[0] && !l.v && *skip_space(ops[1]) == '=') {
    // ldr Rt, =式（リテラルプールに値を置き、そこから読む）
    Expr e = parse_expr_all(skip_space(ops[1]) + 1);
    const char *label = literal_label(is64(&rt) ? 8 : 4, e);
    add_fixup(FX_LD19, 0, true, label, 0);
    emit32((is64(&rt) ? 0x58000000 : 0x18000000) | rt.num);
    return;
  }
  if (!parse_mem(ops[1], &m)) {
    // ldr Rt, label（PC 相対のリテラル）
    if (!is_load || suffix[0] || l.v)
      asm_error("bad memory operand: '%s'", ops[1]);
    Expr e = parse_expr_all(ops[1]);
    if (!e.sym)
      asm_error("bad memory operand: '%s'", ops[1]);
    add_fixup(FX_LD19, 0, true, e.sym, e.val);
    emit32((is64(&rt) ? 0x58000000 : 0x18000000) | rt.num);
    return;
  }
  if (n >= 3) {
    if (m.mode != AM_OFFSET || m.imm)
      asm_error("bad post-index addressing");
    m.mode = AM_POST;
    m.imm = parse_imm(ops[2]);
  }
  enc_ldst_mem(&l, unscaled, &rt, &m);
}

// ロード・ストアの本体（オペランドを読んだ後。かけらの記録からも直接呼ぶ）
static void enc_ldst_mem(LdSt *lp, bool unscaled, Reg *rtp, Mem *mp) {
  LdSt l = *lp;
  Reg rt = *rtp;
  Mem m = *mp;
  uint32_t common = (l.size << 30) | (l.v << 26) | (l.opc << 22) | (m.base.num << 5) | rt.num;

  switch (m.mode) {
  case AM_OFFSET:
    if (!unscaled && m.imm >= 0 && m.imm % l.scale == 0 && m.imm / l.scale < 4096) {
      emit32(0x39000000 | common | ((uint32_t)(m.imm / l.scale) << 10));
      return;
    }
    if (m.imm < -256 || m.imm > 255)
      asm_error("immediate offset out of range: %" PRIi64, m.imm);
    emit32(0x38000000 | common | (((uint32_t)m.imm & 0x1FF) << 12));
    return;
  case AM_PRE:
  case AM_POST:
    if (m.imm < -256 || m.imm > 255)
      asm_error("immediate offset out of range: %" PRIi64, m.imm);
    emit32(0x38000000 | common | (((uint32_t)m.imm & 0x1FF) << 12) |
           (m.mode == AM_PRE ? 0xC00 : 0x400));
    return;
  case AM_REG: {
    uint32_t option, s = 0;
    switch (m.shift.kind) {
    case SH_NONE: option = 3; break;
    case SH_LSL:  option = 3; break;
    case EX_UXTW: option = 2; break;
    case EX_SXTW: option = 6; break;
    case EX_SXTX: option = 7; break;
    default:      asm_error("bad index extend");
    }
    if ((option == 3 || option == 7) && !is64(&m.index))
      asm_error("index register must be 64-bit");
    if ((option == 2 || option == 6) && is64(&m.index))
      asm_error("index register must be 32-bit");
    if (m.shift.has_amount) {
      int log2 = ctz32(l.scale);
      if (m.shift.amount == log2)
        s = 1;
      else if (m.shift.amount != 0)
        asm_error("bad index shift amount");
      else if (m.shift.kind != SH_LSL)
        s = 0;
    }
    emit32(0x38200800 | common | (m.index.num << 16) | (option << 13) | (s << 12));
    return;
  }
  case AM_RELOC: {
    RelExpr *re = &m.rel;
    int type;
    bool adj = true;
    if (!strcmp(re->mod, "lo12"))
      type = lo12_reloc_for(l.scale);
    else if (!strcmp(re->mod, "got_lo12"))
      type = R_LD64_GOT_LO12_NC, adj = false;
    else if (!strcmp(re->mod, "gottprel_lo12"))
      type = R_TLSIE_LD64_GOTTPREL_LO12_NC, adj = false;
    else if (!strcmp(re->mod, "tlsdesc_lo12"))
      type = R_TLSDESC_LD64_LO12, adj = false;
    else
      asm_error("unsupported relocation for load/store: ':%s:'", re->mod);
    if (!re->e.sym)
      asm_error("relocation needs a symbol");
    if (type != lo12_reloc_for(l.scale) && l.scale != 8)
      asm_error("GOT/TLS relocation needs a 64-bit load");
    add_fixup(FX_OTHER, type, adj, re->e.sym, re->e.val);
    emit32(0x39000000 | common);
    return;
  }
  }
  internal_error();
}

static void enc_ldstp_mem(bool is_load, bool sw, Reg *r1p, Reg *r2p, Mem *mp, uint32_t idx);

// ldp/stp
static void enc_ldstp(bool is_load, bool sw, char **ops, int n) {
  if (n < 3)
    asm_error("ldp/stp needs 3 operands");
  Reg r1, r2;
  if (!parse_reg(ops[0], &r1) || !parse_reg(ops[1], &r2) || r1.kind != r2.kind)
    asm_error("bad register pair");
  Mem m;
  if (!parse_mem(ops[2], &m) || (m.mode != AM_OFFSET && m.mode != AM_PRE))
    asm_error("bad memory operand for ldp/stp");
  uint32_t idx = m.mode == AM_PRE ? 3 : 2;
  if (n >= 4) {
    if (m.mode != AM_OFFSET || m.imm)
      asm_error("bad post-index addressing");
    m.imm = parse_imm(ops[3]);
    idx = 1;
  }
  enc_ldstp_mem(is_load, sw, &r1, &r2, &m, idx);
}

// ldp/stp の本体。idx: 1 = 後置、2 = オフセット、3 = 前置
static void enc_ldstp_mem(bool is_load, bool sw, Reg *r1p, Reg *r2p, Mem *mp, uint32_t idx) {
  Reg r1 = *r1p, r2 = *r2p;
  Mem m = *mp;
  uint32_t opc, v = 0;
  int scale;
  switch (r1.kind) {
  case RK_W: opc = 0, scale = 4; break;
  case RK_X: opc = sw ? 1 : 2, scale = sw ? 4 : 8; break;
  case RK_S: opc = 0, v = 1, scale = 4; break;
  case RK_D: opc = 1, v = 1, scale = 8; break;
  case RK_Q: opc = 2, v = 1, scale = 16; break;
  default:   asm_error("bad register for ldp/stp");
  }
  if (m.imm % scale || m.imm / scale < -64 || m.imm / scale > 63)
    asm_error("ldp/stp offset out of range: %" PRIi64, m.imm);
  uint32_t imm7 = (uint32_t)(m.imm / scale) & 0x7F;
  emit32((opc << 30) | 0x28000000 | (v << 26) | (idx << 23) | ((uint32_t)is_load << 22) |
         (imm7 << 15) | (r2.num << 10) | (m.base.num << 5) | r1.num);
}

// 排他・獲得解放のロード・ストア（ldxr、ldaxr、stxr、stlxr、ldar、stlr）
static void enc_ldstx(const char *mn, char **ops, int n) {
  // mn は "ldaxrb" などの先頭から、大きさの接尾辞を除いたもの
  size_t len = strlen(mn);
  char last = mn[len - 1];
  int size = -1;
  char base[16];
  snprintf(base, sizeof(base), "%s", mn);
  if (last == 'b' || last == 'h') {
    size = last == 'b' ? 0 : 1;
    base[len - 1] = '\0';
  }
  uint32_t o2, L, o0;
  bool has_status = false;
  if (!strcmp(base, "ldxr"))       o2 = 0, L = 1, o0 = 0;
  else if (!strcmp(base, "ldaxr")) o2 = 0, L = 1, o0 = 1;
  else if (!strcmp(base, "stxr"))  o2 = 0, L = 0, o0 = 0, has_status = true;
  else if (!strcmp(base, "stlxr")) o2 = 0, L = 0, o0 = 1, has_status = true;
  else if (!strcmp(base, "ldar"))  o2 = 1, L = 1, o0 = 1;
  else if (!strcmp(base, "stlr"))  o2 = 1, L = 0, o0 = 1;
  else asm_error("unknown instruction '%s'", mn);

  int i = 0;
  uint32_t rs = 31;
  if (has_status) {
    Reg s = need_gpr(ops[i++]);
    if (is64(&s))
      asm_error("status register must be 32-bit");
    rs = s.num;
  }
  if (i + 2 > n)
    asm_error("missing operands");
  Reg rt = need_gpr(ops[i++]);
  Mem m;
  if (!parse_mem(ops[i], &m) || m.mode != AM_OFFSET || m.imm)
    asm_error("bad memory operand for an exclusive access");
  if (size < 0)
    size = is64(&rt) ? 3 : 2;
  emit32(((uint32_t)size << 30) | 0x08000000 | (o2 << 23) | (L << 22) | (rs << 16) | (o0 << 15) |
         (31 << 10) | (m.base.num << 5) | rt.num);
}

//
// 分岐
//

static Expr need_target(const char *s) {
  Expr e = parse_expr_all(s);
  if (!e.sym)
    asm_error("branch to an absolute address is not supported");
  return e;
}

static void enc_branch(FixKind kind, uint32_t opcode, const char *target) {
  Expr e = need_target(target);
  add_fixup(kind, 0, true, e.sym, e.val);
  emit32(opcode);
}

//
// システム命令
//

typedef struct {
  const char *name;
  uint32_t enc; // op0:op1:CRn:CRm:op2 を MRS/MSR のビット位置（20:5）に置いたもの
} SysReg;

#define SR(op0, op1, crn, crm, op2) \
  ((((op0) & 3) << 19) | ((op1) << 16) | ((crn) << 12) | ((crm) << 8) | ((op2) << 5))

static const SysReg sysregs[] = {
  {"tpidr_el0", SR(3, 3, 13, 0, 2)},    {"tpidrro_el0", SR(3, 3, 13, 0, 3)},
  {"tpidr_el1", SR(3, 0, 13, 0, 4)},    {"nzcv", SR(3, 3, 4, 2, 0)},
  {"daif", SR(3, 3, 4, 2, 1)},          {"fpcr", SR(3, 3, 4, 4, 0)},
  {"fpsr", SR(3, 3, 4, 4, 1)},          {"currentel", SR(3, 0, 4, 2, 2)},
  {"spsel", SR(3, 0, 4, 2, 0)},         {"cntfrq_el0", SR(3, 3, 14, 0, 0)},
  {"cntpct_el0", SR(3, 3, 14, 0, 1)},   {"cntvct_el0", SR(3, 3, 14, 0, 2)},
  {"pmcr_el0", SR(3, 3, 9, 12, 0)},     {"pmcntenset_el0", SR(3, 3, 9, 12, 1)},
  {"pmccntr_el0", SR(3, 3, 9, 13, 0)},
  // PMU（ARM DDI 0487 D13.4。PMEVCNTR<n>・PMEVTYPER<n> は CRm = 0b10:n[4:3]・0b11:n[4:3]、op2 = n[2:0]）
  {"pmcntenclr_el0", SR(3, 3, 9, 12, 2)},  {"pmovsclr_el0", SR(3, 3, 9, 12, 3)},
  {"pmswinc_el0", SR(3, 3, 9, 12, 4)},  {"pmselr_el0", SR(3, 3, 9, 12, 5)},
  {"pmceid0_el0", SR(3, 3, 9, 12, 6)},  {"pmceid1_el0", SR(3, 3, 9, 12, 7)},
  {"pmxevtyper_el0", SR(3, 3, 9, 13, 1)},  {"pmxevcntr_el0", SR(3, 3, 9, 13, 2)},
  {"pmuserenr_el0", SR(3, 3, 9, 14, 0)},  {"pmovsset_el0", SR(3, 3, 9, 14, 3)},
  {"pmintenset_el1", SR(3, 0, 9, 14, 1)},  {"pmintenclr_el1", SR(3, 0, 9, 14, 2)},
  {"pmccfiltr_el0", SR(3, 3, 14, 15, 7)},  {"mdcr_el2", SR(3, 4, 1, 1, 1)},
  {"pmevcntr0_el0", SR(3, 3, 14, 8, 0)},  {"pmevtyper0_el0", SR(3, 3, 14, 12, 0)},
  {"pmevcntr1_el0", SR(3, 3, 14, 8, 1)},  {"pmevtyper1_el0", SR(3, 3, 14, 12, 1)},
  {"pmevcntr2_el0", SR(3, 3, 14, 8, 2)},  {"pmevtyper2_el0", SR(3, 3, 14, 12, 2)},
  {"pmevcntr3_el0", SR(3, 3, 14, 8, 3)},  {"pmevtyper3_el0", SR(3, 3, 14, 12, 3)},
  {"pmevcntr4_el0", SR(3, 3, 14, 8, 4)},  {"pmevtyper4_el0", SR(3, 3, 14, 12, 4)},
  {"pmevcntr5_el0", SR(3, 3, 14, 8, 5)},  {"pmevtyper5_el0", SR(3, 3, 14, 12, 5)},
  {"pmevcntr6_el0", SR(3, 3, 14, 8, 6)},  {"pmevtyper6_el0", SR(3, 3, 14, 12, 6)},
  {"pmevcntr7_el0", SR(3, 3, 14, 8, 7)},  {"pmevtyper7_el0", SR(3, 3, 14, 12, 7)},
  {"pmevcntr8_el0", SR(3, 3, 14, 9, 0)},  {"pmevtyper8_el0", SR(3, 3, 14, 13, 0)},
  {"pmevcntr9_el0", SR(3, 3, 14, 9, 1)},  {"pmevtyper9_el0", SR(3, 3, 14, 13, 1)},
  {"pmevcntr10_el0", SR(3, 3, 14, 9, 2)},  {"pmevtyper10_el0", SR(3, 3, 14, 13, 2)},
  {"pmevcntr11_el0", SR(3, 3, 14, 9, 3)},  {"pmevtyper11_el0", SR(3, 3, 14, 13, 3)},
  {"pmevcntr12_el0", SR(3, 3, 14, 9, 4)},  {"pmevtyper12_el0", SR(3, 3, 14, 13, 4)},
  {"pmevcntr13_el0", SR(3, 3, 14, 9, 5)},  {"pmevtyper13_el0", SR(3, 3, 14, 13, 5)},
  {"pmevcntr14_el0", SR(3, 3, 14, 9, 6)},  {"pmevtyper14_el0", SR(3, 3, 14, 13, 6)},
  {"pmevcntr15_el0", SR(3, 3, 14, 9, 7)},  {"pmevtyper15_el0", SR(3, 3, 14, 13, 7)},
  {"pmevcntr16_el0", SR(3, 3, 14, 10, 0)},  {"pmevtyper16_el0", SR(3, 3, 14, 14, 0)},
  {"pmevcntr17_el0", SR(3, 3, 14, 10, 1)},  {"pmevtyper17_el0", SR(3, 3, 14, 14, 1)},
  {"pmevcntr18_el0", SR(3, 3, 14, 10, 2)},  {"pmevtyper18_el0", SR(3, 3, 14, 14, 2)},
  {"pmevcntr19_el0", SR(3, 3, 14, 10, 3)},  {"pmevtyper19_el0", SR(3, 3, 14, 14, 3)},
  {"pmevcntr20_el0", SR(3, 3, 14, 10, 4)},  {"pmevtyper20_el0", SR(3, 3, 14, 14, 4)},
  {"pmevcntr21_el0", SR(3, 3, 14, 10, 5)},  {"pmevtyper21_el0", SR(3, 3, 14, 14, 5)},
  {"pmevcntr22_el0", SR(3, 3, 14, 10, 6)},  {"pmevtyper22_el0", SR(3, 3, 14, 14, 6)},
  {"pmevcntr23_el0", SR(3, 3, 14, 10, 7)},  {"pmevtyper23_el0", SR(3, 3, 14, 14, 7)},
  {"pmevcntr24_el0", SR(3, 3, 14, 11, 0)},  {"pmevtyper24_el0", SR(3, 3, 14, 15, 0)},
  {"pmevcntr25_el0", SR(3, 3, 14, 11, 1)},  {"pmevtyper25_el0", SR(3, 3, 14, 15, 1)},
  {"pmevcntr26_el0", SR(3, 3, 14, 11, 2)},  {"pmevtyper26_el0", SR(3, 3, 14, 15, 2)},
  {"pmevcntr27_el0", SR(3, 3, 14, 11, 3)},  {"pmevtyper27_el0", SR(3, 3, 14, 15, 3)},
  {"pmevcntr28_el0", SR(3, 3, 14, 11, 4)},  {"pmevtyper28_el0", SR(3, 3, 14, 15, 4)},
  {"pmevcntr29_el0", SR(3, 3, 14, 11, 5)},  {"pmevtyper29_el0", SR(3, 3, 14, 15, 5)},
  {"pmevcntr30_el0", SR(3, 3, 14, 11, 6)},  {"pmevtyper30_el0", SR(3, 3, 14, 15, 6)},
  {"cntp_tval_el0", SR(3, 3, 14, 2, 0)}, {"cntp_ctl_el0", SR(3, 3, 14, 2, 1)},
  {"cntp_cval_el0", SR(3, 3, 14, 2, 2)}, {"cntv_tval_el0", SR(3, 3, 14, 3, 0)},
  {"cntv_ctl_el0", SR(3, 3, 14, 3, 1)}, {"cntv_cval_el0", SR(3, 3, 14, 3, 2)},
  {"cnthctl_el2", SR(3, 4, 14, 1, 0)},  {"cntvoff_el2", SR(3, 4, 14, 0, 3)},
  {"midr_el1", SR(3, 0, 0, 0, 0)},      {"mpidr_el1", SR(3, 0, 0, 0, 5)},
  {"ctr_el0", SR(3, 3, 0, 0, 1)},       {"dczid_el0", SR(3, 3, 0, 0, 7)},
  {"id_aa64isar0_el1", SR(3, 0, 0, 6, 0)}, {"id_aa64mmfr0_el1", SR(3, 0, 0, 7, 0)},
  {"id_aa64pfr0_el1", SR(3, 0, 0, 4, 0)}, {"id_aa64pfr1_el1", SR(3, 0, 0, 4, 1)},
  {"sctlr_el1", SR(3, 0, 1, 0, 0)},     {"sctlr_el2", SR(3, 4, 1, 0, 0)},
  {"cpacr_el1", SR(3, 0, 1, 0, 2)},     {"cptr_el2", SR(3, 4, 1, 1, 2)},
  {"hcr_el2", SR(3, 4, 1, 1, 0)},       {"hstr_el2", SR(3, 4, 1, 1, 3)},
  {"scr_el3", SR(3, 6, 1, 1, 0)},
  {"ttbr0_el1", SR(3, 0, 2, 0, 0)},     {"ttbr1_el1", SR(3, 0, 2, 0, 1)},
  {"tcr_el1", SR(3, 0, 2, 0, 2)},       {"mair_el1", SR(3, 0, 10, 2, 0)},
  {"vbar_el1", SR(3, 0, 12, 0, 0)},     {"esr_el1", SR(3, 0, 5, 2, 0)},
  {"far_el1", SR(3, 0, 6, 0, 0)},       {"elr_el1", SR(3, 0, 4, 0, 1)},
  {"spsr_el1", SR(3, 0, 4, 0, 0)},      {"sp_el0", SR(3, 0, 4, 1, 0)},
  {"elr_el2", SR(3, 4, 4, 0, 1)},       {"spsr_el2", SR(3, 4, 4, 0, 0)},
  {"elr_el3", SR(3, 6, 4, 0, 1)},       {"spsr_el3", SR(3, 6, 4, 0, 0)},
  {"vpidr_el2", SR(3, 4, 0, 0, 0)},     {"vmpidr_el2", SR(3, 4, 0, 0, 5)},
};

// SYS 命令の別名（tlbi、ic、dc、at）
typedef struct {
  const char *insn;
  const char *name;
  uint8_t op1, crn, crm, op2;
  bool has_rt;
} SysOp;

static const SysOp sysops[] = {
  {"ic", "ialluis", 0, 7, 1, 0, false}, {"ic", "iallu", 0, 7, 5, 0, false},
  {"ic", "ivau", 3, 7, 5, 1, true},
  {"dc", "ivac", 0, 7, 6, 1, true},     {"dc", "isw", 0, 7, 6, 2, true},
  {"dc", "csw", 0, 7, 10, 2, true},     {"dc", "cisw", 0, 7, 14, 2, true},
  {"dc", "zva", 3, 7, 4, 1, true},      {"dc", "cvac", 3, 7, 10, 1, true},
  {"dc", "cvau", 3, 7, 11, 1, true},    {"dc", "civac", 3, 7, 14, 1, true},
  {"at", "s1e1r", 0, 7, 8, 0, true},    {"at", "s1e1w", 0, 7, 8, 1, true},
  {"at", "s1e0r", 0, 7, 8, 2, true},    {"at", "s1e0w", 0, 7, 8, 3, true},
  {"tlbi", "vmalle1is", 0, 8, 3, 0, false}, {"tlbi", "vae1is", 0, 8, 3, 1, true},
  {"tlbi", "aside1is", 0, 8, 3, 2, true},   {"tlbi", "vaae1is", 0, 8, 3, 3, true},
  {"tlbi", "vale1is", 0, 8, 3, 5, true},    {"tlbi", "vaale1is", 0, 8, 3, 7, true},
  {"tlbi", "vmalle1", 0, 8, 7, 0, false},   {"tlbi", "vae1", 0, 8, 7, 1, true},
  {"tlbi", "aside1", 0, 8, 7, 2, true},     {"tlbi", "vaae1", 0, 8, 7, 3, true},
  {"tlbi", "vale1", 0, 8, 7, 5, true},      {"tlbi", "vaale1", 0, 8, 7, 7, true},
  {"tlbi", "alle2", 4, 8, 7, 0, false},     {"tlbi", "alle1", 4, 8, 7, 4, false},
  {"tlbi", "alle2is", 4, 8, 3, 0, false},   {"tlbi", "alle1is", 4, 8, 3, 4, false},
};

static const SysOp *find_sysop(const char *insn, const char *name) {
  name = skip_space(name);
  for (size_t i = 0; i < sizeof(sysops) / sizeof(*sysops); i++)
    if (!strcmp(sysops[i].insn, insn) && !strcasecmp(sysops[i].name, name))
      return &sysops[i];
  return NULL;
}

static uint32_t parse_sysreg(const char *s) {
  s = skip_space(s);
  for (size_t i = 0; i < sizeof(sysregs) / sizeof(*sysregs); i++)
    if (!strcasecmp(s, sysregs[i].name))
      return sysregs[i].enc;
  // s3_3_c13_c0_2 の形
  int op0, op1, crn, crm, op2;
  if (sscanf(s, "%*[sS]%d_%d_%*[cC]%d_%*[cC]%d_%d", &op0, &op1, &crn, &crm, &op2) == 5)
    return SR(op0, op1, crn, crm, op2);
  asm_error("unknown system register '%s'", s);
}

static int parse_barrier(const char *s) {
  static const char *const names[] = {"",    "oshld", "oshst", "osh", "", "nshld", "nshst", "nsh",
                                      "",    "ishld", "ishst", "ish", "", "ld",    "st",    "sy"};
  s = skip_space(s);
  if (!*s)
    return 15;
  for (int i = 0; i < 16; i++)
    if (names[i][0] && !strcasecmp(s, names[i]))
      return i;
  return parse_imm(s);
}

//
// 浮動小数点・SIMD
//

static uint32_t fp_type(Reg *r) {
  switch (r->kind) {
  case RK_S: return 0;
  case RK_D: return 1;
  case RK_H: return 3;
  default:   asm_error("expected a floating-point register (s, d, h)");
  }
}

static Reg need_fpr(const char *s) {
  Reg r;
  if (!parse_reg(s, &r) || (r.kind != RK_S && r.kind != RK_D && r.kind != RK_H))
    asm_error("expected a floating-point register: '%s'", s);
  return r;
}

// 2 ソース（fadd など）
static void enc_fp2(uint32_t opcode, char **ops, int n) {
  if (n != 3)
    asm_error("needs 3 operands");
  Reg rd = need_fpr(ops[0]), rn = need_fpr(ops[1]), rm = need_fpr(ops[2]);
  emit32(0x1E200800 | (fp_type(&rd) << 22) | (rm.num << 16) | (opcode << 12) | (rn.num << 5) |
         rd.num);
}

// 1 ソース（fneg など）
static void enc_fp1(uint32_t opcode, char **ops, int n) {
  if (n != 2)
    asm_error("needs 2 operands");
  Reg rd = need_fpr(ops[0]), rn = need_fpr(ops[1]);
  emit32(0x1E204000 | (fp_type(&rn) << 22) | (opcode << 15) | (rn.num << 5) | rd.num);
}

// 整数と浮動小数点の変換・転送。rmode、opcode は Arm ARM C4.1.96 節の表
static void emit_fpint(Reg *gp, Reg *fp, uint32_t type, uint32_t rmode, uint32_t opcode,
                       bool gp_is_dest) {
  uint32_t rd = gp_is_dest ? gp->num : fp->num;
  uint32_t rn = gp_is_dest ? fp->num : gp->num;
  emit32(sf_bit(gp) | 0x1E200000 | (type << 22) | (rmode << 19) | (opcode << 16) | (rn << 5) | rd);
}

static void enc_fcvt_int(uint32_t rmode, uint32_t opcode, bool to_int, char **ops, int n) {
  if (n != 2)
    asm_error("needs 2 operands");
  if (to_int) {
    Reg rd = need_gpr(ops[0]);
    Reg rn = need_fpr(ops[1]);
    emit_fpint(&rd, &rn, fp_type(&rn), rmode, opcode, true);
  } else {
    Reg rd = need_fpr(ops[0]);
    Reg rn = need_gpr(ops[1]);
    emit_fpint(&rn, &rd, fp_type(&rd), rmode, opcode, false);
  }
}

static void enc_fmov(char **ops, int n) {
  if (n != 2)
    asm_error("fmov needs 2 operands");
  Reg rd, rn;
  if (!parse_reg(ops[0], &rd))
    asm_error("bad operand: '%s'", ops[0]);
  if (!parse_reg(ops[1], &rn)) {
    // fmov d0, #imm（浮動小数点の 8 ビット即値）
    double v = strtod(skip_space(ops[1])[0] == '#' ? skip_space(ops[1]) + 1 : ops[1], NULL);
    for (uint32_t imm8 = 0; imm8 < 256; imm8++) {
      // VFPExpandImm: sign、exp（3 ビット）、frac（4 ビット）
      int sign = imm8 >> 7, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, frac = imm8 & 15;
      int exp = ((!b) << 2 | cd) - 3;
      double f = (1.0 + frac / 16.0) * (exp >= 0 ? (double)(1 << exp) : 1.0 / (1 << -exp));
      if (sign)
        f = -f;
      if (f == v) {
        emit32(0x1E201000 | (fp_type(&rd) << 22) | (imm8 << 13) | rd.num);
        return;
      }
    }
    asm_error("floating-point immediate cannot be encoded: '%s'", ops[1]);
  }
  if (is_gpr(&rd) && rn.kind == RK_VE) {
    // fmov x0, v0.d[1]
    emit_fpint(&rd, &rn, 2, 1, 6, true);
    return;
  }
  if (rd.kind == RK_VE && is_gpr(&rn)) {
    emit_fpint(&rn, &rd, 2, 1, 7, false);
    return;
  }
  if (is_gpr(&rd)) {
    Reg f = need_fpr(ops[1]);
    emit_fpint(&rd, &f, fp_type(&f), 0, 6, true);
    return;
  }
  if (is_gpr(&rn)) {
    Reg f = need_fpr(ops[0]);
    emit_fpint(&rn, &f, fp_type(&f), 0, 7, false);
    return;
  }
  Reg a = need_fpr(ops[0]), b = need_fpr(ops[1]);
  emit32(0x1E204000 | (fp_type(&b) << 22) | (b.num << 5) | a.num);
}

static void enc_fcmp(bool e, char **ops, int n) {
  if (n != 2)
    asm_error("fcmp needs 2 operands");
  Reg rn = need_fpr(ops[0]);
  Reg rm;
  uint32_t opc = e ? 0x10 : 0;
  if (parse_reg(ops[1], &rm)) {
    emit32(0x1E202000 | (fp_type(&rn) << 22) | (rm.num << 16) | (rn.num << 5) | opc);
    return;
  }
  // fcmp s0, #0.0
  emit32(0x1E202000 | (fp_type(&rn) << 22) | (rn.num << 5) | opc | 8);
}

static void enc_fcvt(char **ops, int n) {
  if (n != 2)
    asm_error("fcvt needs 2 operands");
  Reg rd = need_fpr(ops[0]), rn = need_fpr(ops[1]);
  uint32_t opc = rd.kind == RK_S ? 4 : rd.kind == RK_D ? 5 : 7;
  emit32(0x1E204000 | (fp_type(&rn) << 22) | (opc << 15) | (rn.num << 5) | rd.num);
}

static uint32_t vec_q(Reg *r) {
  if (!strcmp(r->arr, "16b") || !strcmp(r->arr, "8h") || !strcmp(r->arr, "4s") ||
      !strcmp(r->arr, "2d"))
    return 1;
  if (!strcmp(r->arr, "8b") || !strcmp(r->arr, "4h") || !strcmp(r->arr, "2s") ||
      !strcmp(r->arr, "1d"))
    return 0;
  asm_error("bad vector arrangement '%s'", r->arr);
}

// SIMD の論理演算（and、orr、eor、bic、orn の .8b/.16b）
static void enc_vlogic(uint32_t base, Reg *rd, Reg *rn, Reg *rm) {
  emit32(base | (vec_q(rd) << 30) | (rm->num << 16) | (rn->num << 5) | rd->num);
}

static uint32_t elem_imm5(Reg *r) {
  switch (r->arr[0]) {
  case 'b': return (r->index << 1) | 1;
  case 'h': return (r->index << 2) | 2;
  case 's': return (r->index << 3) | 4;
  case 'd': return (r->index << 4) | 8;
  }
  asm_error("bad vector element");
}

static void enc_movi(char **ops, int n) {
  Reg rd;
  if (n != 2 || !parse_reg(ops[0], &rd))
    asm_error("bad movi");
  int64_t imm = parse_imm(ops[1]);
  if (rd.kind == RK_V && !strcmp(rd.arr, "2d") && imm == 0) {
    emit32(0x6F00E400 | rd.num);
    return;
  }
  if (rd.kind == RK_D && imm == 0) {
    emit32(0x2F00E400 | rd.num);
    return;
  }
  if (rd.kind == RK_V && (!strcmp(rd.arr, "16b") || !strcmp(rd.arr, "8b")) && imm >= 0 &&
      imm <= 255) {
    emit32(0x0F00E400 | (vec_q(&rd) << 30) | ((uint32_t)(imm >> 5) << 16) |
           ((uint32_t)(imm & 31) << 5) | rd.num);
    return;
  }
  asm_error("unsupported movi form");
}

//
// mov とその他の別名
//

static void enc_mov(char **ops, int n) {
  if (n != 2)
    asm_error("mov needs 2 operands");
  Reg rd, rn;
  if (!parse_reg(ops[0], &rd))
    asm_error("bad operand: '%s'", ops[0]);

  if (rd.kind == RK_V || rd.kind == RK_VE || (parse_reg(ops[1], &rn) && (rn.kind == RK_V || rn.kind == RK_VE))) {
    if (!parse_reg(ops[1], &rn))
      asm_error("bad operand: '%s'", ops[1]);
    if (rd.kind == RK_V && rn.kind == RK_V) {
      enc_vlogic(0x0EA01C00, &rd, &rn, &rn); // orr
      return;
    }
    if (rd.kind == RK_VE && is_gpr(&rn)) {
      emit32(0x4E001C00 | (elem_imm5(&rd) << 16) | (rn.num << 5) | rd.num); // ins
      return;
    }
    if (is_gpr(&rd) && rn.kind == RK_VE) {
      uint32_t q = rn.arr[0] == 'd';
      emit32(0x0E003C00 | (q << 30) | (elem_imm5(&rn) << 16) | (rn.num << 5) | rd.num); // umov
      return;
    }
    if (rd.kind == RK_VE && rn.kind == RK_VE) {
      // ins（要素から要素）
      uint32_t imm5 = elem_imm5(&rd);
      int sz = ctz32(imm5);
      uint32_t imm4 = (uint32_t)rn.index << sz;
      emit32(0x6E000400 | (imm5 << 16) | (imm4 << 11) | (rn.num << 5) | rd.num);
      return;
    }
    asm_error("unsupported vector mov");
  }

  if (!is_gpr_or_sp(&rd))
    asm_error("bad mov destination");
  if (parse_reg(ops[1], &rn)) {
    if (!is_gpr_or_sp(&rn))
      asm_error("bad mov source");
    if (rd.kind == RK_SP || rn.kind == RK_SP || rd.kind == RK_WSP || rn.kind == RK_WSP) {
      // add Rd, Rn, #0
      emit32(sf_bit(&rd) | 0x11000000 | (rn.num << 5) | rd.num);
      return;
    }
    // orr Rd, zr, Rm
    emit32(sf_bit(&rd) | 0x2A000000 | (rn.num << 16) | (31 << 5) | rd.num);
    return;
  }
  enc_mov_imm(&rd, parse_imm(ops[1]));
}

//
// 命令の振り分け
//


// SIMD（Advanced SIMD）のうち、picolibc の memcpy.S・strlen.S などが使うもの。
// 形式は ARM DDI 0487 C4.1.7x（Advanced SIMD three same、two-register misc、shift by immediate、
// copy、load/store multiple structures）。GNU as と同じ結果（test-a64/asm/neon.asm）

// 要素の大きさ（size フィールド）: b=0、h=1、s=2、d=3
static uint32_t vec_size(Reg *r) {
  size_t n = strlen(r->arr);
  switch (n ? r->arr[n - 1] : 0) {
  case 'b': return 0;
  case 'h': return 1;
  case 's': return 2;
  case 'd': return 3;
  }
  asm_error("bad vector arrangement '%s'", r->arr);
}

static Reg need_vec(const char *s) {
  Reg r;
  if (!parse_reg(s, &r) || r.kind != RK_V || !r.arr[0])
    asm_error("expected a vector register: '%s'", s);
  return r;
}

// Vd.T, Vn.T, Vm.T（three same）。base は U と opcode を含む
static void enc_v3same(uint32_t base, bool has_size, char **ops, int n) {
  if (n != 3)
    asm_error("expected 3 operands");
  Reg rd = need_vec(ops[0]), rn = need_vec(ops[1]), rm = need_vec(ops[2]);
  uint32_t size = has_size ? vec_size(&rd) << 22 : 0;
  emit32(base | (vec_q(&rd) << 30) | size | (rm.num << 16) | (rn.num << 5) | rd.num);
}

static bool is_zero_operand(const char *s) {
  s = skip_space(s);
  if (*s == '#')
    s = skip_space(s + 1);
  return !strcmp(s, "0") || !strcmp(s, "0x0") || !strcmp(s, "0.0");
}

// { Vt.T, ... } を ops[*i] から読む（'}' までのオペランドをつないでから分ける）。レジスタの数を返す
static int parse_vec_list(char **ops, int n, int *i, Reg *first) {
  char buf[256] = "";
  for (;;) {
    if (*i >= n)
      asm_error("expected '}'");
    if (buf[0])
      strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, ops[*i], sizeof(buf) - strlen(buf) - 1);
    (*i)++;
    if (strchr(buf, '}'))
      break;
  }
  char *p = (char *)skip_space(buf);
  if (*p != '{')
    asm_error("expected a register list: '%s'", buf);
  p++;
  *strchr(p, '}') = '\0';
  int count = 0, prev = -1;
  for (char *tok = strtok(p, ","); tok; tok = strtok(NULL, ",")) {
    char *e = tok + strlen(tok);
    while (e > tok && (e[-1] == ' ' || e[-1] == '\t'))
      *--e = '\0';
    char *dash = strchr(tok, '-');
    if (dash) {
      // { v0.16b-v3.16b }
      *dash = '\0';
      Reg a = need_vec(tok), b = need_vec(dash + 1);
      if (count == 0)
        *first = a;
      count += (b.num - a.num + 32) % 32 + 1;
      prev = b.num;
      continue;
    }
    Reg r = need_vec(tok);
    if (count == 0)
      *first = r;
    else if (r.num != (prev + 1) % 32)
      asm_error("register list must be consecutive");
    prev = r.num;
    count++;
  }
  if (count < 1 || count > 4)
    asm_error("bad register list");
  return count;
}

// ld1・st1（multiple structures）
static void enc_ldst1(bool is_load, char **ops, int n) {
  int i = 0;
  Reg rt;
  int count = parse_vec_list(ops, n, &i, &rt);
  if (i >= n)
    asm_error("expected an address");
  Mem m;
  if (!parse_mem(ops[i], &m) || m.mode != AM_OFFSET || m.imm)
    asm_error("bad address: '%s'", ops[i]);
  i++;
  static const uint32_t opcodes[] = {0, 7, 10, 6, 2};
  uint32_t q = vec_q(&rt);
  uint32_t insn = (is_load ? 0x0C400000 : 0x0C000000) | (q << 30) | (opcodes[count] << 12) |
                  (vec_size(&rt) << 10) | (m.base.num << 5) | rt.num;
  if (i < n) {
    // 後置インデックス（即値は転送する大きさと同じ、またはレジスタ）
    Reg rm;
    if (parse_reg(ops[i], &rm) && rm.kind == RK_X) {
      insn |= 0x00800000 | (rm.num << 16);
    } else {
      int64_t imm = parse_imm(ops[i]);
      if (imm != count * (q ? 16 : 8))
        asm_error("bad post-index amount");
      insn |= 0x00800000 | (31 << 16);
    }
  }
  emit32(insn);
}

// 戻り値: 処理したら true
static bool assemble_neon(const char *mn, char **ops, int n) {
  static const struct {
    const char *name;
    uint32_t base;
    bool has_size;
  } same[] = {
    {"addp", 0x0E20BC00, true},  {"cmeq", 0x2E208C00, true},  {"cmhs", 0x2E203C00, true},
    {"cmhi", 0x2E203400, true},  {"cmgt", 0x0E203400, true},  {"cmge", 0x0E203C00, true},
    {"umaxp", 0x2E20A400, true}, {"uminp", 0x2E20AC00, true}, {"umax", 0x2E206400, true},
    {"umin", 0x2E206C00, true},  {"bsl", 0x2E601C00, false},  {"bit", 0x2EA01C00, false},
    {"bif", 0x2EE01C00, false},
  };

  // 最初のオペランドがベクトル（v0.16b など）か。名前が合ったときだけ読み解く（どの命令もここを通るので、
  // 前は毎回 parse_reg を呼んでいた。2026-11-15）
  Reg r0;
#define VEC0() (n >= 1 && parse_reg(ops[0], &r0) && r0.kind == RK_V && r0.arr[0])

  // add・sub（ベクトル）
  if (n == 3 && (MN_IS("add") || MN_IS("sub")) && VEC0()) {
    enc_v3same(MN_IS("add") ? 0x0E208400 : 0x2E208400, true, ops, n);
    return true;
  }
  for (size_t i = 0; i < sizeof(same) / sizeof(*same); i++) {
    if (mn[0] != same[i].name[0] || strcmp(mn, same[i].name))
      continue;
    // cmeq Vd, Vn, #0（two-register misc）
    if (n == 3 && is_zero_operand(ops[2])) {
      uint32_t base = MN_IS("cmeq") ? 0x0E209800 : MN_IS("cmgt") ? 0x0E208800
                    : MN_IS("cmge") ? 0x2E208800 : 0;
      if (!base)
        asm_error("'%s' with zero is not supported", mn);
      Reg rd = need_vec(ops[0]), rn = need_vec(ops[1]);
      emit32(base | (vec_q(&rd) << 30) | (vec_size(&rd) << 22) | (rn.num << 5) | rd.num);
      return true;
    }
    enc_v3same(same[i].base, same[i].has_size, ops, n);
    return true;
  }

  // shrn・shrn2・rshrn・rshrn2 Vd.Ta, Vn.Tb, #shift
  if (MN_IS("shrn") || MN_IS("shrn2") || MN_IS("rshrn") ||
      MN_IS("rshrn2")) {
    if (n != 3)
      asm_error("expected 3 operands");
    Reg rd = need_vec(ops[0]), rn = need_vec(ops[1]);
    int64_t sh = parse_imm(ops[2]);
    int esize = 8 << vec_size(&rd);
    if (sh < 1 || sh > esize)
      asm_error("shift amount out of range");
    uint32_t immhb = 2 * esize - sh;
    uint32_t base = mn[0] == 'r' ? 0x0F008C00 : 0x0F008400;
    emit32(base | (vec_q(&rd) << 30) | (immhb << 16) | (rn.num << 5) | rd.num);
    return true;
  }

  // dup Vd.T, Rn ／ dup Vd.T, Vn.Ts[i]
  if (MN_IS("dup") && n == 2 && VEC0()) {
    Reg rn;
    if (!parse_reg(ops[1], &rn))
      asm_error("bad operand: '%s'", ops[1]);
    uint32_t size = vec_size(&r0);
    if (rn.kind == RK_VE) {
      emit32(0x0E000400 | (vec_q(&r0) << 30) | (elem_imm5(&rn) << 16) | (rn.num << 5) | r0.num);
      return true;
    }
    if (rn.kind != RK_X && rn.kind != RK_W)
      asm_error("bad operand: '%s'", ops[1]);
    emit32(0x0E000C00 | (vec_q(&r0) << 30) | ((1u << size) << 16) | (rn.num << 5) | r0.num);
    return true;
  }

  if (MN_IS("ld1") || MN_IS("st1")) {
    enc_ldst1(mn[0] == 'l', ops, n);
    return true;
  }
#undef VEC0
  return false;
}

static const struct { const char *name; const char *suffix; bool load; bool unscaled; } ldst[] = {
  {"ldr", "", true, false},     {"str", "", false, false},     {"ldrb", "b", true, false},
  {"strb", "b", false, false},  {"ldrh", "h", true, false},    {"strh", "h", false, false},
  {"ldrsb", "sb", true, false}, {"ldrsh", "sh", true, false},  {"ldrsw", "sw", true, false},
  {"ldur", "", true, true},     {"stur", "", false, true},     {"ldurb", "b", true, true},
  {"sturb", "b", false, true},  {"ldurh", "h", true, true},    {"sturh", "h", false, true},
  {"ldursb", "sb", true, true}, {"ldursh", "sh", true, true},  {"ldursw", "sw", true, true},
};

// ldr・str などのロード・ストア（assemble_insn）。扱ったら true
static bool assemble_ldst(const char *mn, char **ops, int n) {
  for (size_t i = 0; i < sizeof(ldst) / sizeof(*ldst); i++) {
    if (mn[0] == ldst[i].name[0] && !strcmp(mn, ldst[i].name)) {
      enc_ldst(ldst[i].suffix, ldst[i].load, ldst[i].unscaled, ops, n);
      return true;
    }
  }
  return false;
}

static void assemble_insn(char *mn, char **ops, int n) {
  for (char *p = mn; *p; p++)
    *p = Tolower(*p);

  // よく出る命令は先に（NEON の判定と、長い判定の連なりを通らない。どれも NEON の判定・連なりの前のほうでは
  // 扱わない名前なので、結果は同じ。2026-11-15）
  switch (mn[0]) {
  case 'm':
    if (MN_IS("mov"))  { enc_mov(ops, n); return; }
    if (MN_IS("movz")) { enc_movwide(2, ops, n); return; }
    break;
  case 'l':
  case 's':
    if (assemble_ldst(mn, ops, n))
      return;
    if (MN_IS("ldp"))  { enc_ldstp(true, false, ops, n); return; }
    if (MN_IS("stp"))  { enc_ldstp(false, false, ops, n); return; }
    break;
  case 'b':
    if (MN_IS("bl") && n == 1) { enc_branch(FX_CALL26, 0x94000000, ops[0]); return; }
    if (MN_IS("b") && n == 1)  { enc_branch(FX_B26, 0x14000000, ops[0]); return; }
    break;
  }

  if (assemble_neon(mn, ops, n))
    return;

  // SIMD の論理演算（and、orr、eor、bic、orn の .8b/.16b）
  if (n == 3 && (MN_IS("and") || MN_IS("orr") || MN_IS("eor") ||
                 MN_IS("bic") || MN_IS("orn"))) {
    Reg rd;
    if (parse_reg(ops[0], &rd) && rd.kind == RK_V) {
      Reg rn, rm;
      if (!parse_reg(ops[1], &rn) || !parse_reg(ops[2], &rm))
        asm_error("bad vector operands");
      uint32_t base = MN_IS("and") ? 0x0E201C00 : MN_IS("orr") ? 0x0EA01C00
                    : MN_IS("eor") ? 0x2E201C00 : MN_IS("bic") ? 0x0E601C00
                    : 0x0EE01C00;
      enc_vlogic(base, &rd, &rn, &rm);
      return;
    }
  }

  // 算術・論理
  if (MN_IS("add"))  { enc_addsub(false, false, ops, n); return; }
  if (MN_IS("adds")) { enc_addsub(false, true, ops, n); return; }
  if (MN_IS("sub"))  { enc_addsub(true, false, ops, n); return; }
  if (MN_IS("subs")) { enc_addsub(true, true, ops, n); return; }
  if (MN_IS("cmp") || MN_IS("cmn")) {
    if (n < 2)
      asm_error("cmp needs 2 operands");
    Reg rn;
    if (!parse_reg(ops[0], &rn))
      asm_error("bad operand: '%s'", ops[0]);
    char *ops2[MAX_OPS + 1] = {is64(&rn) || rn.kind == RK_SP ? "xzr" : "wzr"};
    for (int i = 0; i < n; i++)
      ops2[i + 1] = ops[i];
    enc_addsub(mn[2] == 'p', true, ops2, n + 1);
    return;
  }
  if (MN_IS("neg") || MN_IS("negs")) {
    Reg rd = need_gpr(ops[0]);
    char *ops2[MAX_OPS + 1] = {ops[0], is64(&rd) ? "xzr" : "wzr"};
    for (int i = 1; i < n; i++)
      ops2[i + 1] = ops[i];
    enc_addsub(true, mn[3] == 's', ops2, n + 1);
    return;
  }
  if (MN_IS("and"))  { enc_logical(0, false, ops, n); return; }
  if (MN_IS("orr"))  { enc_logical(1, false, ops, n); return; }
  if (MN_IS("eor"))  { enc_logical(2, false, ops, n); return; }
  if (MN_IS("ands")) { enc_logical(3, false, ops, n); return; }
  if (MN_IS("bic"))  { enc_logical(0, true, ops, n); return; }
  if (MN_IS("orn"))  { enc_logical(1, true, ops, n); return; }
  if (MN_IS("eon"))  { enc_logical(2, true, ops, n); return; }
  if (MN_IS("bics")) { enc_logical(3, true, ops, n); return; }
  if (MN_IS("tst")) {
    Reg rn = need_gpr(ops[0]);
    Reg zr = {.kind = is64(&rn) ? RK_X : RK_W, .num = 31};
    enc_logical2(3, false, &zr, &rn, ops[1], n >= 3 ? ops[2] : NULL);
    return;
  }
  if (MN_IS("mvn")) {
    Reg rd = need_gpr(ops[0]);
    Reg zr = {.kind = rd.kind, .num = 31};
    enc_logical2(1, true, &rd, &zr, ops[1], n >= 3 ? ops[2] : NULL);
    return;
  }
  if (MN_IS("mov"))  { enc_mov(ops, n); return; }
  if (MN_IS("movz")) { enc_movwide(2, ops, n); return; }
  if (MN_IS("movn")) { enc_movwide(0, ops, n); return; }
  if (MN_IS("movk")) { enc_movwide(3, ops, n); return; }

  if (MN_IS("lsl")) { enc_shift(SH_LSL, ops, n); return; }
  if (MN_IS("lsr")) { enc_shift(SH_LSR, ops, n); return; }
  if (MN_IS("asr")) { enc_shift(SH_ASR, ops, n); return; }
  if (MN_IS("ror")) { enc_shift(SH_ROR, ops, n); return; }

  if (MN_IS("sxtb") || MN_IS("sxth") || MN_IS("sxtw") ||
      MN_IS("uxtb") || MN_IS("uxth")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]);
    int imms = mn[3] == 'b' ? 7 : mn[3] == 'h' ? 15 : 31;
    emit_bitfield(mn[0] == 's' ? 0 : 2, &rd, &rn, 0, imms);
    return;
  }
  if (MN_IS("sbfm") || MN_IS("bfm") || MN_IS("ubfm")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]);
    emit_bitfield(mn[0] == 's' ? 0 : mn[0] == 'b' ? 1 : 2, &rd, &rn, parse_imm(ops[2]),
                  parse_imm(ops[3]));
    return;
  }
  if (MN_IS("ubfx") || MN_IS("sbfx") || MN_IS("bfxil")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]);
    int64_t lsb = parse_imm(ops[2]), w = parse_imm(ops[3]);
    emit_bitfield(mn[0] == 's' ? 0 : mn[0] == 'b' ? 1 : 2, &rd, &rn, lsb, lsb + w - 1);
    return;
  }
  if (MN_IS("bfi") || MN_IS("ubfiz") || MN_IS("sbfiz")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]);
    int64_t lsb = parse_imm(ops[2]), w = parse_imm(ops[3]);
    int width = reg_width(&rd);
    emit_bitfield(mn[0] == 's' ? 0 : mn[0] == 'b' ? 1 : 2, &rd, &rn, (width - lsb) % width, w - 1);
    return;
  }

  if (MN_IS("mul") || MN_IS("madd") || MN_IS("msub") || MN_IS("mneg")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]), rm = need_gpr(ops[2]);
    Reg ra = {.kind = rd.kind, .num = 31};
    if (n >= 4)
      ra = need_gpr(ops[3]);
    uint32_t o0 = MN_IS("msub") || MN_IS("mneg");
    emit32(sf_bit(&rd) | 0x1B000000 | (rm.num << 16) | (o0 << 15) | (ra.num << 10) |
           (rn.num << 5) | rd.num);
    return;
  }
  if (MN_IS("smulh") || MN_IS("umulh")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]), rm = need_gpr(ops[2]);
    emit32((mn[0] == 'u' ? 0x9BC07C00 : 0x9B407C00) | (rm.num << 16) | (rn.num << 5) | rd.num);
    return;
  }
  if (MN_IS("smull") || MN_IS("umull")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]), rm = need_gpr(ops[2]);
    emit32((mn[0] == 'u' ? 0x9BA07C00 : 0x9B207C00) | (rm.num << 16) | (rn.num << 5) | rd.num);
    return;
  }
  if (MN_IS("udiv") || MN_IS("sdiv")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]), rm = need_gpr(ops[2]);
    emit_dp2(mn[0] == 'u' ? 2 : 3, &rd, &rn, &rm);
    return;
  }
  if (MN_IS("lslv") || MN_IS("lsrv") || MN_IS("asrv") || MN_IS("rorv")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]), rm = need_gpr(ops[2]);
    emit_dp2(mn[1] == 's' ? (mn[2] == 'l' ? 8 : 9) : (mn[0] == 'a' ? 10 : 11), &rd, &rn, &rm);
    return;
  }
  if (MN_IS("clz") || MN_IS("cls") || MN_IS("rbit") || MN_IS("rev") ||
      MN_IS("rev16") || MN_IS("rev32")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]);
    uint32_t opc;
    if (MN_IS("rbit"))       opc = 0;
    else if (MN_IS("rev16")) opc = 1;
    else if (MN_IS("rev32")) opc = 2;
    else if (MN_IS("rev"))   opc = is64(&rd) ? 3 : 2;
    else if (MN_IS("clz"))   opc = 4;
    else                           opc = 5;
    emit32(sf_bit(&rd) | 0x5AC00000 | (opc << 10) | (rn.num << 5) | rd.num);
    return;
  }

  // 条件つき選択
  if (MN_IS("csel") || MN_IS("csinc") || MN_IS("csinv") || MN_IS("csneg")) {
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]), rm = need_gpr(ops[2]);
    uint32_t op = MN_IS("csinv") || MN_IS("csneg");
    uint32_t op2 = MN_IS("csinc") || MN_IS("csneg");
    emit_condsel(op, op2, &rd, &rn, &rm, need_cond(ops[3]));
    return;
  }
  if (MN_IS("cset") || MN_IS("csetm")) {
    Reg rd = need_gpr(ops[0]);
    Reg zr = {.kind = rd.kind, .num = 31};
    emit_condsel(mn[4] == 'm', mn[4] != 'm', &rd, &zr, &zr, need_cond(ops[1]) ^ 1);
    return;
  }
  if (MN_IS("ccmp") || MN_IS("ccmn")) {
    // ccmp Rn, Rm|#imm5, #nzcv, cond
    if (n != 4)
      asm_error("expected 4 operands");
    Reg rn = need_gpr(ops[0]);
    Reg rm;
    uint32_t insn = (is64(&rn) ? 0x80000000 : 0) | (MN_IS("ccmp") ? 0x7A400000 : 0x3A400000);
    if (parse_reg(ops[1], &rm)) {
      if (!is_gpr(&rm) || is64(&rm) != is64(&rn))
        asm_error("bad register: '%s'", ops[1]);
      insn |= rm.num << 16;
    } else {
      int64_t imm = parse_imm(ops[1]);
      if (imm < 0 || imm > 31)
        asm_error("immediate out of range");
      insn |= 0x800 | (uint32_t)imm << 16;
    }
    int64_t nzcv = parse_imm(ops[2]);
    if (nzcv < 0 || nzcv > 15)
      asm_error("bad nzcv");
    emit32(insn | (need_cond(ops[3]) << 12) | (rn.num << 5) | (uint32_t)nzcv);
    return;
  }
  if (MN_IS("cinc") || MN_IS("cinv") || MN_IS("cneg")) {
    // cinc = csinc（op 0、op2 1）、cinv = csinv（1、0）、cneg = csneg（1、1）
    Reg rd = need_gpr(ops[0]), rn = need_gpr(ops[1]);
    uint32_t op = strcmp(mn, "cinc") != 0;
    uint32_t op2 = strcmp(mn, "cinv") != 0;
    emit_condsel(op, op2, &rd, &rn, &rn, need_cond(ops[2]) ^ 1);
    return;
  }

  // 分岐
  if (MN_IS("b") && n == 1)  { enc_branch(FX_B26, 0x14000000, ops[0]); return; }
  if (MN_IS("bl") && n == 1) { enc_branch(FX_CALL26, 0x94000000, ops[0]); return; }
  if (!strncmp(mn, "b.", 2) && n == 1) {
    int c = parse_cond(mn + 2);
    if (c < 0)
      asm_error("unknown condition '%s'", mn + 2);
    enc_branch(FX_COND19, 0x54000000 | c, ops[0]);
    return;
  }
  if (MN_IS("cbz") || MN_IS("cbnz")) {
    Reg rt = need_gpr(ops[0]);
    enc_branch(FX_COND19, sf_bit(&rt) | (mn[2] == 'n' ? 0x35000000 : 0x34000000) | rt.num, ops[1]);
    return;
  }
  if (MN_IS("tbz") || MN_IS("tbnz")) {
    Reg rt = need_gpr(ops[0]);
    int64_t bit = parse_imm(ops[1]);
    if (bit < 0 || bit >= reg_width(&rt))
      asm_error("bit number out of range");
    enc_branch(FX_TST14,
               ((uint32_t)(bit >> 5) << 31) | (mn[2] == 'n' ? 0x37000000 : 0x36000000) |
                 ((uint32_t)(bit & 31) << 19) | rt.num,
               ops[2]);
    return;
  }
  if (MN_IS("br") || MN_IS("blr") || MN_IS("ret")) {
    Reg rn = {.kind = RK_X, .num = 30};
    if (n >= 1)
      rn = need_gpr(ops[0]);
    uint32_t base = MN_IS("br") ? 0xD61F0000 : MN_IS("blr") ? 0xD63F0000 : 0xD65F0000;
    emit32(base | (rn.num << 5));
    return;
  }
  if (MN_IS("adr") || MN_IS("adrp")) {
    Reg rd = need_gpr(ops[0]);
    RelExpr re = parse_relexpr(ops[1]);
    if (!re.e.sym)
      asm_error("adr/adrp needs a symbol");
    if (mn[3] == 'p') {
      if (!re.mod[0] || !strcmp(re.mod, "pg_hi21"))
        add_fixup(FX_ADRP, 0, true, re.e.sym, re.e.val);
      else if (!strcmp(re.mod, "got"))
        add_fixup(FX_OTHER, R_ADR_GOT_PAGE, false, re.e.sym, re.e.val);
      else if (!strcmp(re.mod, "gottprel"))
        add_fixup(FX_OTHER, R_TLSIE_ADR_GOTTPREL_PAGE21, false, re.e.sym, re.e.val);
      else if (!strcmp(re.mod, "tlsdesc"))
        add_fixup(FX_OTHER, R_TLSDESC_ADR_PAGE21, false, re.e.sym, re.e.val);
      else
        asm_error("unsupported relocation for adrp: ':%s:'", re.mod);
      emit32(0x90000000 | rd.num);
      return;
    }
    if (re.mod[0])
      asm_error("unsupported relocation for adr");
    add_fixup(FX_ADR21, 0, true, re.e.sym, re.e.val);
    emit32(0x10000000 | rd.num);
    return;
  }

  // ロード・ストア
  if (assemble_ldst(mn, ops, n))
    return;
  if (MN_IS("ldp"))   { enc_ldstp(true, false, ops, n); return; }
  if (MN_IS("stp"))   { enc_ldstp(false, false, ops, n); return; }
  if (MN_IS("ldpsw")) { enc_ldstp(true, true, ops, n); return; }
  if (!strncmp(mn, "ldxr", 4) || !strncmp(mn, "ldaxr", 5) || !strncmp(mn, "stxr", 4) ||
      !strncmp(mn, "stlxr", 5) || !strncmp(mn, "ldar", 4) || !strncmp(mn, "stlr", 4)) {
    enc_ldstx(mn, ops, n);
    return;
  }

  // システム
  if (MN_IS("nop"))   { emit32(0xD503201F); return; }
  if (MN_IS("yield")) { emit32(0xD503203F); return; }
  if (MN_IS("wfe"))   { emit32(0xD503205F); return; }
  if (MN_IS("wfi"))   { emit32(0xD503207F); return; }
  if (MN_IS("sev"))   { emit32(0xD503209F); return; }
  if (MN_IS("sevl"))  { emit32(0xD50320BF); return; }
  if (MN_IS("eret"))  { emit32(0xD69F03E0); return; }
  if (MN_IS("clrex")) { emit32(0xD503305F | ((n ? parse_imm(ops[0]) : 15) << 8)); return; }
  if (MN_IS("dmb"))   { emit32(0xD50330BF | (parse_barrier(n ? ops[0] : "") << 8)); return; }
  if (MN_IS("dsb"))   { emit32(0xD503309F | (parse_barrier(n ? ops[0] : "") << 8)); return; }
  if (MN_IS("isb"))   { emit32(0xD50330DF | (parse_barrier(n ? ops[0] : "") << 8)); return; }
  if (MN_IS("svc") || MN_IS("hvc") || MN_IS("smc") || MN_IS("brk") ||
      MN_IS("hlt")) {
    int64_t imm = n ? parse_imm(ops[0]) : 0;
    uint32_t base = MN_IS("svc") ? 0xD4000001 : MN_IS("hvc") ? 0xD4000002
                  : MN_IS("smc") ? 0xD4000003 : MN_IS("brk") ? 0xD4200000 : 0xD4400000;
    emit32(base | ((uint32_t)(imm & 0xFFFF) << 5));
    return;
  }
  if (MN_IS("tlbi") || MN_IS("ic") || MN_IS("dc") || MN_IS("at")) {
    // SYS #op1, Cn, Cm, #op2, Xt の別名（出典: ARM DDI 0487 C5.3 の TLBI・IC・DC・AT）
    const SysOp *op = find_sysop(mn, ops[0]);
    if (!op)
      asm_error("unknown operation '%s %s'", mn, n ? ops[0] : "");
    int rt = 31;
    if (op->has_rt) {
      if (n < 2)
        asm_error("'%s %s' needs a register", mn, op->name);
      rt = need_gpr(ops[1]).num;
    } else if (n >= 2) {
      asm_error("'%s %s' takes no register", mn, op->name);
    }
    emit32(0xD5080000 | (op->op1 << 16) | (op->crn << 12) | (op->crm << 8) | (op->op2 << 5) | rt);
    return;
  }
  if (MN_IS("mrs")) {
    Reg rt = need_gpr(ops[0]);
    emit32(0xD5200000 | parse_sysreg(ops[1]) | rt.num); // L = 1
    return;
  }
  if (MN_IS("msr")) {
    const char *name = skip_space(ops[0]);
    if (!strcasecmp(name, "daifset") || !strcasecmp(name, "daifclr") ||
        !strcasecmp(name, "spsel")) {
      int64_t imm = parse_imm(ops[1]);
      uint32_t op1op2 = !strcasecmp(name, "daifset") ? 0x30C0 /* op1=3 op2=6 */
                      : !strcasecmp(name, "daifclr") ? 0x30E0 : 0x00A0;
      emit32(0xD500401F | (op1op2 & 0x7000) << 4 | (op1op2 & 0xE0) | ((uint32_t)imm << 8));
      return;
    }
    Reg rt = need_gpr(ops[1]);
    emit32(0xD5000000 | parse_sysreg(ops[0]) | rt.num); // L = 0
    return;
  }

  // 浮動小数点
  if (MN_IS("fmul"))   { enc_fp2(0, ops, n); return; }
  if (MN_IS("fdiv"))   { enc_fp2(1, ops, n); return; }
  if (MN_IS("fadd"))   { enc_fp2(2, ops, n); return; }
  if (MN_IS("fsub"))   { enc_fp2(3, ops, n); return; }
  if (MN_IS("fmax"))   { enc_fp2(4, ops, n); return; }
  if (MN_IS("fmin"))   { enc_fp2(5, ops, n); return; }
  if (MN_IS("fmaxnm")) { enc_fp2(6, ops, n); return; }
  if (MN_IS("fminnm")) { enc_fp2(7, ops, n); return; }
  if (MN_IS("fnmul"))  { enc_fp2(8, ops, n); return; }
  if (MN_IS("fabs"))   { enc_fp1(1, ops, n); return; }
  if (MN_IS("fneg"))   { enc_fp1(2, ops, n); return; }
  if (MN_IS("fsqrt"))  { enc_fp1(3, ops, n); return; }
  if (MN_IS("frintn")) { enc_fp1(8, ops, n); return; }
  if (MN_IS("frintp")) { enc_fp1(9, ops, n); return; }
  if (MN_IS("frintm")) { enc_fp1(10, ops, n); return; }
  if (MN_IS("frintz")) { enc_fp1(11, ops, n); return; }
  if (MN_IS("frinta")) { enc_fp1(12, ops, n); return; }
  if (MN_IS("frintx")) { enc_fp1(14, ops, n); return; }
  if (MN_IS("frinti")) { enc_fp1(15, ops, n); return; }
  if (MN_IS("fmadd") || MN_IS("fmsub") || MN_IS("fnmadd") || MN_IS("fnmsub")) {
    Reg rd = need_fpr(ops[0]), rn = need_fpr(ops[1]), rm = need_fpr(ops[2]), ra = need_fpr(ops[3]);
    uint32_t o1 = mn[1] == 'n', o0 = mn[strlen(mn) - 3] == 's';
    emit32(0x1F000000 | (fp_type(&rd) << 22) | (o1 << 21) | (rm.num << 16) | (o0 << 15) |
           (ra.num << 10) | (rn.num << 5) | rd.num);
    return;
  }
  if (MN_IS("fcmp"))   { enc_fcmp(false, ops, n); return; }
  if (MN_IS("fcmpe"))  { enc_fcmp(true, ops, n); return; }
  if (MN_IS("fcvt"))   { enc_fcvt(ops, n); return; }
  if (MN_IS("fmov"))   { enc_fmov(ops, n); return; }
  if (MN_IS("scvtf"))  { enc_fcvt_int(0, 2, false, ops, n); return; }
  if (MN_IS("ucvtf"))  { enc_fcvt_int(0, 3, false, ops, n); return; }
  if (MN_IS("fcvtzs")) { enc_fcvt_int(3, 0, true, ops, n); return; }
  if (MN_IS("fcvtzu")) { enc_fcvt_int(3, 1, true, ops, n); return; }
  if (MN_IS("fcvtns")) { enc_fcvt_int(0, 0, true, ops, n); return; }
  if (MN_IS("fcvtnu")) { enc_fcvt_int(0, 1, true, ops, n); return; }
  if (MN_IS("fcvtps")) { enc_fcvt_int(1, 0, true, ops, n); return; }
  if (MN_IS("fcvtpu")) { enc_fcvt_int(1, 1, true, ops, n); return; }
  if (MN_IS("fcvtms")) { enc_fcvt_int(2, 0, true, ops, n); return; }
  if (MN_IS("fcvtmu")) { enc_fcvt_int(2, 1, true, ops, n); return; }
  if (MN_IS("fcvtas")) { enc_fcvt_int(0, 4, true, ops, n); return; }
  if (MN_IS("fcvtau")) { enc_fcvt_int(0, 5, true, ops, n); return; }
  if (MN_IS("movi"))   { enc_movi(ops, n); return; }
  if (MN_IS("ins") || MN_IS("umov")) { enc_mov(ops, n); return; }
  asm_error("unknown instruction '%s'", mn);
}

//
// アセンブラ指示
//

TLS static AsmSection *prev_sect;

static void switch_section(AsmSection *sec) {
  if (sec != cur_sect)
    prev_sect = cur_sect;
  cur_sect = sec;
}

// "a" "b" などをつないだセクション名（.text."foo" など）を読む
static char *read_section_name(const char **rest, const char *p) {
  p = skip_space(p);
  char buf[1024];
  int len = 0;
  while (*p && *p != ',' && *p != ' ' && *p != '\t') {
    if (*p == '"') {
      p++;
      while (*p && *p != '"' && len < (int)sizeof(buf) - 1)
        buf[len++] = *p++;
      if (*p == '"')
        p++;
      continue;
    }
    if (len < (int)sizeof(buf) - 1)
      buf[len++] = *p;
    p++;
  }
  buf[len] = '\0';
  if (!len)
    asm_error("missing section name");
  *rest = p;
  return tmp_strdup(buf);
}

static void dir_section(const char *args) {
  const char *p;
  char *name = read_section_name(&p, args);
  bool is_new = !find_section(name);
  AsmSection *sec = get_section(name);
  char *ops[MAX_OPS];
  char *rest = tmp_strdup(skip_space(p));
  int n = 0;
  if (*rest == ',')
    n = split_operands(rest + 1, ops);
  if (n >= 1) {
    const char *f = skip_space(ops[0]);
    if (*f != '"')
      asm_error("bad section flags: '%s'", f);
    uint64_t flags = 0;
    for (f++; *f && *f != '"'; f++) {
      switch (*f) {
      case 'a': flags |= ASM_SHF_ALLOC; break;
      case 'w': flags |= ASM_SHF_WRITE; break;
      case 'x': flags |= ASM_SHF_EXECINSTR; break;
      case 'M': flags |= ASM_SHF_MERGE; break;
      case 'S': flags |= ASM_SHF_STRINGS; break;
      case 'T': flags |= ASM_SHF_TLS; break;
      case 'G': flags |= ASM_SHF_GROUP; break;
      default:  asm_error("unknown section flag '%c'", *f);
      }
    }
    if (is_new || flags)
      sec->flags = flags;
    if (is_new && (flags & ASM_SHF_EXECINSTR))
      sec->align = MAX(sec->align, 4);
  }
  if (n >= 2) {
    const char *t = skip_space(ops[1]);
    if (*t == '@' || *t == '%')
      t++;
    if (!strcmp(t, "progbits"))
      sec->type = ASM_SHT_PROGBITS;
    else if (!strcmp(t, "nobits"))
      sec->type = ASM_SHT_NOBITS;
    else if (!strcmp(t, "note"))
      sec->type = ASM_SHT_NOTE;
    else if (!strcmp(t, "init_array"))
      sec->type = ASM_SHT_INIT_ARRAY;
    else if (!strcmp(t, "fini_array"))
      sec->type = ASM_SHT_FINI_ARRAY;
    else
      asm_error("unknown section type '%s'", t);
  }
  switch_section(sec);
}

static void for_each_symbol(const char *args, void (*fn)(AsmSymbol *)) {
  char *buf = tmp_strdup(args);
  char *ops[MAX_OPS];
  int n = split_operands(buf, ops);
  for (int i = 0; i < n; i++) {
    const char *rest;
    char *name = read_symbol(&rest, ops[i]);
    if (!name)
      asm_error("expected a symbol name: '%s'", ops[i]);
    fn(get_symbol(name));
  }
}

static void set_global(AsmSymbol *s) {
  s->bind = ASM_STB_GLOBAL;
  s->bind_set = true;
}
static void set_weak(AsmSymbol *s) {
  s->bind = ASM_STB_WEAK;
  s->bind_set = true;
}
static void set_local(AsmSymbol *s) {
  s->bind = ASM_STB_LOCAL;
  s->bind_set = true;
}
static void set_hidden(AsmSymbol *s) {
  s->vis = ASM_STV_HIDDEN;
}
static void set_variant_pcs(AsmSymbol *s) {
  s->variant_pcs = true;
}
static void set_internal(AsmSymbol *s) {
  s->vis = ASM_STV_INTERNAL;
}
static void set_protected(AsmSymbol *s) {
  s->vis = ASM_STV_PROTECTED;
}

// "..." の C 風のエスケープを解釈してバイト列にする
static int64_t parse_string(const char **rest, const char *p, char *out) {
  p = skip_space(p);
  if (*p != '"')
    asm_error("expected a string");
  p++;
  int64_t len = 0;
  while (*p && *p != '"') {
    if (*p != '\\') {
      out[len++] = *p++;
      continue;
    }
    p++;
    switch (*p) {
    case 'n': out[len++] = '\n'; p++; break;
    case 't': out[len++] = '\t'; p++; break;
    case 'r': out[len++] = '\r'; p++; break;
    case 'b': out[len++] = '\b'; p++; break;
    case 'f': out[len++] = '\f'; p++; break;
    case 'x': {
      p++;
      int v = 0;
      while (Isxdigit(*p))
        v = v * 16 + (Isdigit(*p) ? *p - '0' : (Tolower(*p) - 'a' + 10)), p++;
      out[len++] = v;
      break;
    }
    default:
      if (*p >= '0' && *p <= '7') {
        int v = 0;
        for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++)
          v = v * 8 + (*p++ - '0');
        out[len++] = v;
      } else {
        out[len++] = *p++;
      }
    }
  }
  if (*p != '"')
    asm_error("unterminated string");
  *rest = p + 1;
  return len;
}

static void dir_data(int size, const char *args) {
  char *buf = tmp_strdup(args);
  char *ops[MAX_OPS * 32];
  // 個数が多いことがあるので、自前で区切る
  int n = 0;
  int depth = 0;
  char *start = buf;
  for (char *p = buf;; p++) {
    if (*p == '(')
      depth++;
    else if (*p == ')')
      depth--;
    if (*p == '\0' || (*p == ',' && depth == 0)) {
      char c = *p;
      *p = '\0';
      if (*skip_space(start)) {
        if (n == (int)(sizeof(ops) / sizeof(*ops))) {
          for (int i = 0; i < n; i++) {
            Expr e = parse_expr_all(ops[i]);
            if (e.sym)
              asm_error("too many symbolic values in one directive");
            emit_le(e.val, size);
          }
          n = 0;
        }
        ops[n++] = start;
      }
      if (c == '\0')
        break;
      start = p + 1;
    }
  }
  for (int i = 0; i < n; i++) {
    Expr e = parse_expr_all(ops[i]);
    if (e.sym) {
      FixKind k = size == 8 ? FX_ABS64 : size == 4 ? FX_ABS32 : size == 2 ? FX_ABS16 : FX_OTHER;
      if (k == FX_OTHER)
        asm_error("symbolic value of this size is not supported");
      add_fixup(k, 0, true, e.sym, e.val);
      emit_le(0, size);
      continue;
    }
    emit_le(e.val, size);
  }
}

static void assemble_chunk(const char *text);
static void assemble_insn(char *mn, char **ops, int n);
static bool in_macro_def(void);

// 型付きの記録のオペランドを Reg に（parse_reg と同じ結果）。レジスタでなければ false
static bool ao_reg(const AOp *o, Reg *r) {
  if (o->kind != AO_REG)
    return false;
  *r = (Reg){0};
  switch (o->c) {
  case 'x': r->kind = o->r == 32 ? RK_SP : RK_X; break;
  case 'w': r->kind = o->r == 32 ? RK_WSP : RK_W; break;
  case 's': r->kind = RK_S; break;
  case 'd': r->kind = RK_D; break;
  case 'q': r->kind = RK_Q; break;
  default:  return false;
  }
  r->num = o->r == 32 ? 31 : o->r;
  return true;
}

// AO_MEM を Mem に（parse_mem と同じ結果。後置は AM_POST まで決める）
static bool ao_mem(const AOp *o, Mem *m) {
  if (o->kind != AO_MEM)
    return false;
  *m = (Mem){.mode = AM_OFFSET};
  m->base = (Reg){.kind = o->r == 32 ? RK_SP : RK_X, .num = o->r == 32 ? 31 : o->r};
  switch (o->mode) {
  case AOM_OFF:
  case AOM_OFF0: m->imm = o->imm; return true;
  case AOM_PRE:  m->mode = AM_PRE, m->imm = o->imm; return true;
  case AOM_POST: m->mode = AM_POST, m->imm = o->imm; return true;
  case AOM_IDX:
  case AOM_IDXLSL:
    m->mode = AM_REG;
    m->index = (Reg){.kind = RK_X, .num = o->idx};
    m->shift = (Shift){.kind = SH_NONE};
    if (o->mode == AOM_IDXLSL)
      m->shift = (Shift){.kind = SH_LSL, .amount = o->sh, .has_amount = true};
    return true;
  case AOM_GOTLO:
    m->mode = AM_RELOC;
    strcpy(m->rel.mod, "got_lo12");
    m->rel.e = (Expr){.sym = (char *)o->s};
    return true;
  }
  return false;
}

// 分岐の行き先（ラベル・シンボル）
static const char *ao_target(const AOp *o) {
  return o->kind == AO_TEXT || (o->kind == AO_SYM && o->mode == AS_PLAIN) ? o->s : NULL;
}

// よく出る命令を、文字列にせず記録から直接組み立てる（機械語を直接作る形の段階 2。2026-11-16）。
// 結果は文字列から組み立てたときと同じ（cmp_o.sh で .o を比べる）。扱わなければ false（文字列にして読む）
static bool frag_insn(const IrRec *r) {
  const char *mn = r->mn;
  int n = r->nop;
  const AOp *op = r->op;
  Reg rd, rn, rm;
  Mem m;
  switch (mn[0]) {
  case 'm':
    if (MN_IS("mov") && n == 2 && ao_reg(&op[0], &rd) && is_gpr_or_sp(&rd)) {
      if (op[1].kind == AO_IMM) {
        enc_mov_imm(&rd, op[1].imm);
        return true;
      }
      if (!ao_reg(&op[1], &rn) || !is_gpr_or_sp(&rn))
        return false;
      if (rd.kind == RK_SP || rn.kind == RK_SP || rd.kind == RK_WSP || rn.kind == RK_WSP)
        emit32(sf_bit(&rd) | 0x11000000 | (rn.num << 5) | rd.num);
      else
        emit32(sf_bit(&rd) | 0x2A000000 | (rn.num << 16) | (31 << 5) | rd.num);
      return true;
    }
    if ((MN_IS("movz") || MN_IS("movk") || MN_IS("movn")) && (n == 2 || n == 3) &&
        ao_reg(&op[0], &rd) && is_gpr(&rd) && op[1].kind == AO_IMM) {
      int hw = 0;
      if (n == 3) {
        if (op[2].kind != AO_SHIFT || strcmp(op[2].s, "lsl") || op[2].sh % 16)
          return false;
        hw = op[2].sh / 16;
      }
      int64_t imm = op[1].imm;
      if (imm < 0 || imm > 0xFFFF || hw > (is64(&rd) ? 3 : 1))
        return false;
      int opc = mn[3] == 'z' ? 2 : mn[3] == 'k' ? 3 : 0;
      emit32(sf_bit(&rd) | (opc << 29) | 0x12800000 | (hw << 21) | ((uint32_t)imm << 5) | rd.num);
      return true;
    }
    return false;
  case 'l':
  case 's':
    if (n == 2 && ao_reg(&op[0], &rd) && ao_mem(&op[1], &m)) {
      for (size_t i = 0; i < sizeof(ldst) / sizeof(*ldst); i++) {
        if (mn[0] == ldst[i].name[0] && !strcmp(mn, ldst[i].name)) {
          LdSt l = ldst_info(ldst[i].suffix, &rd, ldst[i].load);
          enc_ldst_mem(&l, ldst[i].unscaled, &rd, &m);
          return true;
        }
      }
    }
    if ((MN_IS("ldp") || MN_IS("stp")) && n == 3 && ao_reg(&op[0], &rd) && ao_reg(&op[1], &rn) &&
        rd.kind == rn.kind && ao_mem(&op[2], &m) && m.mode != AM_REG && m.mode != AM_RELOC) {
      uint32_t idx = m.mode == AM_PRE ? 3 : m.mode == AM_POST ? 1 : 2;
      enc_ldstp_mem(mn[0] == 'l', false, &rd, &rn, &m, idx);
      return true;
    }
    if ((MN_IS("sub") || MN_IS("subs")) && (n == 3 || n == 4))
      goto addsub;
    if (MN_IS("sxtb") || MN_IS("sxth") || MN_IS("sxtw"))
      goto ext;
    return false;
  case 'a':
    if ((MN_IS("add") || MN_IS("adds")) && (n == 3 || n == 4))
      goto addsub;
    if (MN_IS("adrp") && n == 2 && ao_reg(&op[0], &rd) && is_gpr(&rd) && op[1].kind == AO_SYM) {
      if (op[1].mode == AS_PLAIN)
        add_fixup(FX_ADRP, 0, true, op[1].s, 0);
      else if (op[1].mode == AS_GOT)
        add_fixup(FX_OTHER, R_ADR_GOT_PAGE, false, op[1].s, 0);
      else
        return false;
      emit32(0x90000000 | rd.num);
      return true;
    }
    return false;
  case 'c':
    // 条件の選択と ccmp（2026-11-27）。AO_COND の値は ir.inc の cc_name の番号（下の表で命令の条件の番号に）
    if (n >= 2 && op[n - 1].kind == AO_COND && op[n - 1].imm >= 0 && op[n - 1].imm < 15) {
      static const uint8_t code[15] = {0, 1, 11, 13, 12, 10, 3, 9, 8, 2, 4, 9, 12, 10, 5};
      uint32_t cond = code[op[n - 1].imm];
      if ((MN_IS("cset") || MN_IS("csetm")) && n == 2 && ao_reg(&op[0], &rd) && is_gpr(&rd)) {
        Reg zr = {.kind = rd.kind, .num = 31};
        emit_condsel(mn[4] == 'm', mn[4] != 'm', &rd, &zr, &zr, cond ^ 1);
        return true;
      }
      if (MN_IS("csel") && n == 4 && ao_reg(&op[0], &rd) && is_gpr(&rd) && ao_reg(&op[1], &rn) &&
          is_gpr(&rn) && ao_reg(&op[2], &rm) && is_gpr(&rm)) {
        emit_condsel(0, 0, &rd, &rn, &rm, cond);
        return true;
      }
      if (MN_IS("ccmp") && n == 4 && ao_reg(&op[0], &rn) && is_gpr(&rn) && op[2].kind == AO_IMM &&
          op[2].imm >= 0 && op[2].imm < 16) {
        uint32_t insn = (is64(&rn) ? 0x80000000 : 0) | 0x7A400000;
        if (op[1].kind == AO_IMM && op[1].imm >= 0 && op[1].imm < 32)
          insn |= 0x800 | (uint32_t)op[1].imm << 16;
        else if (ao_reg(&op[1], &rm) && is_gpr(&rm) && is64(&rm) == is64(&rn))
          insn |= rm.num << 16;
        else
          return false;
        emit32(insn | (cond << 12) | (rn.num << 5) | (uint32_t)op[2].imm);
        return true;
      }
      return false;
    }
    if ((MN_IS("cmp") || MN_IS("cmn")) && n == 2 && ao_reg(&op[0], &rn) && is_gpr_or_sp(&rn)) {
      rd = (Reg){.kind = is64(&rn) || rn.kind == RK_SP ? RK_X : RK_W, .num = 31};
      bool sub = mn[2] == 'p';
      if (op[1].kind == AO_IMM) {
        check_imm_zr(NULL, &rn, true);
        uint32_t base = sf_bit(&rd) | (sub << 30) | (1u << 29) | 0x11000000 | (rn.num << 5) | rd.num;
        enc_addsub_immv(base, sub, op[1].imm, 0);
        return true;
      }
      if (!ao_reg(&op[1], &rm) || !is_gpr(&rm) || rn.kind == RK_SP || rn.kind == RK_WSP)
        return false;
      emit32(sf_bit(&rd) | (sub << 30) | (1u << 29) | 0x0B000000 | (rm.num << 16) | (rn.num << 5) |
             rd.num);
      return true;
    }
    if ((MN_IS("cbz") || MN_IS("cbnz")) && n == 2 && ao_reg(&op[0], &rd) && is_gpr(&rd) &&
        ao_target(&op[1])) {
      add_fixup(FX_COND19, 0, true, ao_target(&op[1]), 0);
      emit32(sf_bit(&rd) | (mn[2] == 'n' ? 0x35000000 : 0x34000000) | rd.num);
      return true;
    }
    return false;
  case 't':     // tbz・tbnz（2026-11-27）
    if ((MN_IS("tbz") || MN_IS("tbnz")) && n == 3 && ao_reg(&op[0], &rd) && is_gpr(&rd) &&
        op[1].kind == AO_IMM && op[1].imm >= 0 && op[1].imm < reg_width(&rd) && ao_target(&op[2])) {
      add_fixup(FX_TST14, 0, true, ao_target(&op[2]), 0);
      emit32(((uint32_t)(op[1].imm >> 5) << 31) | (mn[2] == 'n' ? 0x37000000 : 0x36000000) |
             ((uint32_t)(op[1].imm & 31) << 19) | rd.num);
      return true;
    }
    return false;
  case 'b':
    if (n != 1 || !ao_target(&op[0]))
      return false;
    if (MN_IS("bl")) {
      add_fixup(FX_CALL26, 0, true, ao_target(&op[0]), 0);
      emit32(0x94000000);
      return true;
    }
    if (MN_IS("b")) {
      add_fixup(FX_B26, 0, true, ao_target(&op[0]), 0);
      emit32(0x14000000);
      return true;
    }
    if (mn[1] == '.') {
      int c = parse_cond(mn + 2);
      if (c < 0)
        return false;
      add_fixup(FX_COND19, 0, true, ao_target(&op[0]), 0);
      emit32(0x54000000 | c);
      return true;
    }
    return false;
  case 'u':
    if (MN_IS("uxtb") || MN_IS("uxth"))
      goto ext;
    return false;
  case 'r':
    if (MN_IS("ret") && n == 0) {
      emit32(0xD65F0000 | (30 << 5));
      return true;
    }
    return false;
  }
  return false;

ext:
  // sxtb・sxth・sxtw・uxtb・uxth
  if (n != 2 || !ao_reg(&op[0], &rd) || !is_gpr(&rd) || !ao_reg(&op[1], &rn) || !is_gpr(&rn))
    return false;
  emit_bitfield(mn[0] == 's' ? 0 : 2, &rd, &rn, 0, mn[3] == 'b' ? 7 : mn[3] == 'h' ? 15 : 31);
  return true;

addsub:
  // add・sub（即値、:lo12:、シフトのないレジスタ）
  if (!ao_reg(&op[0], &rd) || !is_gpr_or_sp(&rd) || !ao_reg(&op[1], &rn) || !is_gpr_or_sp(&rn))
    return false;
  {
    bool sub = mn[0] == 's';
    bool setflags = mn[3] == 's';
    if (n == 4) {
      // 即値・:tprel_hi12: の lsl #0・#12 だけ（enc_addsub_imm と同じ）
      if (op[3].kind != AO_SHIFT || strcmp(op[3].s, "lsl") || (op[3].sh != 0 && op[3].sh != 12))
        return false;
      if (op[2].kind == AO_SYM && op[2].mode == AS_TPHI && !sub) {
        add_fixup(FX_OTHER, R_TLSLE_ADD_TPREL_HI12, false, op[2].s, 0);
        emit32(sf_bit(&rd) | (setflags << 29) | 0x11000000 | ((uint32_t)(op[3].sh == 12) << 22) |
               (rn.num << 5) | rd.num);
        return true;
      }
      if (op[2].kind != AO_IMM)
        return false;
      check_imm_zr(&rd, &rn, setflags);
      uint32_t base = sf_bit(&rd) | (sub << 30) | (setflags << 29) | 0x11000000 | (rn.num << 5) | rd.num;
      enc_addsub_immv(base, sub, op[2].imm, op[3].sh == 12);
      return true;
    }
    if (op[2].kind == AO_IMM) {
      check_imm_zr(&rd, &rn, setflags);
      uint32_t base = sf_bit(&rd) | (sub << 30) | (setflags << 29) | 0x11000000 | (rn.num << 5) | rd.num;
      enc_addsub_immv(base, sub, op[2].imm, 0);
      return true;
    }
    if (op[2].kind == AO_SYM && op[2].mode == AS_TPLO && !sub) {
      add_fixup(FX_OTHER, R_TLSLE_ADD_TPREL_LO12_NC, false, op[2].s, 0);
      emit32(sf_bit(&rd) | (setflags << 29) | 0x11000000 | (rn.num << 5) | rd.num);
      return true;
    }
    if (op[2].kind == AO_SYM && op[2].mode == AS_LO12 && !sub) {
      add_fixup(FX_OTHER, R_ADD_ABS_LO12_NC, true, op[2].s, 0);
      emit32(sf_bit(&rd) | (setflags << 29) | 0x11000000 | (rn.num << 5) | rd.num);
      return true;
    }
    if (!ao_reg(&op[2], &rm) || !is_gpr(&rm))
      return false;
    if (rd.kind == RK_SP || rd.kind == RK_WSP || rn.kind == RK_SP || rn.kind == RK_WSP) {
      uint32_t option = is64(&rd) ? 3 : 2;
      emit32(sf_bit(&rd) | (sub << 30) | (setflags << 29) | 0x0B200000 | (rm.num << 16) |
             (option << 13) | (rn.num << 5) | rd.num);
      return true;
    }
    emit32(sf_bit(&rd) | (sub << 30) | (setflags << 29) | 0x0B000000 | (rm.num << 16) |
           (rn.num << 5) | rd.num);
    return true;
  }
}

// かけら（.slimcc_frag 番号。cc/ir.inc の中間表現の関数の記録）を組み立てる（2026-11-16）。型付きの記録は
// オペランドごとに文字列にして assemble_insn に直接渡す（行の切り出し・オペランドの区切りを通らない）。
// 文字列の行（ラベル・アトミックなど）は今までどおり文として読む
static void assemble_frag(int id) {
  const SlimccFrag *f = slimcc_frag_get(id);
  if (!f)
    asm_error("bad .slimcc_frag %d", id);
  for (int i = 0; i < f->n; i++) {
    const IrRec *r = &f->recs[i];
    if (r->kind == IRR_LINE) {
      // ラベルだけの行（.L.ir.1.2: など）は、文として読まずに定義する
      int len = r->len;
      if (len > 1 && r->s[len - 1] == ':' && !in_macro_def() && !Isdigit(r->s[0]) && is_sym_char(r->s[0])) {
        int k = 0;
        while (k < len - 1 && is_sym_char(r->s[k]))
          k++;
        if (k == len - 1) {
          define_label(tmp_strndup(r->s, k));
          continue;
        }
      }
      assemble_chunk(tmp_strndup(r->s, r->len));
      continue;
    }
    if (frag_insn(r))
      continue;
    char buf[1024], mn[16];
    char *ops[4];
    size_t ml = strlen(r->mn);
    memcpy(mn, r->mn, ml + 1);
    int pos = 0;
    for (int k = 0; k < r->nop; k++) {
      ops[k] = buf + pos;
      pos += slimcc_ao_print(buf + pos, &r->op[k]);
      buf[pos++] = '\0';
    }
    assemble_insn(mn, ops, r->nop);
  }
}

// ディレクティブの名前の判定（2 文字目が違えば strcmp を呼ばない。名前はいつも「.」で始まる。2026-11-16）
#define DIR_IS(lit) (name[1] == (lit)[1] && !strcmp(name, lit))

static bool directive(const char *name, const char *args) {
  char *ops[MAX_OPS];
  char *buf = tmp_strdup(args);

  if (DIR_IS(".slimcc_frag")) {
    assemble_frag(atoi(args));
    return true;
  }

  if (DIR_IS(".text") || DIR_IS(".data") || DIR_IS(".bss")) {
    switch_section(get_section(name));
    return true;
  }
  if (DIR_IS(".section") || DIR_IS(".pushsection")) {
    dir_section(args);
    return true;
  }
  if (DIR_IS(".previous") || DIR_IS(".popsection")) {
    if (prev_sect)
      switch_section(prev_sect);
    return true;
  }
  if (DIR_IS(".globl") || DIR_IS(".global")) { for_each_symbol(args, set_global); return true; }
  if (DIR_IS(".weak"))      { for_each_symbol(args, set_weak); return true; }
  if (DIR_IS(".local"))     { for_each_symbol(args, set_local); return true; }
  if (DIR_IS(".hidden"))    { for_each_symbol(args, set_hidden); return true; }
  if (DIR_IS(".variant_pcs")) { for_each_symbol(args, set_variant_pcs); return true; }
  if (DIR_IS(".internal"))  { for_each_symbol(args, set_internal); return true; }
  if (DIR_IS(".protected")) { for_each_symbol(args, set_protected); return true; }
  if (DIR_IS(".type")) {
    int n = split_operands(buf, ops);
    const char *rest;
    char *sname = n ? read_symbol(&rest, ops[0]) : NULL;
    if (n != 2 || !sname)
      asm_error("bad .type");
    const char *t = skip_space(ops[1]);
    if (*t == '@' || *t == '%')
      t++;
    AsmSymbol *s = get_symbol(sname);
    if (!strcmp(t, "function"))
      s->type = ASM_STT_FUNC;
    else if (!strcmp(t, "object"))
      s->type = ASM_STT_OBJECT;
    else if (!strcmp(t, "tls_object"))
      s->type = ASM_STT_TLS;
    else if (!strcmp(t, "notype"))
      s->type = ASM_STT_NOTYPE;
    else
      asm_error("unknown symbol type '%s'", t);
    return true;
  }
  if (DIR_IS(".size")) {
    int n = split_operands(buf, ops);
    const char *rest;
    char *sname = n ? read_symbol(&rest, ops[0]) : NULL;
    if (n != 2 || !sname)
      asm_error("bad .size");
    get_symbol(sname)->size = expr_const(parse_expr_all(ops[1]));
    return true;
  }
  if (DIR_IS(".set") || DIR_IS(".equ")) {
    int n = split_operands(buf, ops);
    const char *rest;
    char *sname = n ? read_symbol(&rest, ops[0]) : NULL;
    if (n != 2 || !sname)
      asm_error("bad .set");
    AsmSymbol *s = get_symbol(sname);
    Expr e = parse_expr_all(ops[1]);
    if (e.sym) {
      if (e.val)
        asm_error(".set with an offset is not supported");
      s->equ = strdup(e.sym);
      get_symbol(e.sym)->referenced = true;
    } else {
      s->defined = true;
      s->sect = NULL;
      s->value = e.val;
    }
    return true;
  }
  if (DIR_IS(".comm")) {
    int n = split_operands(buf, ops);
    const char *rest;
    char *sname = n ? read_symbol(&rest, ops[0]) : NULL;
    if (n < 2 || !sname)
      asm_error("bad .comm");
    AsmSymbol *s = get_symbol(sname);
    int64_t size = parse_imm(ops[1]);
    int64_t align = n >= 3 ? parse_imm(ops[2]) : 1;
    if (s->bind_set && s->bind == ASM_STB_LOCAL) {
      // .local と .comm の組み合わせは .bss に置く（GNU as と同じ）
      AsmSection *save = cur_sect;
      switch_section(get_section(".bss"));
      do_align(align, false);
      define_label(sname);
      s->type = s->type ? s->type : ASM_STT_OBJECT;
      s->size = size;
      emit_zero(size);
      cur_sect = save;
      return true;
    }
    s->is_common = true;
    s->defined = true;
    s->size = size;
    s->common_align = align;
    s->type = ASM_STT_OBJECT;
    if (!s->bind_set)
      s->bind = ASM_STB_GLOBAL;
    return true;
  }
  if (DIR_IS(".zero") || DIR_IS(".space") || DIR_IS(".skip")) {
    int n = split_operands(buf, ops);
    int64_t size = parse_imm(ops[0]);
    int64_t fill = n >= 2 ? parse_imm(ops[1]) : 0;
    if (fill) {
      for (int64_t i = 0; i < size; i++)
        emit_le(fill, 1);
    } else {
      emit_zero(size);
    }
    return true;
  }
  if (DIR_IS(".byte"))  { dir_data(1, args); return true; }
  if (DIR_IS(".hword") || DIR_IS(".short") || DIR_IS(".2byte")) {
    dir_data(2, args);
    return true;
  }
  if (DIR_IS(".word") || DIR_IS(".long") || DIR_IS(".int") ||
      DIR_IS(".4byte")) {
    dir_data(4, args);
    return true;
  }
  if (DIR_IS(".xword") || DIR_IS(".quad") || DIR_IS(".8byte") ||
      DIR_IS(".dword")) {
    dir_data(8, args);
    return true;
  }
  if (DIR_IS(".inst")) {
    int n = split_operands(buf, ops);
    for (int i = 0; i < n; i++)
      emit32(parse_imm(ops[i]));
    return true;
  }
  if (DIR_IS(".ascii") || DIR_IS(".asciz") || DIR_IS(".string")) {
    const char *p = args;
    char *out = malloc(strlen(args) + 1);
    for (;;) {
      int64_t len = parse_string(&p, p, out);
      emit_bytes(out, len);
      if (strcmp(name, ".ascii"))
        emit_le(0, 1);
      p = skip_space(p);
      if (*p != ',')
        break;
      p++;
    }
    free(out);
    return true;
  }
  if (DIR_IS(".balign") || DIR_IS(".p2align") || DIR_IS(".align")) {
    int n = split_operands(buf, ops);
    if (n < 1)
      asm_error("missing alignment");
    int64_t v = parse_imm(ops[0]);
    // AArch64 の GNU as では .align は 2 のべき乗の指数
    int64_t align = DIR_IS(".balign") ? v : (1LL << v);
    if (n >= 2 && *skip_space(ops[1]) && parse_imm(ops[1]) != 0)
      asm_error("alignment fill value is not supported");
    do_align(align, true);
    return true;
  }
  if (DIR_IS(".tlsdesccall")) {
    const char *rest;
    char *sname = read_symbol(&rest, args);
    if (!sname)
      asm_error("bad .tlsdesccall");
    add_fixup(FX_OTHER, R_TLSDESC_CALL, false, sname, 0);
    return true;
  }
  if (DIR_IS(".ltorg") || DIR_IS(".pool")) {
    flush_literals(cur_sect);
    return true;
  }
  if (DIR_IS(".error")) {
    const char *p = args;
    char *out = malloc(strlen(args) + 1);
    int64_t len = parse_string(&p, p, out);
    out[len] = '\0';
    asm_error("%s", out);
  }
  if (DIR_IS(".file") || DIR_IS(".loc") || DIR_IS(".ident") ||
      DIR_IS(".warning") ||
      !strncmp(name, ".cfi_", 5) || !strncmp(name, ".addrsig", 8) || DIR_IS(".arch") ||
      DIR_IS(".cpu") || DIR_IS(".nolist") || DIR_IS(".list"))
    return true;
  return false;
}

//
// 文の処理
//

// .macro と .endm（GNU as の書式の一部: 引数は \名前 で参照、名前=既定値、\@ は展開の通し番号、
// \() は区切り。引数が 1 つのマクロは、呼び出しの残り全体をその引数にする）
typedef struct Macro Macro;
struct Macro {
  Macro *next;
  char *name;
  int nparam;
  char *params[32];
  char *defaults[32];
  char *body;
  size_t len, cap;
};

TLS static Macro *macros;
TLS static Macro *defining_macro;

static bool in_macro_def(void) {
  return defining_macro != NULL;
}
TLS static int macro_depth, macro_counter;

static void free_macros(void) {
  while (macros) {
    Macro *m = macros->next;
    free(macros->name);
    for (int i = 0; i < macros->nparam; i++) {
      free(macros->params[i]);
      free(macros->defaults[i]);
    }
    free(macros->body);
    free(macros);
    macros = m;
  }
  defining_macro = NULL;
  macro_depth = macro_counter = 0;
}

static Macro *find_macro(const char *name) {
  for (Macro *m = macros; m; m = m->next)
    if (!strcmp(m->name, name))
      return m;
  return NULL;
}

static void dir_macro(const char *args) {
  const char *p = skip_space(args);
  char *name = read_symbol(&p, p);
  if (!name)
    asm_error("expected a macro name");
  Macro *m = calloc(1, sizeof(Macro));
  m->name = strdup(name);
  for (;;) {
    p = skip_space(p);
    if (*p == ',')
      p = skip_space(p + 1);
    if (!*p)
      break;
    const char *start = p;
    while (is_sym_char(*p))
      p++;
    if (p == start)
      asm_error("bad macro parameter: '%s'", start);
    if (m->nparam == 32)
      asm_error("too many macro parameters");
    m->params[m->nparam] = strndup(start, p - start);
    p = skip_space(p);
    if (*p == '=') {
      p = skip_space(p + 1);
      start = p;
      while (*p && *p != ',' && *p != ' ' && *p != '\t')
        p++;
      m->defaults[m->nparam] = strndup(start, p - start);
    }
    m->nparam++;
  }
  m->next = macros;
  macros = m;
  defining_macro = m;
}

static void assemble_text(const char *text);

static void expand_macro(Macro *m, const char *args) {
  if (++macro_depth > 100)
    asm_error("macro nesting too deep");

  // 引数を分ける（カンマ、またはカンマがなければ空白。引数が 1 つなら全体）
  char *buf = tmp_strdup(skip_space(args));
  char *vals[32] = {0};
  int nval = 0;
  if (m->nparam == 1) {
    if (*buf)
      vals[nval++] = buf;
  } else if (*buf) {
    bool comma = strchr(buf, ',') != NULL;
    char *p = buf;
    for (;;) {
      p = (char *)skip_space(p);
      if (nval == 32)
        asm_error("too many macro arguments");
      vals[nval++] = p;
      while (*p && *p != ',' && (comma || (*p != ' ' && *p != '\t')))
        p++;
      if (!*p)
        break;
      *p++ = '\0';
      if (!comma && !*skip_space(p))
        break;
    }
  }
  if (nval > m->nparam)
    asm_error("too many arguments for macro '%s'", m->name);
  for (int i = 0; i < nval; i++) {
    // 末尾の空白を取る
    char *e = vals[i] + strlen(vals[i]);
    while (e > vals[i] && (e[-1] == ' ' || e[-1] == '\t'))
      *--e = '\0';
  }
  // キーワード引数（name=value）を、その名前の引数に当てる。それ以外は前から順に
  {
    char *args2[32] = {0};
    int pos = 0;
    for (int i = 0; i < nval; i++) {
      char *v = vals[i];
      char *q = v;
      while (is_sym_char(*q))
        q++;
      int k = -1;
      if (q > v && *q == '=')
        for (int j = 0; j < m->nparam; j++)
          if (strlen(m->params[j]) == (size_t)(q - v) && !strncmp(m->params[j], v, q - v))
            k = j;
      if (k >= 0) {
        args2[k] = q + 1;
      } else {
        while (pos < m->nparam && args2[pos])
          pos++;
        if (pos >= m->nparam)
          asm_error("too many arguments for macro '%s'", m->name);
        args2[pos++] = v;
      }
    }
    for (int i = 0; i < m->nparam; i++)
      vals[i] = args2[i] ? args2[i] : "";
    nval = m->nparam;
  }

  // 本体の \名前 を置き換える
  size_t cap = m->len * 2 + 64, len = 0;
  char *out = malloc(cap);
  int id = macro_counter++;
  for (const char *p = m->body; *p;) {
    const char *rep = NULL;
    char num[16];
    size_t skip = 1;
    if (*p == '\\') {
      if (p[1] == '@') {
        snprintf(num, sizeof(num), "%d", id);
        rep = num;
        skip = 2;
      } else if (p[1] == '(' && p[2] == ')') {
        rep = "";
        skip = 3;
      } else {
        const char *q = p + 1;
        while (is_sym_char(*q))
          q++;
        for (int i = 0; i < m->nparam; i++) {
          if (strlen(m->params[i]) == (size_t)(q - p - 1) &&
              !strncmp(m->params[i], p + 1, q - p - 1)) {
            rep = i < nval && *vals[i] ? vals[i] : m->defaults[i] ? m->defaults[i] : "";
            skip = q - p;
            break;
          }
        }
      }
    }
    if (!rep) {
      rep = num;
      num[0] = *p;
      num[1] = '\0';
    }
    size_t rl = strlen(rep);
    while (len + rl + 1 >= cap)
      out = realloc(out, cap *= 2);
    memcpy(out + len, rep, rl);
    len += rl;
    p += skip;
  }
  out[len] = '\0';

  int line = src_line;
  assemble_text(out);
  src_line = line;
  free(out);
  macro_depth--;
}

static void assemble_stmt(char *s) {
  if (defining_macro) {
    const char *t = skip_space(s);
    if (!strncmp(t, ".endm", 5) && !is_sym_char(t[5])) {
      defining_macro = NULL;
      return;
    }
    if (!strncmp(t, ".macro", 6) && !is_sym_char(t[6]))
      asm_error("nested .macro is not supported");
    Macro *m = defining_macro;
    size_t n = strlen(s);
    if (m->len + n + 2 > m->cap) {
      m->cap = (m->len + n + 2) * 2;
      m->body = realloc(m->body, m->cap);
    }
    memcpy(m->body + m->len, s, n);
    m->len += n;
    m->body[m->len++] = '\n';
    m->body[m->len] = '\0';
    return;
  }

  for (;;) {
    s = (char *)skip_space(s);
    if (!*s)
      return;

    // 数字のローカルラベル（1:）
    if (Isdigit(*s) && s[1] == ':') {
      int n = *s - '0';
      define_label(numlabel_name(n, numlabel_count[n]++));
      s += 2;
      continue;
    }
    // ラベル（name: や "name":）
    const char *rest;
    char *name = read_symbol(&rest, s);
    if (name && *skip_space(rest) == ':') {
      define_label(name);
      s = (char *)skip_space(rest) + 1;
      continue;
    }
    break;
  }

  // . = 式（位置を進める。間はゼロで埋める）
  if (s[0] == '.' && (s[1] == ' ' || s[1] == '\t' || s[1] == '=') && *skip_space(s + 1) == '=') {
    Expr e = parse_expr_all(skip_space(s + 1) + 1);
    int64_t target = e.val;
    if (e.sym) {
      AsmSymbol *sym = resolve_symbol(get_symbol(e.sym));
      if (!sym->defined || sym->sect != cur_sect)
        asm_error("'. =' needs a symbol defined in the current section");
      target += sym->value;
    }
    if (target < cur_sect->size)
      asm_error("'. =' cannot move backwards");
    emit_zero(target - cur_sect->size);
    return;
  }

  // 命令名・指示名
  char *p = s;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  char *mn = tmp_strndup(s, p - s);
  char *args = (char *)skip_space(p);

  if (MN_IS(".macro")) {
    dir_macro(args);
    return;
  }
  if (MN_IS(".endm"))
    asm_error(".endm without .macro");
  Macro *mac = find_macro(mn);
  if (mac) {
    expand_macro(mac, args);
    return;
  }

  if (mn[0] == '.') {
    if (!directive(mn, args))
      asm_error("unknown directive '%s'", mn);
    return;
  }

  char *ops[MAX_OPS];
  int n = split_operands(tmp_strdup(args), ops);
  assemble_insn(mn, ops, n);
}

// テキストを文に分ける（改行と ';'。// 以降と、行頭の # 以降はコメント）
static void assemble_chunk(const char *text);

static void assemble_text(const char *text) {
  src_line = 1;
  assemble_chunk(text);
}

// 文字列を文（改行・;）に分けて組み立てる（かけらの文字列の行からも。インラインアセンブリは 1 行に
// ; で複数の文を書ける）
static void assemble_chunk(const char *text) {
  size_t cap = 256, len = 0;
  char *stmt = malloc(cap);
  bool in_str = false;
  bool line_start = true;

  for (const char *p = text;; p++) {
    // 文の途中（行頭でなく、文字列の外）では、特別な文字（改行・;・"・/）の手前までをまとめて写す
    // （2026-11-15。前は 1 文字ずつ下の条件を全部調べていた）。特別な文字からは下で今までどおり
    if (!in_str && !line_start) {
      const char *q = p;
      while (*q && *q != '\n' && *q != ';' && *q != '"' && *q != '/')
        q++;
      if (q > p) {
        size_t k = (size_t)(q - p);
        while (len + k + 2 >= cap)
          stmt = realloc(stmt, cap *= 2);
        memcpy(stmt + len, p, k);
        len += k;
        p = q;
      }
    }
    char c = *p;
    if (!in_str && line_start && (c == ' ' || c == '\t')) {
      // 行頭の空白は残す（ラベルの判定には影響しない）
    } else if (!in_str && line_start && c == '#') {
      while (*p && *p != '\n')
        p++;
      c = *p;
    }
    if (c != ' ' && c != '\t')
      line_start = false;

    if (!in_str && c == '/' && p[1] == '/') {
      while (*p && *p != '\n')
        p++;
      c = *p;
    }
    if (c == '"' && (len == 0 || stmt[len - 1] != '\\'))
      in_str = !in_str;

    if (c == '\0' || c == '\n' || (c == ';' && !in_str)) {
      stmt[len] = '\0';
      assemble_stmt(stmt);
      len = 0;
      in_str = false;
      if (c == '\0')
        break;
      if (c == '\n') {
        src_line++;
        line_start = true;
      }
      continue;
    }
    if (len + 2 >= cap)
      stmt = realloc(stmt, cap *= 2);
    stmt[len++] = c;
  }
  free(stmt);
}

static void finish(void) {
  if (defining_macro)
    asm_error(".macro without .endm");
  // 残っているリテラルプールを、各セクションの末尾に置く
  for (int i = 0; i < obj->nsect && nlit; i++)
    flush_literals(obj->sects[i]);

  // .set で別名にしたシンボルを、元のシンボルと同じ場所にする
  for (int i = 0; i < obj->nsym; i++) {
    AsmSymbol *s = obj->syms[i];
    if (!s->equ)
      continue;
    AsmSymbol *t = resolve_symbol(s);
    if (t->defined && !t->is_common) {
      s->defined = true;
      s->sect = t->sect;
      s->value = t->value;
      if (!s->type)
        s->type = t->type;
      if (!s->size)
        s->size = t->size;
    }
  }

  // TLS のセクションにあるシンボルの型は STT_TLS（GNU as と同じ）
  for (int i = 0; i < obj->nsym; i++) {
    AsmSymbol *s = obj->syms[i];
    if (s->defined && s->sect && (s->sect->flags & ASM_SHF_TLS) &&
        (s->type == ASM_STT_OBJECT || s->type == ASM_STT_NOTYPE))
      s->type = ASM_STT_TLS;
  }

  // 未定義で参照されたシンボルは GLOBAL（.weak を除く）
  for (int i = 0; i < obj->nsym; i++) {
    AsmSymbol *s = obj->syms[i];
    if (!s->defined && !s->bind_set)
      s->bind = ASM_STB_GLOBAL;
  }

  resolve_fixups();
}

AsmObj *asm_assemble(const char *text, const char *filename) {
  tmp_free_all();
  free(symmap.buckets);
  free(sectmap.buckets);
  obj = calloc(1, sizeof(AsmObj));
  symmap = (HashMap){0};
  sectmap = (HashMap){0};
  // 記号の表は、文字列の長さから数を見込んで最初から大きく（何度も作り直さない。2026-11-16）
  {
    size_t want = strlen(text) / 16 + (size_t)slimcc_frag_nrecs() / 4 + 256;
    int cap = 256;
    while (cap < (int)want && cap < (1 << 16))
      cap *= 2;
    hashmap_reserve(&symmap, cap);
  }
  nfix = 0;
  free(literals);
  literals = NULL;
  nlit = caplit = literal_id = 0;
  free_macros();
  memset(numlabel_count, 0, sizeof(numlabel_count));
  src_name = filename;
  prev_sect = NULL;

  // GNU as と同じく、.text、.data、.bss を最初に作る
  get_section(".text");
  get_section(".data");
  get_section(".bss");
  cur_sect = obj->sects[0];

  assemble_text(text);
  finish();
  tmp_free_all();
  // セクション名の表は ELF の書き出し（find_section）で使うので残す
  free(symmap.buckets);
  symmap = (HashMap){0};
  return obj;
}

void asm_free(AsmObj *o) {
  if (!o)
    return;
  for (int i = 0; i < o->nsect; i++) {
    free(o->sects[i]->name);
    free(o->sects[i]->data);
    free(o->sects[i]->rels);
    free(o->sects[i]);
  }
  for (int i = 0; i < o->nsym; i++) {
    free(o->syms[i]->name);
    free(o->syms[i]->equ);
    free(o->syms[i]);
  }
  free(o->sects);
  free(o->syms);
  for (int i = 0; i < o->nstr; i++)
    free(o->strs[i]);
  free(o->strs);
  if (o == obj) {
    free(sectmap.buckets);
    sectmap = (HashMap){0};
    obj = NULL;
  }
  free(o);
}

//
// 保存と読み込み（Monocs のコンパイルのキャッシュ。os/slimcc/cache.c）
//
// 形式（リトルエンディアン。文字列は長さ u32 と中身、NULL は長さ 0xFFFFFFFF）:
//   "SLMO" u32 版
//   u32 セクションの数、各セクション: 名前、u32 type、u64 flags、i64 align、i64 size、u8 中身があるか、中身、
//     u32 リロケーションの数、各: i64 offset、i32 type、i64 addend、u8 宛先の種類（0 セクションの番号、
//     1 シンボルの番号、2 名前）、u32 番号か名前
//   u32 シンボルの数、各シンボル: 名前、i32 セクションの番号（-1 は未定義）、i64 value、i64 size、
//     u8 bind・type・vis、u8 印（variant_pcs、bind_set、defined、is_common、referenced）、i64 common_align、equ
// 読み込みは壊れた入力でも止まらないように、数と範囲を確かめる（壊れていれば NULL）

#define ASM_SAVE_VERSION 1

static void put_u8(FILE *f, uint8_t v) { fputc(v, f); }
static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

static void put_str(FILE *f, const char *s) {
  if (!s) {
    put_u32(f, 0xFFFFFFFF);
    return;
  }
  uint32_t n = strlen(s);
  put_u32(f, n);
  fwrite(s, 1, n, f);
}

// 保存しなくてよい記号: 名前が .L で始まるローカルなもので、リロケーションから名前で参照されていないもの（2026-11-21。
// コンパイラが関数の中に作る分岐先のラベル .L.ir.N.M など。同じセクションの中の分岐はアセンブルのときに決まり、
// つなぐのに要らない。GNU as も .o の記号の表に入れない。vim のコンパイルのキャッシュ 22.7MB のうち 10MB 余りが
// これで、再起動の後の起動で SD カードから読んでいた）。関数名の表（slimcc_symbol_at）も .L を除くので変わらない
static bool save_drop_sym(AsmSymbol *s, HashMap *referenced) {
  return s->bind == ASM_STB_LOCAL && !strncmp(s->name, ".L", 2) && !hashmap_get(referenced, s->name);
}

bool asm_obj_save(AsmObj *o, FILE *f) {
  HashMap sects = {0}, syms = {0}, referenced = {0};
  for (int i = 0; i < o->nsect; i++)
    hashmap_put(&sects, o->sects[i]->name, (void *)(intptr_t)(i + 1));
  for (int i = 0; i < o->nsect; i++)
    for (int j = 0; j < o->sects[i]->nrel; j++)
      if (!o->sects[i]->rels[j].is_section)
        hashmap_put(&referenced, o->sects[i]->rels[j].sym, (void *)1);
  int nkeep = 0;
  for (int i = 0; i < o->nsym; i++)
    if (!save_drop_sym(o->syms[i], &referenced))
      hashmap_put(&syms, o->syms[i]->name, (void *)(intptr_t)(++nkeep));
  fwrite("SLMO", 1, 4, f);
  put_u32(f, ASM_SAVE_VERSION);
  put_u32(f, o->nsect);
  for (int i = 0; i < o->nsect; i++) {
    AsmSection *s = o->sects[i];
    put_str(f, s->name);
    put_u32(f, s->type);
    put_u64(f, s->flags);
    put_u64(f, s->align);
    put_u64(f, s->size);
    put_u8(f, s->data != NULL);
    if (s->data)
      fwrite(s->data, 1, s->size, f);
    put_u32(f, s->nrel);
    for (int j = 0; j < s->nrel; j++) {
      AsmReloc *r = &s->rels[j];
      put_u64(f, r->offset);
      put_u32(f, r->type);
      put_u64(f, r->addend);
      intptr_t k = (intptr_t)hashmap_get(r->is_section ? &sects : &syms, r->sym);
      if (k) {
        put_u8(f, r->is_section ? 0 : 1);
        put_u32(f, k - 1);
      } else {
        put_u8(f, 2 | (r->is_section ? 4 : 0));
        put_str(f, r->sym);
      }
    }
  }
  put_u32(f, nkeep);
  for (int i = 0; i < o->nsym; i++) {
    AsmSymbol *s = o->syms[i];
    if (save_drop_sym(s, &referenced))
      continue;
    put_str(f, s->name);
    put_u32(f, s->sect ? (uint32_t)s->sect->index : 0xFFFFFFFF);
    put_u64(f, s->value);
    put_u64(f, s->size);
    put_u8(f, s->bind);
    put_u8(f, s->type);
    put_u8(f, s->vis);
    put_u8(f, s->variant_pcs | s->bind_set << 1 | s->defined << 2 | s->is_common << 3 | s->referenced << 4);
    put_u64(f, s->common_align);
    put_str(f, s->equ);
  }
  free(sects.buckets);
  free(syms.buckets);
  free(referenced.buckets);
  return !ferror(f);
}

// 読む元（2026-11-20）: FILE か、メモリの上の [p, end)。Monocs は実行ファイル・コンパイルのキャッシュをメモリに読んでから
// 渡すので、メモリから直接読む（前は fmemopen の FILE から読み、picolibc の fread が 1 バイトずつ getc を呼んで、vim の
// 部品 8MB を読むのに約 2.5 秒かかった）
typedef struct {
  FILE *f;
  const unsigned char *p, *end;
} AsmRd;

static bool get_bytes(AsmRd *f, void *p, size_t n) {
  if (f->f)
    return fread(p, 1, n, f->f) == n;
  if ((size_t)(f->end - f->p) < n)
    return false;
  memcpy(p, f->p, n);
  f->p += n;
  return true;
}

static bool get_u32(AsmRd *f, uint32_t *v) { return get_bytes(f, v, 4); }
static bool get_u64(AsmRd *f, uint64_t *v) { return get_bytes(f, v, 8); }

static bool get_u8(AsmRd *f, uint8_t *v) {
  if (!f->f) {
    if (f->p == f->end)
      return false;
    *v = *f->p++;
    return true;
  }
  int c = fgetc(f->f);
  *v = c;
  return c != EOF;
}
#define ASM_LOAD_MAX (64 << 20)   // 1 つの数・文字列・中身の上限（壊れた入力で大きく確保しない）

static bool get_str(AsmRd *f, char **out) {
  uint32_t n;
  *out = NULL;
  if (!get_u32(f, &n))
    return false;
  if (n == 0xFFFFFFFF)
    return true;
  if (n > ASM_LOAD_MAX || !(*out = malloc(n + 1)))
    return false;
  (*out)[n] = '\0';
  return get_bytes(f, *out, n);
}

static AsmObj *asm_obj_load_rd(AsmRd *f) {
  char magic[4];
  uint32_t ver, n;
  if (!get_bytes(f, magic, 4) || memcmp(magic, "SLMO", 4) || !get_u32(f, &ver) || ver != ASM_SAVE_VERSION)
    return NULL;
  AsmObj *o = calloc(1, sizeof(AsmObj));
  // リロケーションの宛先は、シンボルを読んでから名前を入れる（番号で覚えておく）
  typedef struct { AsmReloc *r; uint32_t idx; } Pending;
  Pending *pend = NULL;
  int npend = 0, cappend = 0;
  bool ok = get_u32(f, &n) && n <= ASM_LOAD_MAX / 64;
  if (ok) {
    o->sects = calloc(n ? n : 1, sizeof(AsmSection *));
    o->capsect = n;
  }
  for (uint32_t i = 0; ok && i < n; i++) {
    AsmSection *s = calloc(1, sizeof(AsmSection));
    o->sects[o->nsect++] = s;
    uint64_t flags, align, size;
    uint8_t has;
    ok = get_str(f, &s->name) && s->name && get_u32(f, &s->type) && get_u64(f, &flags) &&
         get_u64(f, &align) && get_u64(f, &size) && get_u8(f, &has) && size <= ASM_LOAD_MAX;
    if (!ok)
      break;
    s->flags = flags;
    s->align = align;
    s->size = s->cap = size;
    s->index = i;
    if (has) {
      s->data = malloc(size ? size : 1);
      ok = get_bytes(f, s->data, size);
    }
    uint32_t nrel;
    ok = ok && get_u32(f, &nrel) && nrel <= ASM_LOAD_MAX / 16;
    if (!ok)
      break;
    s->rels = calloc(nrel ? nrel : 1, sizeof(AsmReloc));
    s->nrel = s->caprel = nrel;
    for (uint32_t j = 0; ok && j < nrel; j++) {
      AsmReloc *r = &s->rels[j];
      uint64_t ofs, addend;
      uint32_t type, idx;
      uint8_t kind;
      ok = get_u64(f, &ofs) && get_u32(f, &type) && get_u64(f, &addend) && get_u8(f, &kind);
      if (!ok)
        break;
      r->offset = ofs;
      r->type = type;
      r->addend = addend;
      if (kind & 2) {
        char *name;
        ok = get_str(f, &name) && name;
        if (ok) {
          o->strs = realloc(o->strs, sizeof(char *) * (o->nstr + 1));
          o->strs[o->nstr++] = name;
          r->sym = name;
          r->is_section = kind & 4;
        } else {
          free(name);
        }
      } else if (kind == 0) {
        ok = get_u32(f, &idx) && idx < n;
        r->is_section = true;
        if (ok && idx < (uint32_t)o->nsect)
          r->sym = o->sects[idx]->name;
        else if (ok) {
          // まだ読んでいないセクション: 後で入れる
          if (npend == cappend)
            pend = realloc(pend, sizeof(Pending) * (cappend = cappend ? cappend * 2 : 64));
          pend[npend++] = (Pending){r, idx | 0x80000000u};
        }
      } else {
        ok = get_u32(f, &idx);
        if (ok) {
          if (npend == cappend)
            pend = realloc(pend, sizeof(Pending) * (cappend = cappend ? cappend * 2 : 64));
          pend[npend++] = (Pending){r, idx};
        }
      }
    }
  }
  uint32_t nsym = 0;
  ok = ok && get_u32(f, &nsym) && nsym <= ASM_LOAD_MAX / 64;
  if (ok) {
    o->syms = calloc(nsym ? nsym : 1, sizeof(AsmSymbol *));
    o->capsym = nsym;
  }
  for (uint32_t i = 0; ok && i < nsym; i++) {
    AsmSymbol *s = calloc(1, sizeof(AsmSymbol));
    o->syms[o->nsym++] = s;
    uint32_t sect;
    uint64_t value, size, calign;
    uint8_t bits;
    ok = get_str(f, &s->name) && s->name && get_u32(f, &sect) && get_u64(f, &value) &&
         get_u64(f, &size) && get_u8(f, &s->bind) && get_u8(f, &s->type) && get_u8(f, &s->vis) &&
         get_u8(f, &bits) && get_u64(f, &calign) && get_str(f, &s->equ) &&
         (sect == 0xFFFFFFFF || sect < (uint32_t)o->nsect);
    if (!ok)
      break;
    s->sect = sect == 0xFFFFFFFF ? NULL : o->sects[sect];
    s->value = value;
    s->size = size;
    s->common_align = calign;
    s->variant_pcs = bits & 1;
    s->bind_set = bits >> 1 & 1;
    s->defined = bits >> 2 & 1;
    s->is_common = bits >> 3 & 1;
    s->referenced = bits >> 4 & 1;
  }
  for (int i = 0; ok && i < npend; i++) {
    uint32_t idx = pend[i].idx & 0x7FFFFFFF;
    if (pend[i].idx & 0x80000000u)
      ok = idx < (uint32_t)o->nsect && (pend[i].r->sym = o->sects[idx]->name);
    else
      ok = idx < (uint32_t)o->nsym && (pend[i].r->sym = o->syms[idx]->name);
  }
  free(pend);
  if (!ok) {
    asm_free(o);
    return NULL;
  }
  return o;
}

AsmObj *asm_obj_load(FILE *in) {
  AsmRd r = {in, NULL, NULL};
  return asm_obj_load_rd(&r);
}

AsmObj *asm_obj_load_mem(const void *p, size_t n) {
  AsmRd r = {NULL, p, (const unsigned char *)p + n};
  return asm_obj_load_rd(&r);
}

//
// 比較用の出力（GNU as の .o を同じ形式にする test-a64/objdump.py と比べる）
//

static int cmp_sect(const void *a, const void *b) {
  return strcmp((*(AsmSection **)a)->name, (*(AsmSection **)b)->name);
}

static int cmp_sym(const void *a, const void *b) {
  return strcmp((*(AsmSymbol **)a)->name, (*(AsmSymbol **)b)->name);
}

static int cmp_rel(const void *a, const void *b) {
  int64_t x = ((AsmReloc *)a)->offset, y = ((AsmReloc *)b)->offset;
  return x < y ? -1 : x > y;
}

static bool is_output_symbol(AsmSymbol *s) {
  if (has_prefix(s->name, ".L"))
    return false;
  if (s->defined)
    return true;
  return s->referenced || s->bind_set;
}

void asm_dump(AsmObj *o, FILE *out) {
  AsmSection **sects = malloc(sizeof(*sects) * o->nsect);
  memcpy(sects, o->sects, sizeof(*sects) * o->nsect);
  qsort(sects, o->nsect, sizeof(*sects), cmp_sect);

  for (int i = 0; i < o->nsect; i++) {
    AsmSection *sec = sects[i];
    if (sec->size == 0 && sec->nrel == 0 && !has_prefix(sec->name, ".note"))
      continue;
    fprintf(out, "section %s type=%u flags=0x%" PRIx64 " align=%" PRIi64 " size=%" PRIi64 "\n",
            sec->name, sec->type, sec->flags, sec->align, sec->size);
    if (sec->type != ASM_SHT_NOBITS) {
      for (int64_t j = 0; j < sec->size; j += 32) {
        fprintf(out, "  data %06" PRIx64 " ", j);
        for (int64_t k = j; k < j + 32 && k < sec->size; k++)
          fprintf(out, "%02x", sec->data[k]);
        fprintf(out, "\n");
      }
    }
    qsort(sec->rels, sec->nrel, sizeof(AsmReloc), cmp_rel);
    for (int j = 0; j < sec->nrel; j++) {
      AsmReloc *r = &sec->rels[j];
      fprintf(out, "  reloc %06" PRIx64 " %d %s%s%s %+" PRIi64 "\n", r->offset, r->type,
              r->is_section ? "[" : "", r->sym, r->is_section ? "]" : "", r->addend);
    }
  }

  AsmSymbol **syms = malloc(sizeof(*syms) * (o->nsym + 1));
  int n = 0;
  for (int i = 0; i < o->nsym; i++)
    if (is_output_symbol(o->syms[i]))
      syms[n++] = o->syms[i];
  qsort(syms, n, sizeof(*syms), cmp_sym);
  for (int i = 0; i < n; i++) {
    AsmSymbol *s = syms[i];
    const char *sect = s->is_common ? "*COM*" : !s->defined ? "*UND*" : s->sect ? s->sect->name : "*ABS*";
    fprintf(out, "symbol %s bind=%d type=%d vis=%d sect=%s value=%" PRIi64 " size=%" PRIi64 "\n",
            s->name, s->bind, s->type, s->vis | (s->variant_pcs ? 0x80 : 0), sect, s->is_common ? s->common_align : s->value,
            s->size);
  }
  free(sects);
  free(syms);
}

//
// ELF の .o（ELF64、リトルエンディアン、ET_REL、EM_AARCH64）の書き出し
// 出典: System V ABI の ELF の章（gABI）、AAELF64
//
// セクションの並び: [0] NULL、内容のあるセクション、.rela.*、.symtab、.strtab、.shstrtab
// シンボルの並び: [0] NULL、セクションシンボル、ローカルなシンボル、グローバル・weak なシンボル

#define ELF_SHT_SYMTAB 2
#define ELF_SHT_STRTAB 3
#define ELF_SHT_RELA 4
#define ELF_SHF_INFO_LINK 0x40
#define ELF_SHN_ABS 0xFFF1
#define ELF_SHN_COMMON 0xFFF2
#define ELF_STT_SECTION 3
#define ELF_EM_AARCH64 183

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
  if (b->len + n > b->cap) {
    b->cap = MAX(b->cap * 2, b->len + n + 1024);
    b->buf = realloc(b->buf, b->cap);
  }
  if (n)
    memcpy(b->buf + b->len, p, n);   // n が 0 のとき p は NULL のことがある
  b->len += n;
}

static void buf_le(Buf *b, uint64_t v, int n) {
  uint8_t t[8];
  for (int i = 0; i < n; i++)
    t[i] = v >> (8 * i);
  buf_put(b, t, n);
}

static void buf_zero(Buf *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    buf_le(b, 0, 1);
}

static void buf_align(Buf *b, size_t align) {
  while (b->len % align)
    buf_le(b, 0, 1);
}

static uint32_t strtab_add(Buf *b, const char *s) {
  if (!*s)
    return 0;
  uint32_t ofs = b->len;
  buf_put(b, s, strlen(s) + 1);
  return ofs;
}

typedef struct {
  uint32_t name, type;
  uint64_t flags, offset, size;
  uint32_t link, info;
  uint64_t align, entsize;
} Shdr;

// シンボル表に入れるか。.L で始まるシンボルは、リロケーションが名前で参照するときだけ入れる
static bool is_elf_symbol(AsmSymbol *s, HashMap *by_name) {
  if (has_prefix(s->name, ".L"))
    return hashmap_get(by_name, s->name) != NULL;
  if (s->defined)
    return true;
  return s->referenced || s->bind_set;
}

void asm_write_elf(AsmObj *o, FILE *out) {
  Buf file = {0}, shstr = {0}, str = {0}, symtab = {0};
  buf_le(&shstr, 0, 1);
  buf_le(&str, 0, 1);

  int nsect = o->nsect;
  // セクション番号: 内容のあるセクションは 1 から
  int first_rela = 1 + nsect;
  int nrela = 0;
  for (int i = 0; i < nsect; i++)
    if (o->sects[i]->nrel)
      nrela++;
  int symtab_idx = first_rela + nrela;
  int strtab_idx = symtab_idx + 1;
  int shstrtab_idx = symtab_idx + 2;
  int shnum = shstrtab_idx + 1;
  Shdr *sh = calloc(shnum, sizeof(Shdr));

  // リロケーションが名前で参照するシンボル
  HashMap by_name = {0};
  for (int i = 0; i < nsect; i++)
    for (int j = 0; j < o->sects[i]->nrel; j++)
      if (!o->sects[i]->rels[j].is_section)
        hashmap_put(&by_name, o->sects[i]->rels[j].sym, (void *)1);

  // シンボル表
  HashMap symidx = {0};
  int nsyms = 0;
  buf_zero(&symtab, 24);
  nsyms++;
  for (int i = 0; i < nsect; i++) {
    // st_name、st_info、st_other、st_shndx、st_value、st_size
    buf_le(&symtab, 0, 4);
    buf_le(&symtab, ELF_STT_SECTION, 1);
    buf_le(&symtab, 0, 1);
    buf_le(&symtab, 1 + i, 2);
    buf_le(&symtab, 0, 8);
    buf_le(&symtab, 0, 8);
    nsyms++;
  }
  int first_global = 0;
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1)
      first_global = nsyms;
    for (int i = 0; i < o->nsym; i++) {
      AsmSymbol *s = o->syms[i];
      if (!is_elf_symbol(s, &by_name))
        continue;
      bool local = s->defined && !s->is_common && s->bind == ASM_STB_LOCAL;
      if (local != (pass == 0))
        continue;
      uint16_t shndx = s->is_common ? ELF_SHN_COMMON : !s->defined ? 0
                     : s->sect ? 1 + s->sect->index : ELF_SHN_ABS;
      uint8_t bind = local ? ASM_STB_LOCAL : (s->bind == ASM_STB_WEAK ? ASM_STB_WEAK : ASM_STB_GLOBAL);
      buf_le(&symtab, strtab_add(&str, s->name), 4);
      buf_le(&symtab, (bind << 4) | s->type, 1);
      buf_le(&symtab, s->vis | (s->variant_pcs ? 0x80 : 0), 1);
      buf_le(&symtab, shndx, 2);
      buf_le(&symtab, s->is_common ? s->common_align : s->value, 8);
      buf_le(&symtab, s->size, 8);
      hashmap_put(&symidx, s->name, (void *)(intptr_t)nsyms);
      nsyms++;
    }
  }

  // ELF ヘッダーは最後に書き直す
  buf_zero(&file, 64);

  for (int i = 0; i < nsect; i++) {
    AsmSection *sec = o->sects[i];
    Shdr *h = &sh[1 + i];
    h->name = strtab_add(&shstr, sec->name);
    h->type = sec->type;
    h->flags = sec->flags;
    h->align = sec->align;
    h->size = sec->size;
    if (sec->type != ASM_SHT_NOBITS) {
      buf_align(&file, MAX(sec->align, 1));
      h->offset = file.len;
      buf_put(&file, sec->data, sec->size);
    } else {
      h->offset = file.len;
    }
  }

  int ri = first_rela;
  for (int i = 0; i < nsect; i++) {
    AsmSection *sec = o->sects[i];
    if (!sec->nrel)
      continue;
    Shdr *h = &sh[ri++];
    char *rela_name = format(".rela%s", sec->name);
    h->name = strtab_add(&shstr, rela_name);
    free(rela_name);
    h->type = ELF_SHT_RELA;
    h->flags = ELF_SHF_INFO_LINK;
    h->link = symtab_idx;
    h->info = 1 + i;
    h->align = 8;
    h->entsize = 24;
    buf_align(&file, 8);
    h->offset = file.len;
    for (int j = 0; j < sec->nrel; j++) {
      AsmReloc *r = &sec->rels[j];
      uint64_t sidx;
      if (r->is_section) {
        sidx = 1 + find_section(r->sym)->index;
      } else {
        void *v = hashmap_get(&symidx, r->sym);
        if (!v)
          error("assembler: symbol '%s' is not in the symbol table", r->sym);
        sidx = (intptr_t)v;
      }
      buf_le(&file, r->offset, 8);
      buf_le(&file, (sidx << 32) | (uint32_t)r->type, 8);
      buf_le(&file, r->addend, 8);
    }
    h->size = file.len - h->offset;
  }

  Shdr *h = &sh[symtab_idx];
  h->name = strtab_add(&shstr, ".symtab");
  h->type = ELF_SHT_SYMTAB;
  h->link = strtab_idx;
  h->info = first_global;
  h->align = 8;
  h->entsize = 24;
  buf_align(&file, 8);
  h->offset = file.len;
  h->size = symtab.len;
  buf_put(&file, symtab.buf, symtab.len);

  h = &sh[strtab_idx];
  h->name = strtab_add(&shstr, ".strtab");
  h->type = ELF_SHT_STRTAB;
  h->align = 1;
  h->offset = file.len;
  h->size = str.len;
  buf_put(&file, str.buf, str.len);

  h = &sh[shstrtab_idx];
  h->name = strtab_add(&shstr, ".shstrtab");
  h->type = ELF_SHT_STRTAB;
  h->align = 1;
  h->offset = file.len;
  h->size = shstr.len;
  buf_put(&file, shstr.buf, shstr.len);

  buf_align(&file, 8);
  uint64_t shoff = file.len;
  for (int i = 0; i < shnum; i++) {
    buf_le(&file, sh[i].name, 4);
    buf_le(&file, sh[i].type, 4);
    buf_le(&file, sh[i].flags, 8);
    buf_le(&file, 0, 8); // sh_addr
    buf_le(&file, sh[i].offset, 8);
    buf_le(&file, sh[i].size, 8);
    buf_le(&file, sh[i].link, 4);
    buf_le(&file, sh[i].info, 4);
    buf_le(&file, sh[i].align, 8);
    buf_le(&file, sh[i].entsize, 8);
  }

  // ELF ヘッダー
  Buf eh = {0};
  static const uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
  buf_put(&eh, ident, 16);
  buf_le(&eh, 1, 2);              // e_type: ET_REL
  buf_le(&eh, ELF_EM_AARCH64, 2); // e_machine
  buf_le(&eh, 1, 4);              // e_version
  buf_le(&eh, 0, 8);              // e_entry
  buf_le(&eh, 0, 8);              // e_phoff
  buf_le(&eh, shoff, 8);          // e_shoff
  buf_le(&eh, 0, 4);              // e_flags
  buf_le(&eh, 64, 2);             // e_ehsize
  buf_le(&eh, 0, 2);              // e_phentsize
  buf_le(&eh, 0, 2);              // e_phnum
  buf_le(&eh, 64, 2);             // e_shentsize
  buf_le(&eh, shnum, 2);          // e_shnum
  buf_le(&eh, shstrtab_idx, 2);   // e_shstrndx
  memcpy(file.buf, eh.buf, 64);

  fwrite(file.buf, 1, file.len, out);
  free(file.buf);
  free(eh.buf);
  free(shstr.buf);
  free(str.buf);
  free(symtab.buf);
  free(sh);
  free(symidx.buckets);
  free(by_name.buckets);
}
