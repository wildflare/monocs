// slimcc-a64: AArch64 向けのコード生成（GNU as のアセンブリを出力する）
//
// x86-64 版（third_party/slimcc/codegen.c）の構成（一時スタック、関数呼び出しの準備、
// データの出力など）を引き継ぎ、命令と呼び出し規約を AArch64 に置き換えた。
// 出典: Arm Architecture Reference Manual for A-profile（A64 命令）
//       Procedure Call Standard for the Arm 64-bit Architecture（AAPCS64）
//
// レジスタの使い方
//   x0 / w0     整数・ポインタの値（アキュムレータ）。4 バイト以下の型は w0 で扱い、
//               1・2 バイトの型は 32 ビットに符号拡張・ゼロ拡張しておく。上位 32 ビットは不定
//   v0          浮動小数点の値（float は s0、double は d0、long double は q0）
//   x1, v1      二項演算で一時スタックから戻した値など
//   x2〜x7      命令列の中の作業用（関数呼び出しの引数を並べている間を除く）
//   x8          構造体を返すときの、戻り値の領域のアドレス（AAPCS64 の間接結果レジスタ）
//   x9〜x15     一時スタック（式の途中の値）。足りないときや呼び出しをまたぐときはフレームに退避
//   v16〜v23    浮動小数点の一時スタック
//   x16, x17    アドレス計算・メモリコピーの作業用
//   x19         局所変数の境界が 16 を超える関数での、局所変数の基準
//   x20         _BitInt の作業バッファ（BUF）の基準
//   x29, x30    フレームポインタ、リンクレジスタ
//
// フレーム（x29 から下）
//   [x29 + 16 ...]   スタックで渡された引数
//   [x29, x29 + 16)  呼び出し元の x29、x30
//   [x29 - 16, x29)  呼び出し元の x19、x20（常に保存する）
//   その下           局所変数、一時スタックの退避先、BUF（一番下）

#include "slimcc.h"

#ifdef SLIMCC_LIBRARY
__attribute__((weak)) void slimcc_progress(void);   // 進み具合（cc/libslimcc.h。使う側が定義していなければ NULL）
#endif


#define GP_MAX 8
#define FP_MAX 8

#define GP_SLOTS 7
#define FP_SLOTS 8

#define STRBUF_SZ 192
#define RESERVE_WIDTH 240

TLS static FILE *output_file;

static const char *const xreg[] = {
  "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
  "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
  "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30",
};
static const char *const wreg[] = {
  "w0",  "w1",  "w2",  "w3",  "w4",  "w5",  "w6",  "w7",  "w8",  "w9",  "w10",
  "w11", "w12", "w13", "w14", "w15", "w16", "w17", "w18", "w19", "w20", "w21",
  "w22", "w23", "w24", "w25", "w26", "w27", "w28", "w29", "w30",
};

// 一時スタックのレジスタ（x9〜x15、v16〜v23）
static const int tmpreg[GP_SLOTS] = {9, 10, 11, 12, 13, 14, 15};
static const int tmpfreg[FP_SLOTS] = {16, 17, 18, 19, 20, 21, 22, 23};

static const char x29[] = "x29";
static const char x19[] = "x19";

TLS static Obj *codegen_fn;

// レジスタ割り当て（局所変数を x21〜x28、d8〜d15 に置く）。
// 関数ごとに、出力を捨てる「下見」のコード生成（reg_analysis）で、アドレスやメモリが必要な変数
// （reg_pinned）と、使われた回数（reg_weight。ループの中ほど重い）を調べてから、本番を生成する
#define REG_FP_BASE 100
TLS static bool reg_analysis;
TLS static bool reg_disable;  // この関数では割り当てない（setjmp を呼ぶなど）
TLS static int loop_depth;

static void note_var_use(Obj *var) {
  if (!reg_analysis)
    return;
  int d = loop_depth < 4 ? loop_depth : 4;
  var->reg_weight += (int64_t)1 << (3 * d);
}

static void pin_var(Obj *var) {
  if (reg_analysis)
    var->reg_pinned = true;
  else if (var->reg)
    internal_error();
}
TLS static const char *lvar_ptr;
TLS static int lvar_base;     // lvar_ptr から下で、局所変数より上に取ってある量
TLS static int va_gr_top_ofs; // 可変長引数: 汎用レジスタの保存領域の終わり（lvar_ptr からの位置）
TLS static int va_vr_top_ofs; // 可変長引数: SIMD レジスタの保存領域の終わり
TLS static int va_gp_named;
TLS static int va_fp_named;
TLS static int va_stack_ofs;  // 可変長引数: スタックの最初の可変長引数（x29 からの位置）
TLS static int vla_base_ofs;
TLS static int rtn_ptr_ofs;
TLS static int lvar_stk_sz;
TLS static int peak_stk_usage;
TLS static int tmpbuf_sz;
TLS static int64_t rtn_label;
TLS static HashMap *ext_refs;
TLS static bool *debug_file_used;
TLS static int *debug_file_id;
TLS static StringArray debug_files;

typedef enum {
  SL_GP,
  SL_FP,
  SL_ST,
} SlotKind;

typedef struct {
  SlotKind kind;
  int gp_depth;
  int fp_depth;
  int st_depth;
  int st_ofs;
  const char *push_reg;
  long loc;
} Slot;

TLS static struct {
  Slot *data;
  int32_t capacity;
  int32_t depth;
} tmp_stack;

struct AsmContext {
  int dummy;
};

struct FuncObj {
  char *buf;
  size_t buflen;
  Obj **refs;
  int32_t ref_cnt;
  int32_t ref_capacity;
};

static void gen_expr(Node *node);
static void gen_void_expr(Node *node);
static bool ir_gen_fn(Obj *fn);
static void ir_free_all(void);
static void ir_pool_forget(void);
static void ir_rec_abort(void);
extern TLS bool ir_rec_on;
void ir_rec_text(const char *s, size_t n);
static void gen_stmt(Node *node);
static void gen_asm(Node *node);

#define Prints(str) fprintf(output_file, str)
#define Printsts(str) fprintf(output_file, "\t" str)
#define Printssn(str) fprintf(output_file, str "\n")
// レジスタ割り当ての下見（reg_analysis）では出力を捨てるので、文字列を作らない。
// 引数に副作用のある呼び出し（pop_inreg など）があるので、引数は必ず評価する
// 出力の 1 回分を作ってまとめて書く（picolibc の vfprintf は 1 文字ずつ書くので、実機では時間の 1 割を
// 使っていた。2026-11-15）。書式が slimcc_vformat の外か、長すぎるときは vfprintf
static int emit_out(const char *fmt, ...) {
  if (reg_analysis)
    return 0;
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  char buf[2048];
  bool ok;
  int r = slimcc_vformat(buf, sizeof(buf), fmt, ap, &ok);
  if (ir_rec_on) {
    // 中間表現の関数の出力は記録にためる（ir.inc の ir_emit_fn）
    if (!ok || r >= (int)sizeof(buf)) {
      va_list ap3;
      va_copy(ap3, ap2);
      r = vsnprintf(NULL, 0, fmt, ap2);
      char *big = malloc(r + 1);
      vsnprintf(big, r + 1, fmt, ap3);
      va_end(ap3);
      ir_rec_text(big, r);
      free(big);
    } else {
      ir_rec_text(buf, r);
    }
  } else if (ok && r < (int)sizeof(buf)) {
    fwrite(buf, 1, r, output_file);
  } else {
    r = vfprintf(output_file, fmt, ap2);
  }
  va_end(ap2);
  va_end(ap);
  return r;
}

#define Printstn(str) emit_out("\t" str "\n")

#define Printf(str, ...) emit_out(str, __VA_ARGS__)
#define Printfts(str, ...) emit_out("\t" str, __VA_ARGS__)
#define Printfsn(str, ...) emit_out(str "\n", __VA_ARGS__)
#define Printftn(str, ...) emit_out("\t" str "\n", __VA_ARGS__)

//
// 出力の後埋め
//
// 一時スタックへの退避（レジスタかフレームか）や、フレームの大きさは後で決まるので、
// 空行を予約しておき、決まった時点で書き込む。

static long resrvln(void) {
  long loc = ftell(output_file) + 1;
  fprintf(output_file, "\t%*s\n", RESERVE_WIDTH, "");
  return loc;
}

FMTCHK(1, 3)
static void insrtln(const char *fmt, long loc, ...) {
  char buf[RESERVE_WIDTH + 64];
  va_list ap;
  va_start(ap, loc);
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (len > RESERVE_WIDTH)
    internal_error();

  long cur_loc = ftell(output_file);
  fseek(output_file, loc, SEEK_SET);
  fputs(buf, output_file);
  fseek(output_file, cur_loc, SEEK_SET);
}

// 出力を一時的に文字列に取り込む（予約した行に書き込む命令列を、通常の関数で作るため）
TLS static FILE *capture_saved;
TLS static char *capture_buf;
TLS static size_t capture_len;

static void capture_begin(void) {
  capture_saved = output_file;
  output_file = open_memstream(&capture_buf, &capture_len);
}

// 取り込んだ命令列を 1 行（"; " 区切り）にして返す。呼び出し側で free する
static char *capture_end(void) {
  fclose(output_file);
  output_file = capture_saved;

  char *s = capture_buf;
  char *w = s;
  bool first = true;
  for (char *p = s; *p;) {
    if (*p == '\t' || *p == '\n') {
      p++;
      continue;
    }
    if (!first) {
      *w++ = ';';
      *w++ = ' ';
    }
    first = false;
    while (*p && *p != '\n')
      *w++ = *p++;
  }
  *w = '\0';
  return s;
}

static void insert_captured(long loc) {
  char *s = capture_end();
  insrtln("%s", loc, s);
  free(s);
}

//
// シンボル
//

static void push_ref(Obj *var) {
  HashEntry *ent = hashmap_get_or_insert(ext_refs, var->name, strlen(var->name));
  if (ent->val)
    return;
  ent->val = (void *)1;

  FuncObj *fn = codegen_fn->output;
  if (fn->ref_cnt == fn->ref_capacity)
    GrowArr(fn->refs, &fn->ref_capacity);

  fn->refs[fn->ref_cnt++] = var;
}

static const char *get_symbol(Obj *var) {
  if (codegen_fn && codegen_fn != var) {
    if (var->is_static_lvar)
      var->is_live = true;
    else if (!var->is_definition ||
             var->is_static ||
             (var->is_always_inline && opt_fake_always_inline))
      push_ref(var);
  }
  return var->asm_name ? var->asm_name : var->name;
}

static const char *asm_name(Obj *var) {
  return var->asm_name ? var->asm_name : var->name;
}

static int get_align(Obj *var) {
  if (var->alt_align)
    return var->alt_align;
  if (var->ty->kind == TY_ARRAY && var->ty->size >= 16)
    return MAX(16, var->ty->align);
  return var->ty->align;
}

// ラベルの番号（codegen_reset で 1 から数え直す）
TLS static int64_t label_count = 1;

static int64_t count(void) {
  return label_count++;
}

static void name_labels(Node *n) {
  for (; n; n = n->lbl.next)
    n->lbl.id = count();
}

static int tmpbuf(int sz) {
  tmpbuf_sz = MAX(tmpbuf_sz, align_to(sz, 16));
  return 0;
}

static bool is_gp_ty(Type *ty) {
  return is_integer(ty) || ty->kind == TY_PTR || ty->kind == TY_NULLPTR;
}

static bool is_scalar(Type *ty) {
  return is_gp_ty(ty) || is_flonum(ty) || ty->kind == TY_BITINT;
}

// GOT を通さず、PC 相対（adrp）で直接参照してよいか
static bool use_direct(Obj *var) {
  if (var->is_static || !(opt_fpie || opt_fpic))
    return true;

  if (opt_fpie) {
    if (var->ty->kind == TY_FUNC)
      return var->is_definition && var->export_fn;
    // x86-64 と違い、AArch64 の GNU ld は PIE で外部の変数への adrp（コピー再配置）を
    // 解決できないので、この翻訳単位で定義していない変数は GOT を通す
    return var->is_definition;
  }
  const char *vis = var->visibility ? var->visibility : opt_visibility;
  if (vis && (!strcmp(vis, "hidden"))) {
    if (var->ty->kind == TY_FUNC)
      return var->is_definition && var->export_fn;
    return var->is_definition || (!var->is_weak && var->visibility);
  }
  return false;
}

static bool has_defr(Node *node) {
  return node->dfr_from != node->dfr_dest;
}

static bool is_plain_asm(Node *node) {
  return !node->gasm.outputs &&
         !node->gasm.inputs &&
         !node->gasm.clobbers &&
         !node->gasm.labels;
}

static bool is_direct_fncall(Node *fn_expr) {
  return fn_expr->kind == ND_VAR && fn_expr->m.var->ty->kind == TY_FUNC;
}

static int32_t ovf_headroom(Type *ty, NodeKind kind) {
  int32_t bits = bit_size(ty);

  if (kind == ND_MUL)
    return bits * 2 + ty->is_unsigned;
  return bits + 1 + ty->is_unsigned;
}

static int64_t write_size(Type *ty) {
  if (ty->is_flexible && ty->origin)
    return ty->origin->size;
  return ty->size;
}

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int64_t align_to(int64_t n, int64_t align) {
  if (!align)
    internal_error();
  return (n + align - 1) / align * align;
}

static int reg_no(const char *r) {
  for (int i = 0; i <= 30; i++)
    if (!strcmp(r, xreg[i]) || !strcmp(r, wreg[i]))
      return i;
  internal_error();
}

static const char *w_of(const char *x) {
  return wreg[reg_no(x)];
}

// 型の大きさに合わせたレジスタ名（4 バイト以下は w）
static const char *gp_of(const char *x, int sz) {
  return sz <= 4 ? w_of(x) : x;
}

//
// 即値とアドレス
//

// reg に即値を入れる（movz/movn/movk）。reg は x か w のレジスタ名
static void mov_imm(const char *reg, int64_t v) {
  bool is64 = reg[0] == 'x';
  uint64_t val = is64 ? (uint64_t)v : (uint32_t)v;
  int n = is64 ? 4 : 2;

  int zero = 0, ones = 0;
  for (int i = 0; i < n; i++) {
    uint16_t c = val >> (16 * i);
    zero += (c == 0);
    ones += (c == 0xFFFF);
  }

  bool first = true;
  if (ones > zero) {
    for (int i = 0; i < n; i++) {
      uint16_t c = val >> (16 * i);
      if (c == 0xFFFF)
        continue;
      if (first)
        Printftn("movn %s, #%u, lsl #%d", reg, (uint16_t)~c, 16 * i);
      else
        Printftn("movk %s, #%u, lsl #%d", reg, c, 16 * i);
      first = false;
    }
    if (first)
      Printftn("movn %s, #0", reg);
    return;
  }
  for (int i = 0; i < n; i++) {
    uint16_t c = val >> (16 * i);
    if (c == 0)
      continue;
    if (first)
      Printftn("movz %s, #%u, lsl #%d", reg, c, 16 * i);
    else
      Printftn("movk %s, #%u, lsl #%d", reg, c, 16 * i);
    first = false;
  }
  if (first)
    Printftn("movz %s, #0", reg);
}

// dst = src + val（64 ビット）。dst・src は sp でもよい。必要なら x16 か x17 を使う
static void add_imm(const char *dst, const char *src, int64_t val) {
  if (val == 0) {
    if (strcmp(dst, src))
      Printftn("mov %s, %s", dst, src);
    return;
  }
  const char *op = val < 0 ? "sub" : "add";
  uint64_t a = val < 0 ? -(uint64_t)val : (uint64_t)val;

  if (a < 4096) {
    Printftn("%s %s, %s, #%" PRIu64, op, dst, src, a);
    return;
  }
  if (a < (1 << 24)) {
    Printftn("%s %s, %s, #%" PRIu64 ", lsl #12", op, dst, src, a >> 12);
    if (a & 0xFFF)
      Printftn("%s %s, %s, #%" PRIu64, op, dst, dst, a & 0xFFF);
    return;
  }
  const char *tmp = strcmp(src, "x16") ? "x16" : "x17";
  mov_imm(tmp, a);
  Printftn("%s %s, %s, %s", op, dst, src, tmp);
}

// [base + ofs] のメモリオペランドを buf に作る。sz はアクセスの大きさ。
// 即値で表せないときは scratch にアドレスを計算する
static const char *mem_op(char *buf, int64_t ofs, const char *base, int sz,
                          const char *scratch) {
  if ((ofs >= -256 && ofs <= 255) ||
      (ofs >= 0 && ofs % sz == 0 && ofs / sz <= 4095)) {
    if (ofs)
      snprintf(buf, STRBUF_SZ, "[%s, #%" PRIi64 "]", base, ofs);
    else
      snprintf(buf, STRBUF_SZ, "[%s]", base);
    return buf;
  }
  add_imm(scratch, base, ofs);
  snprintf(buf, STRBUF_SZ, "[%s]", scratch);
  return buf;
}

// reg と即値を比べる（cmp/cmn）
static void cmp_imm(const char *reg, int64_t val) {
  bool is64 = reg[0] == 'x';
  if (!is64)
    val = (int32_t)val;
  if (val >= 0 && val < 4096) {
    Printftn("cmp %s, #%" PRIi64, reg, val);
    return;
  }
  if (val < 0 && val > -4096) {
    Printftn("cmn %s, #%" PRIi64, reg, -val);
    return;
  }
  const char *tmp = is64 ? "x16" : "w16";
  mov_imm(tmp, val);
  Printftn("cmp %s, %s", reg, tmp);
}

// ld/st の命令名（整数）
static const char *ld_ins(int sz, bool is_unsigned) {
  switch (sz) {
  case 1: return is_unsigned ? "ldrb" : "ldrsb";
  case 2: return is_unsigned ? "ldrh" : "ldrsh";
  case 4:
  case 8: return "ldr";
  }
  internal_error();
}

static const char *st_ins(int sz) {
  switch (sz) {
  case 1: return "strb";
  case 2: return "strh";
  case 4:
  case 8: return "str";
  }
  internal_error();
}

// 浮動小数点レジスタの名前（大きさ 4: s、8: d、16: q）
static const char *freg(int n, int sz) {
  TLS static char names[3][32][4];
  TLS static bool init;
  if (!init) {
    for (int i = 0; i < 32; i++) {
      snprintf(names[0][i], 4, "s%d", i);
      snprintf(names[1][i], 4, "d%d", i);
      snprintf(names[2][i], 4, "q%d", i);
    }
    init = true;
  }
  return names[sz == 4 ? 0 : sz == 8 ? 1 : 2][n];
}

//
// メモリのコピーとゼロクリア
//

// [src, src + sz) を [dst, ...) にコピーする。x16（データ）と x17（大きいときの添字）を使う。
// src・dst は x16、x17 以外のレジスタ
static void gen_mem_copy(const char *src, const char *dst, int64_t sz) {
  if (sz <= 256) {
    for (int64_t i = 0; i < sz;) {
      int64_t rem = sz - i;
      int p2 = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
      const char *r = p2 == 8 ? "x16" : "w16";
      Printftn("%s %s, [%s, #%" PRIi64 "]", ld_ins(p2, true), r, src, i);
      Printftn("%s %s, [%s, #%" PRIi64 "]", st_ins(p2), r, dst, i);
      i += p2;
    }
    return;
  }
  int64_t c = count();
  int64_t n8 = sz / 8 * 8;
  mov_imm("x17", n8);
  Printfsn(".L.copy.%" PRIi64 ":", c);
  Printstn("sub x17, x17, #8");
  Printftn("ldr x16, [%s, x17]", src);
  Printftn("str x16, [%s, x17]", dst);
  Printftn("cbnz x17, .L.copy.%" PRIi64, c);
  if (n8 == sz)
    return;
  mov_imm("x17", n8);
  for (int64_t i = n8; i < sz;) {
    int64_t rem = sz - i;
    int p2 = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
    Printftn("%s w16, [%s, x17]", ld_ins(p2, true), src);
    Printftn("%s w16, [%s, x17]", st_ins(p2), dst);
    i += p2;
    if (i < sz)
      Printftn("add x17, x17, #%d", p2);
  }
}

// [dst, dst + sz) をゼロにする。x17 を使う。dst は x16、x17 以外
static void gen_mem_zero(const char *dst, int64_t sz) {
  if (sz <= 0)
    return;
  if (sz <= 256) {
    for (int64_t i = 0; i < sz;) {
      int64_t rem = sz - i;
      int p2 = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
      Printftn("%s %s, [%s, #%" PRIi64 "]", st_ins(p2), p2 == 8 ? "xzr" : "wzr", dst, i);
      i += p2;
    }
    return;
  }
  int64_t c = count();
  int64_t n8 = sz / 8 * 8;
  mov_imm("x17", n8);
  Printfsn(".L.zero.%" PRIi64 ":", c);
  Printstn("sub x17, x17, #8");
  Printftn("str xzr, [%s, x17]", dst);
  Printftn("cbnz x17, .L.zero.%" PRIi64, c);
  if (n8 == sz)
    return;
  mov_imm("x17", n8);
  for (int64_t i = n8; i < sz;) {
    int64_t rem = sz - i;
    int p2 = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
    Printftn("%s wzr, [%s, x17]", st_ins(p2), dst);
    i += p2;
    if (i < sz)
      Printftn("add x17, x17, #%d", p2);
  }
}

// [base + ofs] から sz バイト（1〜8）をちょうど読み、reg（x）に入れる。x17 を使う
static void load_gp_exact(const char *reg, const char *base, int64_t ofs, int sz) {
  char buf[STRBUF_SZ];
  if (sz == 8 || sz == 4 || sz == 2 || sz == 1) {
    Printftn("%s %s, %s", ld_ins(sz, true), sz == 8 ? reg : w_of(reg),
             mem_op(buf, ofs, base, sz, "x17"));
    return;
  }
  // 3、5、6、7 バイト: 下位から 4・2・1 バイトずつ読んで組み立てる
  int pos = 0;
  while (pos < sz) {
    int rem = sz - pos;
    int p2 = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
    const char *dst = pos == 0 ? reg : "x17";
    Printftn("%s %s, [%s, #%" PRIi64 "]", ld_ins(p2, true), w_of(dst), base, ofs + pos);
    if (pos)
      Printftn("orr %s, %s, x17, lsl #%d", reg, reg, pos * 8);
    pos += p2;
  }
}

// reg（x）の下位 sz バイト（1〜8）を [base + ofs] に書く。x17 を使う
static void store_gp_exact(const char *reg, const char *base, int64_t ofs, int sz) {
  char buf[STRBUF_SZ];
  if (sz == 8 || sz == 4 || sz == 2 || sz == 1) {
    Printftn("%s %s, %s", st_ins(sz), sz == 8 ? reg : w_of(reg),
             mem_op(buf, ofs, base, sz, "x17"));
    return;
  }
  int pos = 0;
  while (pos < sz) {
    int rem = sz - pos;
    int p2 = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
    if (pos == 0) {
      Printftn("%s %s, [%s, #%" PRIi64 "]", st_ins(p2), w_of(reg), base, ofs);
    } else {
      Printftn("lsr x17, %s, #%d", reg, pos * 8);
      Printftn("%s w17, [%s, #%" PRIi64 "]", st_ins(p2), base, ofs + pos);
    }
    pos += p2;
  }
}

//
// 一時スタック
//

static void clobber_all_regs(void) {
  for (int i = 0; i < tmp_stack.depth; i++)
    tmp_stack.data[i].kind = SL_ST;
}

static Slot *push_tmpstack(SlotKind kind) {
  if (tmp_stack.depth == tmp_stack.capacity)
    GrowArr(tmp_stack.data, &tmp_stack.capacity);

  long loc = resrvln();
  Slot *sl = &tmp_stack.data[tmp_stack.depth++];
  *sl = (Slot){.kind = kind, .loc = loc};
  return sl;
}

// sz は 8 バイト単位の大きさ
static Slot *pop_tmpstack(int sz) {
  tmp_stack.depth--;

  Slot *sl = &tmp_stack.data[tmp_stack.depth];
  if ((sl->kind == SL_GP && sl->gp_depth >= GP_SLOTS) ||
      (sl->kind == SL_FP && sl->fp_depth >= FP_SLOTS))
    sl->kind = SL_ST;

  if (tmp_stack.depth > 0) {
    Slot *sl2 = &tmp_stack.data[tmp_stack.depth - 1];
    int gp_depth = sl->gp_depth + (sl->kind == SL_GP);
    int fp_depth = sl->fp_depth + (sl->kind == SL_FP);
    int st_depth = sl->st_depth + (sl->kind == SL_ST) * sz;
    sl2->gp_depth = MAX(sl2->gp_depth, gp_depth);
    sl2->fp_depth = MAX(sl2->fp_depth, fp_depth);
    sl2->st_depth = MAX(sl2->st_depth, st_depth);
  }

  if (sl->kind == SL_ST) {
    if (!opt_reuse_stack || codegen_fn->dont_reuse_stk) {
      peak_stk_usage += sz * 8;
      sl->st_ofs = -peak_stk_usage;
    } else {
      int bottom = lvar_stk_sz + (sl->st_depth + sz) * 8;
      peak_stk_usage = MAX(peak_stk_usage, bottom);
      sl->st_ofs = -bottom;
    }
  }
  return sl;
}

static void push(void) {
  push_tmpstack(SL_GP);
}

static void push_from(const char *reg) {
  Slot *sl = push_tmpstack(SL_GP);
  sl->push_reg = reg;
}

// 一時スタックから値を戻す。値のあるレジスタ名を返す。
// フレームに退避されていたときや must_be_dest のときは dest に読み込む
static const char *pop_gp(const char *dest, bool must_be_dest) {
  Slot *sl = pop_tmpstack(1);
  const char *src = sl->push_reg ? sl->push_reg : "x0";
  char buf[STRBUF_SZ];

  if (sl->kind == SL_GP) {
    const char *r = xreg[tmpreg[sl->gp_depth]];
    insrtln("mov %s, %s", sl->loc, r, src);
    if (!must_be_dest)
      return r;
    Printftn("mov %s, %s", dest, r);
    return dest;
  }
  capture_begin();
  Printftn("str %s, %s", src, mem_op(buf, sl->st_ofs, lvar_ptr, 8, "x16"));
  insert_captured(sl->loc);
  Printftn("ldr %s, %s", dest, mem_op(buf, sl->st_ofs, lvar_ptr, 8, "x16"));
  return dest;
}

static const char *pop_inreg(const char *fallback) {
  return pop_gp(fallback, false);
}

static void pop(const char *dest) {
  pop_gp(dest, true);
}

static void pushf(void) {
  push_tmpstack(SL_FP);
}

// 浮動小数点の値を戻す。値のある v レジスタの番号を返す（全 128 ビットを退避する）
static int pop_fp(int dest, bool must_be_dest) {
  Slot *sl = pop_tmpstack(2);
  char buf[STRBUF_SZ];

  if (sl->kind == SL_FP) {
    int r = tmpfreg[sl->fp_depth];
    insrtln("mov v%d.16b, v0.16b", sl->loc, r);
    if (!must_be_dest)
      return r;
    Printftn("mov v%d.16b, v%d.16b", dest, r);
    return dest;
  }
  capture_begin();
  Printftn("str q0, %s", mem_op(buf, sl->st_ofs, lvar_ptr, 16, "x16"));
  insert_captured(sl->loc);
  Printftn("ldr q%d, %s", dest, mem_op(buf, sl->st_ofs, lvar_ptr, 16, "x16"));
  return dest;
}

static int popf_inreg(int fallback) {
  return pop_fp(fallback, false);
}

static void popf(int dest) {
  pop_fp(dest, true);
}

// _BitInt の値（BUF の中身）を一時スタックに積む
static void push_bitint(int sz) {
  for (int ofs = align_to(sz, 8) - 8; ofs >= 0; ofs -= 8)
    push_tmpstack(SL_ST);
}

// 積んだ _BitInt の値の、フレーム上の位置（lvar_ptr からの位置）を返す
static int pop_bitint(int sz) {
  int pos = 0;
  char buf[STRBUF_SZ];
  for (int ofs = align_to(sz, 8) - 8; ofs >= 0; ofs -= 8) {
    Slot *sl = pop_tmpstack(1);
    pos = sl->st_ofs;
    capture_begin();
    Printftn("ldr x16, [x20, #%d]", ofs);
    Printftn("str x16, %s", mem_op(buf, sl->st_ofs, lvar_ptr, 8, "x17"));
    insert_captured(sl->loc);
  }
  return pos;
}

static void push_by_ty(Type *ty) {
  switch (ty->kind) {
  case TY_VOID:    return;
  case TY_LDOUBLE:
  case TY_DOUBLE:
  case TY_FLOAT:   pushf(); return;
  case TY_BITINT:  push_bitint(ty->size); return;
  default:         push(); return;
  }
}

static void pop_by_ty(Type *ty) {
  switch (ty->kind) {
  case TY_VOID:    return;
  case TY_LDOUBLE:
  case TY_DOUBLE:
  case TY_FLOAT:   popf(0); return;
  case TY_BITINT: {
    int pos = pop_bitint(ty->size);
    add_imm("x2", lvar_ptr, pos);
    gen_mem_copy("x2", "x20", ty->size);
    return;
  }
  default: pop("x0"); return;
  }
}

//
// 呼び出し規約（AAPCS64）
//

typedef enum {
  AC_GP,  // 汎用レジスタ（整数、ポインタ、16 バイト以下の構造体、128 ビット以下の _BitInt）
  AC_FP,  // SIMD レジスタ 1 本（float、double、long double）
  AC_HFA, // 同じ浮動小数点型のメンバー 1〜4 個からなる構造体（HFA）。メンバーごとに 1 本
  AC_REF, // コピーへのポインタで渡す（16 バイトを超える構造体、128 ビットを超える _BitInt）
} ArgClass;

typedef struct {
  ArgClass cls;
  int nreg;      // 使うレジスタの数
  int elem_sz;   // AC_HFA のメンバーの大きさ（4、8、16）
  bool align16;  // 汎用レジスタを偶数番から使う・スタックで 16 バイト境界に置く
} ArgInfo;

// HFA（AAPCS64 5.9.5 節「Homogeneous Aggregates」）なら、メンバーの型と数を返す
static bool hfa_walk(Type *ty, TypeKind *base, int *cnt) {
  switch (ty->kind) {
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    if (*base == TY_VOID)
      *base = ty->kind;
    else if (*base != ty->kind)
      return false;
    return ++*cnt <= 4;
  case TY_ARRAY:
    for (int64_t i = 0; i < ty->array_len; i++)
      if (!hfa_walk(ty->base, base, cnt))
        return false;
    return true;
  case TY_STRUCT:
    for (Member *mem = ty->members; mem; mem = mem->next) {
      if (mem->is_bitfield)
        return false;
      if (!hfa_walk(mem->ty, base, cnt))
        return false;
    }
    return true;
  case TY_UNION: {
    int start = *cnt, max = start;
    for (Member *mem = ty->members; mem; mem = mem->next) {
      if (mem->is_bitfield)
        return false;
      *cnt = start;
      if (!hfa_walk(mem->ty, base, cnt))
        return false;
      max = MAX(max, *cnt);
    }
    *cnt = max;
    return true;
  }
  }
  return false;
}

static bool is_hfa(Type *ty, int *cnt, int *elem_sz) {
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return false;
  TypeKind base = TY_VOID;
  int n = 0;
  if (!hfa_walk(ty, &base, &n) || n == 0)
    return false;
  int esz = base == TY_FLOAT ? 4 : base == TY_DOUBLE ? 8 : 16;
  if (ty->size != n * esz)
    return false;
  *cnt = n;
  *elem_sz = esz;
  return true;
}

static ArgInfo classify(Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return (ArgInfo){.cls = AC_FP, .nreg = 1, .elem_sz = ty->size};
  case TY_STRUCT:
  case TY_UNION: {
    int n, esz;
    if (is_hfa(ty, &n, &esz))
      return (ArgInfo){.cls = AC_HFA, .nreg = n, .elem_sz = esz};
    if (ty->size > 16)
      return (ArgInfo){.cls = AC_REF, .nreg = 1};
    return (ArgInfo){.cls = AC_GP, .nreg = (ty->size + 7) / 8, .align16 = ty->align >= 16};
  }
  case TY_BITINT:
    if (ty->bit_cnt > 128)
      return (ArgInfo){.cls = AC_REF, .nreg = 1};
    if (ty->bit_cnt > 64)
      return (ArgInfo){.cls = AC_GP, .nreg = 2, .align16 = true};
    return (ArgInfo){.cls = AC_GP, .nreg = 1};
  }
  return (ArgInfo){.cls = AC_GP, .nreg = 1};
}

bool va_arg_need_copy(Type *ty) {
  // HFA のメンバーは、レジスタ保存領域で 16 バイトごとに離れているので組み立て直す
  ArgInfo ai = classify(ty);
  return ai.cls == AC_HFA && ai.nreg > 1;
}

bool bitint_rtn_need_copy(size_t width) {
  return width > 128;
}

static bool is_mem_rtn(Type *ty) {
  if (ty->kind == TY_VOID)
    return false;
  return classify(ty).cls == AC_REF;
}

// 引数の渡し方を決める（AAPCS64 6.8.2 節「Parameter passing rules」）。
// var->arg_reg、pass_by_stack、pass_by_ref、stack_offset を設定し、スタックで渡す量を返す。
// Linux の AAPCS64 では、可変長引数も名前付きの引数と同じ規則で渡す
static int calling_convention(Obj *var, int *gp_count, int *fp_count) {
  int stack = 0;
  int gp = *gp_count, fp = *fp_count;
  for (; var; var = var->param_next) {
    Type *ty = var->ty;
    var->pass_by_stack = false;
    var->pass_by_ref = false;
    var->arg_reg = -1;

    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->size == 0) {
      var->is_zero_sized_arg = true;
      continue;
    }

    ArgInfo ai = classify(ty);
    int64_t stk_sz = align_to(ty->size, 8);
    int stk_align = ty->align >= 16 ? 16 : 8;

    switch (ai.cls) {
    case AC_FP:
    case AC_HFA:
      if (fp + ai.nreg <= FP_MAX) {
        var->arg_reg = fp;
        fp += ai.nreg;
        continue;
      }
      fp = FP_MAX;
      break;
    case AC_REF:
      var->pass_by_ref = true;
      if (gp < GP_MAX) {
        var->arg_reg = gp++;
        continue;
      }
      stk_sz = 8;
      stk_align = 8;
      break;
    case AC_GP:
      if (ai.align16)
        gp = align_to(gp, 2);
      if (gp + ai.nreg <= GP_MAX) {
        var->arg_reg = gp;
        gp += ai.nreg;
        continue;
      }
      gp = GP_MAX;
      break;
    }
    if (!var->pass_by_ref)
      var->pass_by_stack = true;
    stack = align_to(stack, stk_align);
    var->stack_offset = stack;
    stack += stk_sz;
  }
  *gp_count = gp;
  *fp_count = fp;
  return align_to(stack, 8);
}

//
// 実行時ライブラリの呼び出し
//

static void gen_libcall(const char *fn) {
  Printftn("bl %s", fn);
  clobber_all_regs();
}

// _BitInt の実行時ライブラリ関数の Obj（コンパイルごとに作り直す）
TLS static HashMap bitint_fn_map;

static void gen_bitint_builtin_call2(const char *fn) {
  HashEntry *ent = hashmap_get_or_insert(&bitint_fn_map, fn, strlen(fn));
  Obj *var = ent->val;
  if (!var) {
    var = ent->val = get_symbol_var(fn);
    if (!var)
      error("missing builtin function %s", fn);
  }
  push_ref(var);
  gen_libcall(fn);
}

static void gen_bitint_builtin_call(NodeKind kind) {
  static const char *const fn[] = {
    [ND_NEG] = "__slimcc_bitint_neg",       [ND_BITNOT] = "__slimcc_bitint_bitnot",
    [ND_BITAND] = "__slimcc_bitint_bitand", [ND_BITOR] = "__slimcc_bitint_bitor",
    [ND_BITXOR] = "__slimcc_bitint_bitxor", [ND_SHL] = "__slimcc_bitint_shl",
    [ND_SHR] = "__slimcc_bitint_shr",       [ND_SAR] = "__slimcc_bitint_shr",
    [ND_ADD] = "__slimcc_bitint_add",       [ND_SUB] = "__slimcc_bitint_sub",
    [ND_MUL] = "__slimcc_bitint_mul",       [ND_DIV] = "__slimcc_bitint_div",
    [ND_REM] = "__slimcc_bitint_div",
  };
  gen_bitint_builtin_call2(fn[kind]);
}

//
// 値の読み込み
//

static void load_val(Type *ty, int64_t val) {
  mov_imm(ty->size == 8 ? "x0" : "w0", val);
}

static void load_fval2(Type *ty, FPVal *fval) {
  switch (ty->kind) {
  case TY_FLOAT:
    if (fval->chunk32[0] == 0) {
      Printstn("movi v0.2d, #0");
      return;
    }
    mov_imm("w16", fval->chunk32[0]);
    Printstn("fmov s0, w16");
    return;
  case TY_DOUBLE:
    if (fval->chunk[0] == 0) {
      Printstn("movi v0.2d, #0");
      return;
    }
    mov_imm("x16", fval->chunk[0]);
    Printstn("fmov d0, x16");
    return;
  case TY_LDOUBLE:
    if (fval->chunk[0] == 0 && fval->chunk[1] == 0) {
      Printstn("movi v0.2d, #0");
      return;
    }
    mov_imm("x16", fval->chunk[0]);
    mov_imm("x17", fval->chunk[1]);
    Printstn("fmov d0, x16");
    Printstn("mov v0.d[1], x17");
    return;
  }
  internal_error();
}

static void load_fval(Node *node) {
  FPVal fval = {0};
  eval_fp(node, &fval);
  load_fval2(node->ty, &fval);
}

static void load_bitint_val(Type *ty, BitBuf *buf) {
  tmpbuf(ty->size);
  int32_t cnt = (ty->bit_cnt + 63) / 64;
  for (int32_t i = 0; i < cnt; i++) {
    mov_imm("x16", (&buf->as64)[i]);
    Printftn("str x16, [x20, #%d]", i * 8);
  }
}

// [base + ofs] の値を読み込む（整数は x0/w0、浮動小数点は v0、_BitInt は BUF）
static void load_s(Type *ty, int64_t ofs, const char *base) {
  char buf[STRBUF_SZ];
  switch (ty->kind) {
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    Printftn("ldr %s, %s", freg(0, ty->size), mem_op(buf, ofs, base, ty->size, "x17"));
    return;
  case TY_BITINT:
    tmpbuf(ty->size);
    add_imm("x2", base, ofs);
    gen_mem_copy("x2", "x20", ty->size);
    return;
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
    return;
  }
  Printftn("%s %s, %s", ld_ins(ty->size, ty->is_unsigned), gp_of("x0", ty->size),
           mem_op(buf, ofs, base, ty->size, "x17"));
}

// [base + ofs] に値を書く
static void store_s(Type *ty, int64_t ofs, const char *base) {
  char buf[STRBUF_SZ];
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION: {
    const char *dst = base;
    if (ofs) {
      add_imm("x2", base, ofs);
      dst = "x2";
    }
    gen_mem_copy("x0", dst, write_size(ty));
    return;
  }
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    Printftn("str %s, %s", freg(0, ty->size), mem_op(buf, ofs, base, ty->size, "x17"));
    return;
  case TY_BITINT: {
    tmpbuf(ty->size);
    const char *dst = base;
    if (ofs) {
      add_imm("x2", base, ofs);
      dst = "x2";
    }
    gen_mem_copy("x20", dst, ty->size);
    return;
  }
  }
  Printftn("%s %s, %s", st_ins(ty->size), gp_of("x0", ty->size),
           mem_op(buf, ofs, base, ty->size, "x17"));
}

//
// ビットフィールド
//

static void gen_bitfield_load(Node *node) {
  Member *mem = node->m.member;
  bool is_unsigned = mem->ty->is_unsigned;

  if (mem->ty->kind == TY_BITINT) {
    tmpbuf(mem->ty->size);
    Printstn("mov x1, x0");
    mov_imm("w0", mem->ty->bit_cnt);
    Printstn("mov x2, x20");
    mov_imm("w3", mem->bit_width);
    mov_imm("w4", mem->bit_offset);
    mov_imm("w5", is_unsigned);
    gen_bitint_builtin_call2("__slimcc_bitint_bitfield_load");
    // 戻り値は src。ND_POST_INCDEC が x2 にアドレスを持っているので戻す
    Printstn("mov x2, x0");
    return;
  }

  if (mem->is_aligned_bitfield) {
    int p2bits = next_pow_of_two(mem->bit_offset + mem->bit_width);
    int sz = p2bits / 8;
    if (mem->bit_width == p2bits) {
      Printftn("%s %s, [x0]", ld_ins(sz, is_unsigned), gp_of("x0", sz));
      return;
    }
    Printftn("%s %s, [x0]", ld_ins(sz, true), gp_of("x0", sz));
    Printftn("%s x0, x0, #%d, #%d", is_unsigned ? "ubfx" : "sbfx", mem->bit_offset,
             mem->bit_width);
    return;
  }

  // 境界に揃っていない（packed など）: 1 バイトずつ読んで組み立てる
  Printstn("mov x2, x0");
  int pofs = bitfield_footprint(mem) - 1;
  for (int start = pofs; pofs >= !!mem->bit_offset; pofs--) {
    if (pofs == start) {
      Printftn("ldrb w0, [x2, #%d]", pofs);
      continue;
    }
    Printftn("ldrb w16, [x2, #%d]", pofs);
    Printstn("orr x0, x16, x0, lsl #8");
  }
  if (mem->bit_offset) {
    Printstn("ldrb w16, [x2]");
    Printftn("lsr w16, w16, #%d", mem->bit_offset);
    Printftn("orr x0, x16, x0, lsl #%d", 8 - mem->bit_offset);
  }
  if (mem->bit_width < 64) {
    int shft = 64 - mem->bit_width;
    Printftn("lsl x0, x0, #%d", shft);
    Printftn("%s x0, x0, #%d", is_unsigned ? "lsr" : "asr", shft);
  }
}

static void gen_bitfield_store(Node *node, bool is_void) {
  Member *mem = node->m.member;
  bool is_unsigned = mem->ty->is_unsigned;

  if (mem->ty->kind == TY_BITINT) {
    tmpbuf(mem->ty->size);
    pop("x2");
    mov_imm("w0", mem->ty->bit_cnt);
    Printstn("mov x1, x20");
    mov_imm("w3", mem->bit_width);
    mov_imm("w4", mem->bit_offset);
    gen_bitint_builtin_call2("__slimcc_bitint_bitfield_save");
    if (!is_void) {
      mov_imm("w0", mem->bit_width);
      Printstn("mov x1, x20");
      mov_imm("w2", mem->ty->bit_cnt);
      mov_imm("w3", is_unsigned);
      gen_bitint_builtin_call2("__slimcc_bitint_sign_ext");
    }
    return;
  }

  const char *ptr = pop_inreg("x1");

  if (mem->is_aligned_bitfield) {
    int p2bits = next_pow_of_two(mem->bit_offset + mem->bit_width);
    int sz = p2bits / 8;
    if (mem->bit_width == p2bits) {
      Printftn("%s %s, [%s]", st_ins(sz), gp_of("x0", sz), ptr);
    } else {
      Printftn("%s %s, [%s]", ld_ins(sz, true), gp_of("x16", sz), ptr);
      Printftn("bfi x16, x0, #%d, #%d", mem->bit_offset, mem->bit_width);
      Printftn("%s %s, [%s]", st_ins(sz), gp_of("x16", sz), ptr);
    }
  } else {
    // 境界に揃っていない: 1 バイトずつ書く
    int rem = mem->bit_offset + mem->bit_width;
    int pofs = 0;
    if (mem->bit_offset) {
      Printftn("ldrb w16, [%s]", ptr);
      Printftn("and w16, w16, #%d", (1 << mem->bit_offset) - 1);
      Printftn("lsl w17, w0, #%d", mem->bit_offset);
      Printstn("and w17, w17, #0xff");
      Printstn("orr w16, w16, w17");
      Printftn("strb w16, [%s]", ptr);
      Printftn("lsr x3, x0, #%d", 8 - mem->bit_offset);
      pofs++;
      rem -= 8;
    } else {
      Printstn("mov x3, x0");
    }
    for (int start = rem; rem > 0; rem -= 8, pofs++) {
      if (rem != start)
        Printstn("lsr x3, x3, #8");
      if (rem >= 8) {
        Printftn("strb w3, [%s, #%d]", ptr, pofs);
        continue;
      }
      Printftn("ldrb w16, [%s, #%d]", ptr, pofs);
      Printftn("and w16, w16, #%d", ~((1 << rem) - 1) & 0xFF);
      Printftn("and w17, w3, #%d", (1 << rem) - 1);
      Printstn("orr w16, w16, w17");
      Printftn("strb w16, [%s, #%d]", ptr, pofs);
      break;
    }
  }

  if (!is_void && mem->bit_width < 64) {
    int shft = 64 - mem->bit_width;
    Printftn("lsl x0, x0, #%d", shft);
    Printftn("%s x0, x0, #%d", is_unsigned ? "lsr" : "asr", shft);
  }
}

//
// アドレス
//

static void gen_tls_addr(Obj *var) {
  const char *sym = get_symbol(var);

  if (opt_femulated_tls) {
    Printftn("adrp x0, :got:\"__emutls_v.%s\"", sym);
    Printftn("ldr x0, [x0, :got_lo12:\"__emutls_v.%s\"]", sym);
    gen_libcall("__emutls_get_address");
    return;
  }
  if (opt_fpic) {
    // TLS ディスクリプタ（AAPCS64 の ELF TLS）。呼び出しが壊すのは x0、x1、x30 だけ
    Printftn("adrp x0, :tlsdesc:\"%s\"", sym);
    Printftn("ldr x1, [x0, :tlsdesc_lo12:\"%s\"]", sym);
    Printftn("add x0, x0, :tlsdesc_lo12:\"%s\"", sym);
    Printftn(".tlsdesccall \"%s\"", sym);
    Printstn("blr x1");
    Printstn("mrs x1, tpidr_el0");
    Printstn("add x0, x1, x0");
    return;
  }
  if (var->is_definition) {
    Printstn("mrs x0, tpidr_el0");
    Printftn("add x0, x0, :tprel_hi12:\"%s\", lsl #12", sym);
    Printftn("add x0, x0, :tprel_lo12_nc:\"%s\"", sym);
    return;
  }
  Printstn("mrs x1, tpidr_el0");
  Printftn("adrp x0, :gottprel:\"%s\"", sym);
  Printftn("ldr x0, [x0, :gottprel_lo12:\"%s\"]", sym);
  Printstn("add x0, x1, x0");
}

static void gen_sym_addr(const char *reg, Obj *var) {
  const char *sym = get_symbol(var);
  if (use_direct(var)) {
    Printftn("adrp %s, \"%s\"", reg, sym);
    Printftn("add %s, %s, :lo12:\"%s\"", reg, reg, sym);
  } else {
    Printftn("adrp %s, :got:\"%s\"", reg, sym);
    Printftn("ldr %s, [%s, :got_lo12:\"%s\"]", reg, reg, sym);
  }
}

static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->m.var;
    if (var->kind == OBJ_INDIR) {
      char buf[STRBUF_SZ];
      Printftn("ldr x0, %s", mem_op(buf, var->vptr->ofs, var->vptr->ptr, 8, "x17"));
      return;
    }
    if (var->kind == OBJ_LOCAL) {
      pin_var(var);
      add_imm("x0", var->ptr, var->ofs);
      return;
    }
    if (var->kind == OBJ_TLS) {
      gen_tls_addr(var);
      return;
    }
    gen_sym_addr("x0", var);
    return;
  }
  case ND_DEREF:
    gen_expr(node->m.lhs);
    return;
  case ND_CHAIN:
  case ND_COMMA:
    gen_void_expr(node->m.lhs);
    gen_addr(node->m.rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->m.lhs);
    add_imm("x0", "x0", node->m.member->offset);
    return;
  case ND_ASSIGN:
  case ND_COND:
  case ND_CAST:
  case ND_FUNCALL:
  case ND_STMT_EXPR:
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) {
      gen_expr(node);
      return;
    }
  }

  error_tok(node->tok, "not an lvalue");
}

// 局所変数を、アドレスを計算せずにフレームの基準（x29 など）からの位置で直接読み書きできるか
static bool is_direct_local(Node *node) {
  if (node->kind != ND_VAR || node->m.var->kind != OBJ_LOCAL)
    return false;
  Type *ty = node->ty;
  if (!is_scalar(ty) || is_bitfield(node) || (ty->qual & Q_ATOMIC) || ty->kind == TY_BITINT)
    return false;
  note_var_use(node->m.var);
  return true;
}

static const char *var_reg_name(Obj *var, int sz) {
  TLS static char buf[8];
  if (var->reg >= REG_FP_BASE)
    snprintf(buf, sizeof(buf), "%c%d", sz == 4 ? 's' : 'd', var->reg - REG_FP_BASE);
  else
    snprintf(buf, sizeof(buf), "%c%d", sz == 8 ? 'x' : 'w', var->reg);
  return buf;
}

// 局所変数の値を x0（浮動小数点は v0）に読む
static void load_local(Obj *var, Type *ty) {
  if (!var->reg) {
    load_s(ty, var->ofs, var->ptr);
    return;
  }
  if (var->reg >= REG_FP_BASE)
    Printftn("fmov %s, %s", freg(0, ty->size), var_reg_name(var, ty->size));
  else
    Printftn("mov %s, %s", gp_of("x0", ty->size == 8 ? 8 : 4), var_reg_name(var, ty->size));
}

// レジスタ src（x または w）の値を、型 ty の変数のレジスタ reg に入れる。
// メモリに書いて読み直したのと同じ値になるよう、1・2 バイトの型は符号拡張・ゼロ拡張する
static void set_var_reg(const char *dst_w, const char *dst_x, Type *ty, const char *src_x) {
  switch (ty->size) {
  case 8:
    Printftn("mov %s, %s", dst_x, src_x);
    return;
  case 4:
    Printftn("mov %s, %s", dst_w, w_of(src_x));
    return;
  case 2:
    Printftn("%s %s, %s", ty->is_unsigned ? "uxth" : "sxth", dst_w, w_of(src_x));
    return;
  case 1:
    Printftn("%s %s, %s", ty->is_unsigned || ty->kind == TY_BOOL ? "uxtb" : "sxtb", dst_w,
             w_of(src_x));
    return;
  }
  internal_error();
}

// x0（浮動小数点は v0）の値を局所変数に書く
static void store_local(Obj *var, Type *ty) {
  if (!var->reg) {
    store_s(ty, var->ofs, var->ptr);
    return;
  }
  if (var->reg >= REG_FP_BASE) {
    Printftn("fmov %s, %s", var_reg_name(var, ty->size), freg(0, ty->size));
    return;
  }
  char w[8], x[8];
  snprintf(w, sizeof(w), "w%d", var->reg);
  snprintf(x, sizeof(x), "x%d", var->reg);
  set_var_reg(w, x, ty, "x0");
}


// 右辺を一時スタックを使わずに直接 x1 へ読める（定数か、直接読める整数・ポインタの局所変数）
static bool is_simple_gp_operand(Node *node) {
  if (!is_integer(node->ty) && !is_ptr(node->ty) && node->ty->kind != TY_ARRAY)
    return false;
  if (node->ty->kind == TY_BITINT)
    return false;
  if (node->kind == ND_NUM)
    return true;
  // 局所変数の配列（値はアドレス）。配列からポインタへの変換も同じ
  {
    Node *n = node;
    if (n->kind == ND_CAST && is_ptr(n->ty) && n->ty->kind != TY_ARRAY)
      n = n->m.lhs;
    if (n->kind == ND_VAR && n->m.var->kind == OBJ_LOCAL && n->ty->kind == TY_ARRAY)
      return true;
  }
  // 局所変数（VLA 以外）のアドレス
  if (node->kind == ND_ADDR && node->m.lhs->kind == ND_VAR &&
      node->m.lhs->m.var->kind == OBJ_LOCAL && node->m.lhs->ty->kind != TY_VLA)
    return true;
  return is_direct_local(node);
}

// is_simple_gp_operand の値を reg（x1 など）に読む
static void load_simple_gp(const char *reg, Node *node) {
  int sz = node->ty->size;
  if (node->kind == ND_NUM) {
    mov_imm(sz == 8 ? reg : w_of(reg), node->num.val);
    return;
  }
  if (node->kind == ND_CAST && node->m.lhs->kind == ND_VAR && node->m.lhs->ty->kind == TY_ARRAY)
    node = node->m.lhs;
  if (node->kind == ND_ADDR || (node->kind == ND_VAR && node->ty->kind == TY_ARRAY)) {
    Obj *var = node->kind == ND_ADDR ? node->m.lhs->m.var : node->m.var;
    pin_var(var);
    add_imm(reg, var->ptr, var->ofs);
    return;
  }
  char buf[STRBUF_SZ];
  Obj *var = node->m.var;
  if (var->reg) {
    Printftn("mov %s, %s", gp_of(reg, sz == 8 ? 8 : 4), var_reg_name(var, sz));
    return;
  }
  Printftn("%s %s, %s", ld_ins(sz, node->ty->is_unsigned), gp_of(reg, sz),
           mem_op(buf, var->ofs, var->ptr, sz, "x17"));
}

// 即値として使える定数か（add・sub・cmp の 12 ビット、シフト量）
static bool is_imm12_operand(Node *node, int64_t *val) {
  if (node->kind != ND_NUM || !is_integer(node->ty) || node->ty->kind == TY_BITINT)
    return false;
  *val = node->num.val;
  if (node->ty->size < 8 && !node->ty->is_unsigned)
    *val = (int32_t)*val;
  return *val >= 0 && *val < 4096;
}

// アドレッシングモード: [base, #ofs] か [base, index, lsl #shift]（index が w なら sxtw）
typedef struct {
  const char *base;
  int64_t ofs;
  const char *index;
  int shift;
  char buf[2][8];
} Addr;

static bool is_plain_access(Node *node) {
  Type *ty = node->ty;
  if (!is_scalar(ty) || is_bitfield(node) || (ty->qual & Q_ATOMIC) || ty->kind == TY_BITINT)
    return false;
  return ty->size == 1 || ty->size == 2 || ty->size == 4 || ty->size == 8;
}

// レジスタに置いたポインタ・整数の変数なら、そのレジスタ名
static const char *reg_var_x(Node *node, char *buf) {
  if (node->kind == ND_VAR && node->m.var->reg && node->m.var->reg < REG_FP_BASE &&
      node->ty->size == 8 && is_direct_local(node)) {
    snprintf(buf, 8, "x%d", node->m.var->reg);
    return buf;
  }
  return NULL;
}

// ポインタの足し算 p + i * s を、p と i と s に分ける（s は定数）
static bool split_ptr_add(Node *e, Node **p, Node **i, int64_t *scale) {
  if (e->kind != ND_ADD || !is_ptr(e->ty))
    return false;
  Node *l = e->m.lhs, *r = e->m.rhs;
  if (!is_ptr(l->ty)) {
    Node *t = l;
    l = r;
    r = t;
  }
  if (!is_ptr(l->ty) || is_ptr(r->ty) || !is_integer(r->ty) || r->ty->size != 8)
    return false;
  *p = l;
  if (r->kind == ND_MUL && r->m.rhs->kind == ND_NUM) {
    *i = r->m.lhs;
    *scale = r->m.rhs->num.val;
  } else if (r->kind == ND_MUL && r->m.lhs->kind == ND_NUM) {
    *i = r->m.rhs;
    *scale = r->m.lhs->num.val;
  } else {
    *i = r;
    *scale = 1;
  }
  return true;
}

// 左辺値 node のアドレスを、コードを出さずに求められるか（局所変数か、レジスタのポインタの参照に、
// 定数のオフセットを足したもの）。右辺の計算で壊れないので、代入にも使える
static bool addr_nocode(Node *node, Addr *a) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->m.var;
    if (var->kind != OBJ_LOCAL || node->ty->kind == TY_VLA)
      return false;
    if (reg_analysis)
      var->reg_pinned = true;
    else if (var->reg)
      return false;
    a->base = var->ptr;
    a->ofs = var->ofs;
    a->index = NULL;
    return true;
  }
  case ND_MEMBER:
    if (node->m.member->is_bitfield || !addr_nocode(node->m.lhs, a))
      return false;
    a->ofs += node->m.member->offset;
    return true;
  case ND_DEREF: {
    Node *e = node->m.lhs;
    int64_t c = 0, scale;
    Node *p, *i;
    if (split_ptr_add(e, &p, &i, &scale) && is_const_expr(i, &c)) {
      c *= scale;
      e = p;
    }
    const char *r = reg_var_x(e, a->buf[0]);
    if (!r)
      return false;
    a->base = r;
    a->ofs = c;
    a->index = NULL;
    return true;
  }
  }
  return false;
}

// 読み込みのためにアドレスを求める（x0、x1 を使ってよい）。access は読む大きさ
static void gen_load_addr(Node *node, Addr *a, int access) {
  if (addr_nocode(node, a))
    return;
  a->index = NULL;
  a->ofs = 0;
  if (node->kind == ND_MEMBER && !node->m.member->is_bitfield) {
    gen_load_addr(node->m.lhs, a, 0);
    if (a->index) {
      // [base, index] にオフセットは足せないので、アドレスを計算する
      Printftn("add x0, %s, %s, %s #%d", a->base, a->index, a->index[0] == 'w' ? "sxtw" : "lsl",
               a->shift);
      a->base = "x0";
      a->index = NULL;
    }
    a->ofs += node->m.member->offset;
    return;
  }
  if (node->kind == ND_DEREF && access) {
    Node *p, *i;
    int64_t scale;
    if (split_ptr_add(node->m.lhs, &p, &i, &scale) &&
        (scale == 1 || scale == access) && (scale == 1 || scale == 2 || scale == 4 || scale == 8)) {
      int shift = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
      int64_t c;
      if (is_const_expr(i, &c)) {
        // 定数の添字
        const char *r = reg_var_x(p, a->buf[0]);
        if (!r) {
          gen_expr(p);
          r = "x0";
        }
        a->base = r;
        a->ofs = c * scale;
        return;
      }
      // 添字: レジスタの int 変数を long にしたものなら、w のまま sxtw で使う
      const char *idx = NULL;
      if (i->kind == ND_CAST && i->m.lhs->kind == ND_VAR && i->m.lhs->m.var->reg &&
          i->m.lhs->m.var->reg < REG_FP_BASE && i->m.lhs->ty->size == 4 &&
          !i->m.lhs->ty->is_unsigned && is_integer(i->m.lhs->ty) && is_direct_local(i->m.lhs)) {
        snprintf(a->buf[1], 8, "w%d", i->m.lhs->m.var->reg);
        idx = a->buf[1];
      } else {
        idx = reg_var_x(i, a->buf[1]);
      }
      const char *base = reg_var_x(p, a->buf[0]);
      if (!base && idx) {
        gen_expr(p);
        base = "x0";
      } else if (base && !idx) {
        gen_expr(i);
        idx = "x0";
      } else if (!base && !idx) {
        gen_expr(p);
        push();
        gen_expr(i);
        base = pop_inreg("x1");
        idx = "x0";
      }
      a->base = base;
      a->index = idx;
      a->shift = shift;
      return;
    }
    const char *r = reg_var_x(node->m.lhs, a->buf[0]);
    if (r) {
      a->base = r;
      return;
    }
  }
  if (node->kind == ND_DEREF) {
    // p + i * s で p が単純（レジスタの変数、局所変数の配列・アドレス）なら、i を x0、p を x1 に置き、
    // 1 命令でアドレスにする（s が 2 のべき乗なら add のシフト、そうでなければ madd）
    Node *p, *i;
    int64_t scale;
    char pb[8];
    if (split_ptr_add(node->m.lhs, &p, &i, &scale) && scale > 0 &&
        (reg_var_x(p, pb) || (is_simple_gp_operand(p) && p->kind != ND_NUM))) {
      gen_expr(i);
      const char *base = reg_var_x(p, a->buf[0]);
      if (!base) {
        load_simple_gp("x1", p);
        base = "x1";
      }
      if (!(scale & (scale - 1)) && scale <= 16) {
        int sh = 0;
        while ((1 << sh) != scale)
          sh++;
        Printftn("add x0, %s, x0, lsl #%d", base, sh);
      } else {
        mov_imm("x16", scale);
        Printftn("madd x0, x0, x16, %s", base);
      }
      a->base = "x0";
      return;
    }
    if (access == 0) {
      const char *r = reg_var_x(node->m.lhs, a->buf[0]);
      if (r) {
        a->base = r;
        return;
      }
    }
    gen_expr(node->m.lhs);
    a->base = "x0";
    return;
  }
  gen_addr(node);
  a->base = "x0";
}

// node（is_plain_access）の値を、a のアドレスから x0（浮動小数点は v0）に読む
static void load_addr(Node *node, Addr *a) {
  Type *ty = node->ty;
  if (!a->index) {
    load_s(ty, a->ofs, a->base);
    return;
  }
  char m[64];
  if (a->index[0] == 'w')
    snprintf(m, sizeof(m), "[%s, %s, sxtw #%d]", a->base, a->index, a->shift);
  else
    snprintf(m, sizeof(m), "[%s, %s, lsl #%d]", a->base, a->index, a->shift);
  if (is_flonum(ty))
    Printftn("ldr %s, %s", freg(0, ty->size), m);
  else
    Printftn("%s %s, %s", ld_ins(ty->size, ty->is_unsigned), gp_of("x0", ty->size), m);
}

static void load(Node *node) {
  Type *ty = node->ty;
  if (is_scalar(ty)) {
    if (is_bitfield(node)) {
      gen_bitfield_load(node);
      return;
    }
    // 原子的な変数の読み出し（seq_cst）は ldar で行う（LSE 命令は使わない）
    if ((ty->qual & Q_ATOMIC) && ty->kind != TY_BITINT && ty->kind != TY_LDOUBLE) {
      static const char *const ldar[] = {[1] = "ldarb", [2] = "ldarh", [4] = "ldar", [8] = "ldar"};
      if (is_flonum(ty)) {
        Printftn("ldar %s, [x0]", ty->size == 8 ? "x16" : "w16");
        Printftn("fmov %s, %s", freg(0, ty->size), ty->size == 8 ? "x16" : "w16");
        return;
      }
      Printftn("%s %s, [x0]", ldar[ty->size], gp_of("x0", ty->size));
      if (!ty->is_unsigned && ty->size < 4)
        Printftn("%s w0, w0", ty->size == 1 ? "sxtb" : "sxth");
      return;
    }
    load_s(ty, 0, "x0");
    return;
  }

  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:    return;
  }
  internal_error();
}

static void store(Node *node, bool is_void) {
  if (is_bitfield(node)) {
    gen_bitfield_store(node, is_void);
    return;
  }
  const char *reg = pop_inreg("x1");
  store_s(node->ty, 0, reg);
}

//
// 型変換
//

static void gen_cmp_zero(Node *node, NodeKind kind) {
  if (node->ty->kind == TY_BITINT) {
    gen_expr(node);
    mov_imm("w0", node->ty->bit_cnt);
    Printstn("mov x1, x20");
    gen_bitint_builtin_call2("__slimcc_bitint_to_bool");
    if (kind == ND_EQ)
      Printstn("eor w0, w0, #1");
    return;
  }
  Node zero = {.kind = ND_NUM, .ty = node->ty, .tok = node->tok};
  Node expr = {.kind = kind, .m.lhs = node, .m.rhs = &zero, .tok = node->tok};
  add_type(&expr);
  gen_expr(&expr);
}

static void gen_var_assign(Obj *var, Node *expr) {
  Node var_node = {.kind = ND_VAR, .m.var = var, .tok = expr->tok};
  Node node = {.kind = ND_ASSIGN, .m.lhs = &var_node, .m.rhs = expr, .tok = expr->tok};
  add_type(&node);
  gen_void_expr(&node);
}

static void gen_expr_null_lhs(NodeKind kind, Type *ty, Node *rhs) {
  Node null = {.kind = ND_NULL_EXPR, .ty = ty, .tok = rhs->tok};
  Node expr = {.kind = kind, .m.lhs = &null, .m.rhs = rhs, .tok = rhs->tok};
  add_type(&expr);
  gen_expr(new_cast(&expr, ty));
}

static void gen_cast_to_bitint(int32_t width, Node *node) {
  Node expr = *node;
  gen_expr(new_cast(&expr, new_bitint(width, node->tok)));
}

enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, F128 };

static int getTypeId(Type *ty) {
  switch (ty->kind) {
  case TY_PCHAR:
  case TY_CHAR:     return ty->is_unsigned ? U8 : I8;
  case TY_SHORT:    return ty->is_unsigned ? U16 : I16;
  case TY_INT:      return ty->is_unsigned ? U32 : I32;
  case TY_LONG:
  case TY_LONGLONG: return ty->is_unsigned ? U64 : I64;
  case TY_FLOAT:    return F32;
  case TY_DOUBLE:   return F64;
  case TY_LDOUBLE:  return F128;
  }
  return U64;
}

// 整数どうしの変換。値は w0/x0（1・2 バイトの型は 32 ビットに拡張済み）
static void cast_int(int from, int to) {
  switch (to) {
  case I8:
    if (from != I8)
      Printstn("sxtb w0, w0");
    return;
  case U8:
    if (from != U8)
      Printstn("uxtb w0, w0");
    return;
  case I16:
    if (from != I8 && from != U8 && from != I16)
      Printstn("sxth w0, w0");
    return;
  case U16:
    if (from != U8 && from != U16)
      Printstn("uxth w0, w0");
    return;
  case I32:
  case U32:
    return;
  case I64:
  case U64:
    if (from == I8 || from == I16 || from == I32)
      Printstn("sxtw x0, w0");
    else if (from == U8 || from == U16 || from == U32)
      Printstn("mov w0, w0");
    return;
  }
  internal_error();
}

// long double（IEEE binary128）との変換は libgcc の関数を呼ぶ
static void cast_to_f128(int from) {
  switch (from) {
  case I8:
  case I16:
  case I32: gen_libcall("__floatsitf"); return;
  case U8:
  case U16:
  case U32: gen_libcall("__floatunsitf"); return;
  case I64: gen_libcall("__floatditf"); return;
  case U64: gen_libcall("__floatunditf"); return;
  case F32: gen_libcall("__extendsftf2"); return;
  case F64: gen_libcall("__extenddftf2"); return;
  }
  internal_error();
}

static void cast_from_f128(int to) {
  switch (to) {
  case I8:
  case I16:
  case I32:
  case U8:
  case U16:
    gen_libcall("__fixtfsi");
    cast_int(I32, to);
    return;
  case U32: gen_libcall("__fixunstfsi"); return;
  case I64: gen_libcall("__fixtfdi"); return;
  case U64: gen_libcall("__fixunstfdi"); return;
  case F32: gen_libcall("__trunctfsf2"); return;
  case F64: gen_libcall("__trunctfdf2"); return;
  }
  internal_error();
}

static void gen_cast(Node *node) {
  if (node->ty->kind == TY_BOOL) {
    gen_cmp_zero(node->m.lhs, ND_NE);
    return;
  }

  // レジスタに置いた 4 バイトの整数の変数を 8 バイトの整数・ポインタにする（配列の添字など）
  {
    Node *l = node->m.lhs;
    if (l->kind == ND_VAR && l->m.var->reg && l->m.var->reg < REG_FP_BASE && l->ty->size == 4 &&
        is_integer(l->ty) && (is_integer(node->ty) || is_ptr(node->ty)) && node->ty->size == 8 &&
        node->ty->kind != TY_BITINT && is_direct_local(l)) {
      if (l->ty->is_unsigned)
        Printftn("mov w0, w%d", l->m.var->reg);
      else
        Printftn("sxtw x0, w%d", l->m.var->reg);
      return;
    }
  }
  gen_expr(node->m.lhs);

  Type *from_ty = node->m.lhs->ty;
  Type *to_ty = node->ty;

  if (to_ty->kind == TY_VOID)
    return;

  if (to_ty->kind == TY_BITINT) {
    tmpbuf(to_ty->size);
    int64_t from_sz;
    if (from_ty->kind == TY_BITINT) {
      if (from_ty->bit_cnt >= to_ty->bit_cnt)
        return;
      from_sz = from_ty->bit_cnt;
    } else if (is_gp_ty(from_ty)) {
      Printstn("str x0, [x20]");
      if (from_ty->size * 8 >= to_ty->bit_cnt)
        return;
      from_sz = from_ty->size * 8;
    } else if (is_flonum(from_ty)) {
      // 浮動小数点 → _BitInt（bitint_builtins の __slimcc_bitint_from_f64。d0 に double。2026-11-20）
      if (from_ty->kind == TY_FLOAT)
        Printstn("fcvt d0, s0");
      else if (from_ty->kind == TY_LDOUBLE)
        cast_from_f128(F64);
      mov_imm("w0", to_ty->bit_cnt);
      Printstn("mov x1, x20");
      mov_imm("w2", to_ty->is_unsigned);
      gen_bitint_builtin_call2("__slimcc_bitint_from_f64");
      return;
    } else {
      error_tok(node->tok, "Unimplemented cast to _BitInt");
    }
    mov_imm("w0", from_sz);
    Printstn("mov x1, x20");
    mov_imm("w2", to_ty->bit_cnt);
    mov_imm("w3", from_ty->is_unsigned);
    gen_bitint_builtin_call2("__slimcc_bitint_sign_ext");
    return;
  }

  if (from_ty->kind == TY_BITINT && is_flonum(to_ty)) {
    // _BitInt → 浮動小数点（bitint_builtins。結果は s0・d0。long double は double から。2026-11-20）
    mov_imm("w0", from_ty->bit_cnt);
    Printstn("mov x1, x20");
    mov_imm("w2", from_ty->is_unsigned);
    gen_bitint_builtin_call2(to_ty->kind == TY_FLOAT ? "__slimcc_bitint_to_f32" : "__slimcc_bitint_to_f64");
    if (to_ty->kind == TY_LDOUBLE)
      cast_to_f128(F64);
    return;
  }

  if (from_ty->kind == TY_BITINT) {
    if (!is_gp_ty(to_ty))
      error_tok(node->tok, "Unimplemented cast from _BitInt");
    Printstn("ldr x0, [x20]");
    int64_t shft = 64 - from_ty->bit_cnt;
    if (shft > 0) {
      Printftn("lsl x0, x0, #%d", (int)shft);
      Printftn("%s x0, x0, #%d", from_ty->is_unsigned ? "lsr" : "asr", (int)shft);
    }
    return;
  }

  int t1 = getTypeId(from_ty);
  int t2 = getTypeId(to_ty);

  if (t1 == t2)
    return;
  if (t2 == F128) {
    cast_to_f128(t1);
    return;
  }
  if (t1 == F128) {
    cast_from_f128(t2);
    return;
  }

  bool from_fp = (t1 == F32 || t1 == F64);
  bool to_fp = (t2 == F32 || t2 == F64);

  if (from_fp && to_fp) {
    Printftn("%s", t2 == F64 ? "fcvt d0, s0" : "fcvt s0, d0");
    return;
  }
  if (to_fp) {
    const char *f = t2 == F64 ? "d0" : "s0";
    switch (t1) {
    case I8:
    case I16:
    case I32: Printftn("scvtf %s, w0", f); return;
    case U8:
    case U16:
    case U32: Printftn("ucvtf %s, w0", f); return;
    case I64: Printftn("scvtf %s, x0", f); return;
    case U64: Printftn("ucvtf %s, x0", f); return;
    }
    internal_error();
  }
  if (from_fp) {
    const char *f = t1 == F64 ? "d0" : "s0";
    switch (t2) {
    case I8:
    case I16:
    case I32:
    case U8:
    case U16:
      Printftn("fcvtzs w0, %s", f);
      cast_int(I32, t2);
      return;
    case U32: Printftn("fcvtzu w0, %s", f); return;
    case I64: Printftn("fcvtzs x0, %s", f); return;
    case U64: Printftn("fcvtzu x0, %s", f); return;
    }
    internal_error();
  }
  cast_int(t1, t2);
}

//
// 比較と条件分岐
//

static bool is_cmp(Node *node) {
  switch (node->kind) {
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE: return true;
  }
  return false;
}

static bool is_int_arith(Node *node) {
  switch (node->kind) {
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_REM:
  case ND_NEG:
  case ND_BITNOT:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
  case ND_SAR:    return true;
  }
  return false;
}

static bool is_fp_arith(Node *node) {
  switch (node->kind) {
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_NEG: return true;
  }
  return false;
}

static NodeKind flip_cmp(NodeKind kind) {
  switch (kind) {
  case ND_EQ: return ND_NE;
  case ND_NE: return ND_EQ;
  case ND_LT: return ND_GE;
  case ND_LE: return ND_GT;
  case ND_GT: return ND_LE;
  case ND_GE: return ND_LT;
  }
  internal_error();
}

// 整数の比較の条件コード
static const char *cmp_cc(NodeKind kind, bool is_unsigned) {
  switch (kind) {
  case ND_EQ: return "eq";
  case ND_NE: return "ne";
  case ND_LT: return is_unsigned ? "lo" : "lt";
  case ND_LE: return is_unsigned ? "ls" : "le";
  case ND_GT: return is_unsigned ? "hi" : "gt";
  case ND_GE: return is_unsigned ? "hs" : "ge";
  }
  internal_error();
}

// 浮動小数点の比較（fcmp）の条件コード。NaN（順序なし）では EQ〜GE が偽、NE が真になる
static const char *fcmp_cc(NodeKind kind) {
  switch (kind) {
  case ND_EQ: return "eq";
  case ND_NE: return "ne";
  case ND_LT: return "mi";
  case ND_LE: return "ls";
  case ND_GT: return "gt";
  case ND_GE: return "ge";
  }
  internal_error();
}

// 条件式を簡単にする（x86-64 版と同じ）。! を数えて flip に、
// 「(副作用のある式) && 0」のように結果が定数になるときは is_void を立てて、評価だけする式を返す
static Node *bool_expr_opt(Node *node, bool *flip, bool *is_void) {
  Node *boolexpr = NULL;
  bool has_not = false;
  *is_void = false;
  bool boolexpr_has_not = false;

  for (;;) {
    switch (node->kind) {
    case ND_NOT:
      has_not = !has_not;
      node = node->m.lhs;
      continue;
    case ND_LOGOR:
    case ND_LOGAND: {
      int64_t val;
      if (is_const_expr(node->m.rhs, &val)) {
        if (val == (node->kind == ND_LOGAND)) {
          node = node->m.lhs;
          continue;
        }
        *is_void = true;
        *flip = (node->kind == ND_LOGOR) ^ has_not;
        return node->m.lhs;
      }
    }
      // fallthrough
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_GT:
    case ND_GE:
      *flip = has_not;
      return node;
    }
    if (node->ty->kind == TY_BOOL) {
      boolexpr = node;
      boolexpr_has_not = has_not;
    }
    if (!(node->kind == ND_CAST && is_gp_ty(node->ty)))
      break;
    node = node->m.lhs;
  }
  if (boolexpr) {
    *flip = boolexpr_has_not;
    return boolexpr;
  }
  return NULL;
}

// 左辺の値を置くレジスタ。レジスタに置いた変数ならそのレジスタ（コピーしない）、
// それ以外は x0 に計算する。buf は名前を入れる場所
static const char *gen_lhs_operand(Node *lhs, int sz, char *buf) {
  if (lhs->kind == ND_VAR && lhs->m.var->reg && lhs->m.var->reg < REG_FP_BASE &&
      is_direct_local(lhs)) {
    snprintf(buf, 8, "%s", var_reg_name(lhs->m.var, sz));
    return buf;
  }
  gen_expr(lhs);
  return gp_of("x0", sz);
}

static void gen_cond(Node *node, bool jump_cond, const char *jump_label) {
  {
    int64_t val;
    if (is_const_expr(node, &val)) {
      if (val == jump_cond)
        Printftn("b %s", jump_label);
      return;
    }
    bool flip, is_void;
    Node *expr = bool_expr_opt(node, &flip, &is_void);
    if (expr) {
      if (is_void) {
        gen_void_expr(expr);
        if (flip == jump_cond)
          Printftn("b %s", jump_label);
        return;
      }
      node = expr;
      jump_cond ^= flip;
    }
  }

  if (is_cmp(node) && is_gp_ty(node->m.lhs->ty) && is_simple_gp_operand(node->m.rhs)) {
    int sz = node->m.lhs->ty->size == 8 || node->m.lhs->ty->base ? 8 : 4;
    char lbuf[8];
    const char *l = gen_lhs_operand(node->m.lhs, sz, lbuf);
    int64_t imm;
    if (is_imm12_operand(node->m.rhs, &imm)) {
      Printftn("cmp %s, #%" PRIi64, l, imm);
    } else if (node->m.rhs->kind == ND_VAR && node->m.rhs->m.var->reg) {
      Printftn("cmp %s, %s", l, var_reg_name(node->m.rhs->m.var, sz));
    } else {
      load_simple_gp("x1", node->m.rhs);
      Printftn("cmp %s, %s", l, gp_of("x1", sz));
    }
    NodeKind kind = jump_cond ? node->kind : flip_cmp(node->kind);
    Printftn("b.%s %s", cmp_cc(kind, node->m.lhs->ty->is_unsigned), jump_label);
    return;
  }

  if (is_cmp(node) && is_gp_ty(node->m.lhs->ty)) {
    gen_expr(node->m.lhs);
    push();
    gen_expr(node->m.rhs);
    int sz = node->m.lhs->ty->size == 8 || node->m.lhs->ty->base ? 8 : 4;
    const char *op = pop_inreg("x1");
    Printftn("cmp %s, %s", gp_of(op, sz), gp_of("x0", sz));
    NodeKind kind = jump_cond ? node->kind : flip_cmp(node->kind);
    Printftn("b.%s %s", cmp_cc(kind, node->m.lhs->ty->is_unsigned), jump_label);
    return;
  }

  if (node->kind == ND_LOGAND || node->kind == ND_LOGOR) {
    bool short_cond = (node->kind == ND_LOGOR);
    if (short_cond == jump_cond) {
      gen_cond(node->m.lhs, jump_cond, jump_label);
      gen_cond(node->m.rhs, jump_cond, jump_label);
      return;
    }
    char short_label[STRBUF_SZ];
    snprintf(short_label, STRBUF_SZ, ".L.short.%" PRIi64, count());

    gen_cond(node->m.lhs, short_cond, short_label);
    gen_cond(node->m.rhs, short_cond, short_label);
    Printftn("b %s", jump_label);
    Printfsn("%s:", short_label);
    return;
  }

  gen_expr(node);
  // 条件は bool（0 か 1）。関数が返した bool の上位ビットは不定なので下位 8 ビットだけを見る
  Printstn("tst w0, #0xff");
  Printftn("b.%s %s", jump_cond ? "ne" : "eq", jump_label);
}

static void gen_logical(Node *node, bool flip) {
  int64_t c = count();
  char short_label[STRBUF_SZ];
  snprintf(short_label, STRBUF_SZ, ".L.short.%" PRIi64, c);
  bool short_cond = (node->kind == ND_LOGOR);

  gen_cond(node->m.lhs, short_cond, short_label);
  gen_cond(node->m.rhs, short_cond, short_label);

  short_cond ^= flip;
  Printftn("mov w0, #%d", short_cond ? 0 : 1);
  Printftn("b .L.fall.%" PRIi64, c);
  Printfsn("%s:", short_label);
  Printftn("mov w0, #%d", short_cond ? 1 : 0);
  Printfsn(".L.fall.%" PRIi64 ":", c);
}

//
// 算術演算
//

static void gen_fp_arith(Node *node) {
  int sz = node->m.lhs->ty->size;
  const char *f0 = freg(0, sz);

  if (node->kind == ND_NEG) {
    gen_expr(node->m.lhs);
    Printftn("fneg %s, %s", f0, f0);
    return;
  }

  // 右辺が直接読める局所変数なら、左辺を d0（s0）に、右辺を d1（s1）に読み、一時スタックを使わない
  if (is_direct_local(node->m.rhs) && is_flonum(node->m.rhs->ty) &&
      node->m.rhs->ty->size == sz) {
    gen_expr(node->m.lhs);
    char buf[STRBUF_SZ];
    Obj *var = node->m.rhs->m.var;
    char f1buf[8];
    const char *f1 = freg(1, sz);
    if (var->reg) {
      snprintf(f1buf, sizeof(f1buf), "%s", var_reg_name(var, sz));
      f1 = f1buf;
    } else {
      Printftn("ldr %s, %s", f1, mem_op(buf, var->ofs, var->ptr, sz, "x17"));
    }
    switch (node->kind) {
    case ND_ADD: Printftn("fadd %s, %s, %s", f0, f0, f1); return;
    case ND_SUB: Printftn("fsub %s, %s, %s", f0, f0, f1); return;
    case ND_MUL: Printftn("fmul %s, %s, %s", f0, f0, f1); return;
    case ND_DIV: Printftn("fdiv %s, %s, %s", f0, f0, f1); return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_GT:
    case ND_GE:
      Printftn("fcmp %s, %s", f0, f1);
      Printftn("cset w0, %s", fcmp_cc(node->kind));
      return;
    }
    internal_error();
  }

  gen_expr(node->m.lhs);
  pushf();
  gen_expr(node->m.rhs);
  int r = popf_inreg(1);
  const char *fl = freg(r, sz);

  switch (node->kind) {
  case ND_ADD: Printftn("fadd %s, %s, %s", f0, fl, f0); return;
  case ND_SUB: Printftn("fsub %s, %s, %s", f0, fl, f0); return;
  case ND_MUL: Printftn("fmul %s, %s, %s", f0, fl, f0); return;
  case ND_DIV: Printftn("fdiv %s, %s, %s", f0, fl, f0); return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    Printftn("fcmp %s, %s", fl, f0);
    Printftn("cset w0, %s", fcmp_cc(node->kind));
    return;
  }
  internal_error();
}

// long double の演算: 左辺を q0、右辺を q1 にして libgcc の関数を呼ぶ
static void gen_f128_arith(Node *node) {
  if (node->kind == ND_NEG) {
    gen_expr(node->m.lhs);
    Printstn("mov x16, v0.d[1]");
    Printstn("eor x16, x16, #0x8000000000000000");
    Printstn("mov v0.d[1], x16");
    return;
  }

  gen_expr(node->m.lhs);
  pushf();
  gen_expr(node->m.rhs);
  popf(2);
  Printstn("mov v1.16b, v0.16b");
  Printstn("mov v0.16b, v2.16b");

  switch (node->kind) {
  case ND_ADD: gen_libcall("__addtf3"); return;
  case ND_SUB: gen_libcall("__subtf3"); return;
  case ND_MUL: gen_libcall("__multf3"); return;
  case ND_DIV: gen_libcall("__divtf3"); return;
  case ND_EQ:
    gen_libcall("__eqtf2");
    Printstn("cmp w0, #0");
    Printstn("cset w0, eq");
    return;
  case ND_NE:
    gen_libcall("__netf2");
    Printstn("cmp w0, #0");
    Printstn("cset w0, ne");
    return;
  case ND_LT:
    gen_libcall("__lttf2");
    Printstn("cmp w0, #0");
    Printstn("cset w0, lt");
    return;
  case ND_LE:
    gen_libcall("__letf2");
    Printstn("cmp w0, #0");
    Printstn("cset w0, le");
    return;
  case ND_GT:
    gen_libcall("__gttf2");
    Printstn("cmp w0, #0");
    Printstn("cset w0, gt");
    return;
  case ND_GE:
    gen_libcall("__getf2");
    Printstn("cmp w0, #0");
    Printstn("cset w0, ge");
    return;
  }
  internal_error();
}

static void gen_bitint_arith(Node *node) {
  Type *ty = node->m.lhs->ty;
  tmpbuf(ty->size);

  switch (node->kind) {
  case ND_NEG:
  case ND_BITNOT:
    gen_expr(node->m.lhs);
    mov_imm("w0", ty->bit_cnt);
    Printstn("mov x1, x20");
    gen_bitint_builtin_call(node->kind);
    return;
  }

  gen_expr(node->m.lhs);
  push_bitint(ty->size);
  gen_expr(node->m.rhs);

  // シフト量（右辺）は整数として w0 にある
  if (node->kind == ND_SHL || node->kind == ND_SHR || node->kind == ND_SAR)
    Printstn("mov w3, w0");

  int pos = pop_bitint(ty->size);
  mov_imm("w0", ty->bit_cnt);
  add_imm("x1", lvar_ptr, pos);
  Printstn("mov x2, x20");

  switch (node->kind) {
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
    gen_bitint_builtin_call(node->kind);
    return;
  case ND_DIV:
  case ND_REM:
    mov_imm("w3", ty->is_unsigned);
    mov_imm("w4", node->kind == ND_DIV);
    gen_bitint_builtin_call(node->kind);
    return;
  case ND_SHL:
  case ND_SHR:
  case ND_SAR:
    if (node->kind != ND_SHL)
      mov_imm("w4", ty->is_unsigned);
    gen_bitint_builtin_call(node->kind);
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE: {
    mov_imm("w3", ty->is_unsigned);
    gen_bitint_builtin_call2("__slimcc_bitint_cmp");

    // __slimcc_bitint_cmp の戻り値: 0 = 等しい、1 = 左辺が小さい、2 = 左辺が大きい
    NodeKind kind;
    int val;
    switch (node->kind) {
    case ND_EQ: val = 0, kind = ND_EQ; break;
    case ND_NE: val = 0, kind = ND_NE; break;
    case ND_LT: val = 1, kind = ND_EQ; break;
    case ND_LE: val = 2, kind = ND_NE; break;
    case ND_GT: val = 2, kind = ND_EQ; break;
    case ND_GE: val = 1, kind = ND_NE; break;
    default:    internal_error();
    }
    Printftn("cmp w0, #%d", val);
    Printftn("cset w0, %s", cmp_cc(kind, false));
    return;
  }
  }
  internal_error();
}

static void gen_gp_arith(Node *node) {
  int sz = node->ty->size == 8 || node->ty->base ? 8 : 4;
  const char *a = gp_of("x0", sz);

  switch (node->kind) {
  case ND_NEG:
    gen_expr(node->m.lhs);
    Printftn("neg %s, %s", a, a);
    return;
  case ND_BITNOT:
    gen_expr(node->m.lhs);
    Printstn("mvn x0, x0");
    return;
  }

  // 比較は左辺の型の大きさで行う
  int opsz = is_cmp(node) ? (node->m.lhs->ty->size == 8 || node->m.lhs->ty->base ? 8 : 4) : sz;

  // 交換できる演算で左辺だけが単純なら、左右を入れ替える
  Node swapped;
  switch (node->kind) {
  case ND_ADD:
  case ND_MUL:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_EQ:
  case ND_NE:
    if (is_simple_gp_operand(node->m.lhs) && !is_simple_gp_operand(node->m.rhs) &&
        node->m.lhs->ty->size == node->m.rhs->ty->size) {
      swapped = *node;
      swapped.m.lhs = node->m.rhs;
      swapped.m.rhs = node->m.lhs;
      node = &swapped;
    }
  }

  // 右辺が単純なら、左辺を x0 に、右辺を x1 に（または即値で）置き、一時スタックを使わない
  if (is_simple_gp_operand(node->m.rhs)) {
    char lbuf[8];
    const char *ls = gen_lhs_operand(node->m.lhs, opsz, lbuf);
    a = gp_of("x0", opsz);
    int64_t imm;
    bool small = is_imm12_operand(node->m.rhs, &imm);
    char immbuf[32];
    snprintf(immbuf, sizeof(immbuf), "#%" PRIi64, imm);
    const char *r = immbuf;
    switch (node->kind) {
    case ND_ADD:
    case ND_SUB:
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_GT:
    case ND_GE:
      break;
    case ND_SHL:
    case ND_SHR:
    case ND_SAR:
      small = small && imm < opsz * 8;
      break;
    default:
      small = false;
    }
    // 2 のべき乗の掛け算はシフトにする
    if (node->kind == ND_MUL && node->m.rhs->kind == ND_NUM) {
      uint64_t v = opsz == 8 ? (uint64_t)node->m.rhs->num.val : (uint32_t)node->m.rhs->num.val;
      if (v && !(v & (v - 1))) {
        int sh = 0;
        while ((1ULL << sh) != v)
          sh++;
        if (sh)
          Printftn("lsl %s, %s, #%d", a, ls, sh);
        else if (strcmp(ls, a))
          Printftn("mov %s, %s", a, ls);
        return;
      }
    }
    char regbuf[8];
    if (!small && node->m.rhs->kind == ND_VAR && node->m.rhs->m.var->reg) {
      // レジスタに置いた変数は、そのまま使う
      snprintf(regbuf, sizeof(regbuf), "%s", var_reg_name(node->m.rhs->m.var, opsz));
      r = regbuf;
    } else if (!small) {
      load_simple_gp("x1", node->m.rhs);
      r = gp_of("x1", opsz);
    }
    bool is_unsigned = node->ty->is_unsigned;
    switch (node->kind) {
    case ND_ADD:    Printftn("add %s, %s, %s", a, ls, r); return;
    case ND_SUB:    Printftn("sub %s, %s, %s", a, ls, r); return;
    case ND_MUL:    Printftn("mul %s, %s, %s", a, ls, r); return;
    case ND_DIV:    Printftn("%s %s, %s, %s", is_unsigned ? "udiv" : "sdiv", a, ls, r); return;
    case ND_REM: {
      const char *t = gp_of("x16", opsz);
      Printftn("%s %s, %s, %s", is_unsigned ? "udiv" : "sdiv", t, ls, r);
      Printftn("msub %s, %s, %s, %s", a, t, r, ls);
      return;
    }
    case ND_BITAND: Printftn("and %s, %s, %s", a, ls, r); return;
    case ND_BITOR:  Printftn("orr %s, %s, %s", a, ls, r); return;
    case ND_BITXOR: Printftn("eor %s, %s, %s", a, ls, r); return;
    case ND_SHL:    Printftn("lsl %s, %s, %s", a, ls, r); return;
    case ND_SHR:    Printftn("lsr %s, %s, %s", a, ls, r); return;
    case ND_SAR:    Printftn("asr %s, %s, %s", a, ls, r); return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_GT:
    case ND_GE:
      Printftn("cmp %s, %s", ls, r);
      Printftn("cset w0, %s", cmp_cc(node->kind, node->m.lhs->ty->is_unsigned));
      return;
    }
    internal_error();
  }

  gen_expr(node->m.lhs);
  push();
  gen_expr(node->m.rhs);

  sz = opsz;
  a = gp_of("x0", sz);
  const char *l = gp_of(pop_inreg("x1"), sz);
  bool is_unsigned = node->ty->is_unsigned;

  switch (node->kind) {
  case ND_ADD:    Printftn("add %s, %s, %s", a, l, a); return;
  case ND_SUB:    Printftn("sub %s, %s, %s", a, l, a); return;
  case ND_MUL:    Printftn("mul %s, %s, %s", a, l, a); return;
  case ND_DIV:    Printftn("%s %s, %s, %s", is_unsigned ? "udiv" : "sdiv", a, l, a); return;
  case ND_REM: {
    const char *t = gp_of("x16", sz);
    Printftn("%s %s, %s, %s", is_unsigned ? "udiv" : "sdiv", t, l, a);
    Printftn("msub %s, %s, %s, %s", a, t, a, l);
    return;
  }
  case ND_BITAND: Printftn("and %s, %s, %s", a, l, a); return;
  case ND_BITOR:  Printftn("orr %s, %s, %s", a, l, a); return;
  case ND_BITXOR: Printftn("eor %s, %s, %s", a, l, a); return;
  case ND_SHL:    Printftn("lsl %s, %s, %s", a, l, a); return;
  case ND_SHR:    Printftn("lsr %s, %s, %s", a, l, a); return;
  case ND_SAR:    Printftn("asr %s, %s, %s", a, l, a); return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    Printftn("cmp %s, %s", l, a);
    Printftn("cset w0, %s", cmp_cc(node->kind, node->m.lhs->ty->is_unsigned));
    return;
  }
  internal_error();
}

//
// 関数呼び出し
//

// [base + ofs] にある値を、AAPCS64 の割り当てどおりにレジスタへ読み込む
static void place_reg_arg(Obj *var) {
  Type *ty = var->ty;
  ArgInfo ai = classify(ty);
  pin_var(var);
  const char *base = var->ptr;
  int64_t ofs = var->ofs;
  char buf[STRBUF_SZ];

  if (var->pass_by_ref) {
    if (var->arg_reg >= 0) {
      add_imm(xreg[var->arg_reg], base, ofs);
    } else {
      add_imm("x16", base, ofs);
      Printftn("str x16, %s", mem_op(buf, var->stack_offset, "sp", 8, "x17"));
    }
    return;
  }

  switch (ai.cls) {
  case AC_FP:
    Printftn("ldr %s, %s", freg(var->arg_reg, ty->size), mem_op(buf, ofs, base, ty->size, "x17"));
    return;
  case AC_HFA:
    add_imm("x16", base, ofs);
    for (int i = 0; i < ai.nreg; i++)
      Printftn("ldr %s, [x16, #%d]", freg(var->arg_reg + i, ai.elem_sz), i * ai.elem_sz);
    return;
  case AC_GP:
    if (is_gp_ty(ty)) {
      Printftn("%s %s, %s", ld_ins(ty->size, ty->is_unsigned),
               gp_of(xreg[var->arg_reg], ty->size), mem_op(buf, ofs, base, ty->size, "x17"));
      return;
    }
    // 構造体、_BitInt: 8 バイトずつ（端数はちょうどの大きさで）読む
    add_imm("x16", base, ofs);
    for (int i = 0; i < ai.nreg; i++)
      load_gp_exact(xreg[var->arg_reg + i], "x16", i * 8, MIN(8, ty->size - i * 8));
    return;
  }
  internal_error();
}

// 引数の式を、それぞれの一時変数（スタック渡しなら呼び出し時のスタック上）に評価する
static void gen_funcall_stack_args(Obj *var) {
  for (; var; var = var->param_next) {
    if (var->is_zero_sized_arg)
      gen_void_expr(var->arg_expr);
    else if (var->ptr)
      gen_var_assign(var, var->arg_expr);
  }
}

static void gen_funcall_args(Node *node) {
  for (Obj *var = node->call.args; var; var = var->param_next) {
    if (var->is_zero_sized_arg || var->pass_by_stack)
      continue;
    place_reg_arg(var);
  }
}

static bool is_setjmp_call(Node *fn) {
  if (fn->kind != ND_VAR)
    return false;
  const char *n = fn->m.var->name;
  if (!n)
    return false;
  // GCC と同じく、前の _ を除いて決まった名前と比べる（前は名前に setjmp を含むかで見ていて、
  // is_setjmp_call を呼ぶ ir_funcall なども setjmp を呼ぶ関数とみなし、今までのコード生成にしていた）
  while (*n == '_')
    n++;
  return !strcmp(n, "setjmp") || !strcmp(n, "sigsetjmp") || !strcmp(n, "savectx") ||
         !strcmp(n, "vfork") || !strcmp(n, "getcontext") || !strcmp(n, "builtin_setjmp");
}

static void gen_funcall(Node *node) {
  if (reg_analysis && is_setjmp_call(node->call.expr))
    reg_disable = true;
  int gp = 0, fp = 0;
  int arg_stk_size = calling_convention(node->call.args, &gp, &fp);
  int64_t stk = align_to(arg_stk_size, 16);

  bool use_fn_ptr = !is_direct_fncall(node->call.expr);
  if (use_fn_ptr) {
    gen_expr(node->call.expr);
    push();
  }

  gen_funcall_stack_args(node->call.alloca_args);

  if (stk)
    add_imm("sp", "sp", -stk);

  gen_funcall_stack_args(node->call.args);
  gen_funcall_args(node);

  if (node->call.rtn_buf && is_mem_rtn(node->ty))
    add_imm("x8", node->call.rtn_buf->ptr, node->call.rtn_buf->ofs);

  if (use_fn_ptr) {
    Printftn("blr %s", pop_inreg("x17"));
  } else {
    Obj *fn = node->call.expr->m.var;
    if (use_direct(fn) || opt_use_plt) {
      Printftn("bl \"%s\"", get_symbol(fn));
    } else {
      Printftn("adrp x17, :got:\"%s\"", get_symbol(fn));
      Printftn("ldr x17, [x17, :got_lo12:\"%s\"]", get_symbol(fn));
      Printstn("blr x17");
    }
  }

  clobber_all_regs();

  if (stk)
    add_imm("sp", "sp", stk);
}

// レジスタで返された構造体を、戻り値の領域に書く
static void copy_ret_buffer(Obj *var) {
  Type *ty = var->ty;
  ArgInfo ai = classify(ty);
  pin_var(var);
  add_imm("x2", var->ptr, var->ofs);

  if (ai.cls == AC_HFA) {
    for (int i = 0; i < ai.nreg; i++)
      Printftn("str %s, [x2, #%d]", freg(i, ai.elem_sz), i * ai.elem_sz);
    return;
  }
  for (int i = 0; i < ai.nreg; i++)
    store_gp_exact(xreg[i], "x2", i * 8, MIN(8, ty->size - i * 8));
}

// return: [x0] にある構造体を、戻り値のレジスタに読み込む
static void copy_struct_reg(void) {
  Type *ty = codegen_fn->ty->return_ty;
  ArgInfo ai = classify(ty);

  if (ai.cls == AC_HFA) {
    for (int i = 0; i < ai.nreg; i++)
      Printftn("ldr %s, [x0, #%d]", freg(i, ai.elem_sz), i * ai.elem_sz);
    return;
  }
  Printstn("mov x16, x0");
  for (int i = 0; i < ai.nreg; i++)
    load_gp_exact(xreg[i], "x16", i * 8, MIN(8, ty->size - i * 8));
}

// return: [x0] にある構造体を、呼び出し元が x8 で渡した領域にコピーする
static void copy_struct_mem(void) {
  Type *ty = codegen_fn->ty->return_ty;
  char buf[STRBUF_SZ];
  Printftn("ldr x2, %s", mem_op(buf, -rtn_ptr_ofs, lvar_ptr, 8, "x17"));
  gen_mem_copy("x0", "x2", ty->size);
  Printstn("mov x0, x2");
}

//
// 可変長引数（AAPCS64 の「Appendix: Variable argument lists」）
//
// va_list: { void *__stack; void *__gr_top; void *__vr_top; int __gr_offs; int __vr_offs; }

#define VA_STACK 0
#define VA_GR_TOP 8
#define VA_VR_TOP 16
#define VA_GR_OFFS 24
#define VA_VR_OFFS 28

static void gen_va_start(void) {
  // x0 = &ap
  add_imm("x16", x29, va_stack_ofs);
  Printftn("str x16, [x0, #%d]", VA_STACK);
  add_imm("x16", lvar_ptr, va_gr_top_ofs);
  Printftn("str x16, [x0, #%d]", VA_GR_TOP);
  add_imm("x16", lvar_ptr, va_vr_top_ofs);
  Printftn("str x16, [x0, #%d]", VA_VR_TOP);
  mov_imm("w16", -(GP_MAX - MIN(va_gp_named, GP_MAX)) * 8);
  Printftn("str w16, [x0, #%d]", VA_GR_OFFS);
  mov_imm("w16", -(FP_MAX - MIN(va_fp_named, FP_MAX)) * 16);
  Printftn("str w16, [x0, #%d]", VA_VR_OFFS);
}

// x0 = &ap。引数のアドレスを x0 に返す
static void gen_va_arg(Node *node) {
  Type *ty = node->ty->base;
  ArgInfo ai = classify(ty);
  int64_t c = count();
  bool use_fp = (ai.cls == AC_FP || ai.cls == AC_HFA);
  bool align16 = ai.cls != AC_REF && ty->align >= 16;
  int nreg = ai.cls == AC_REF ? 1 : ai.nreg;

  int offs_pos = use_fp ? VA_VR_OFFS : VA_GR_OFFS;
  int top_pos = use_fp ? VA_VR_TOP : VA_GR_TOP;
  int step = use_fp ? 16 : 8;

  // レジスタ保存領域から取れるか
  Printftn("ldr w16, [x0, #%d]", offs_pos);
  Printftn("tbz w16, #31, .L.va_stack.%" PRIi64, c);
  if (align16 && !use_fp) {
    Printstn("add w16, w16, #15");
    Printstn("and w16, w16, #-16");
  }
  Printftn("add w17, w16, #%d", nreg * step);
  Printftn("str w17, [x0, #%d]", offs_pos);
  Printstn("cmp w17, #0");
  Printftn("b.gt .L.va_stack.%" PRIi64, c);
  Printftn("ldr x17, [x0, #%d]", top_pos);
  Printstn("add x1, x17, w16, sxtw");

  if (ai.cls == AC_HFA && ai.nreg > 1) {
    // HFA のメンバーは 16 バイトごとに置かれているので、局所変数に詰めてコピーする
    Obj *var = node->m.var;
    pin_var(var);
    add_imm("x2", var->ptr, var->ofs);
    for (int i = 0; i < ai.nreg; i++) {
      Printftn("ldr %s, [x1, #%d]", freg(2, ai.elem_sz), i * 16);
      Printftn("str %s, [x2, #%d]", freg(2, ai.elem_sz), i * ai.elem_sz);
    }
    Printstn("mov x1, x2");
  }
  Printftn("b .L.va_end.%" PRIi64, c);

  // スタックから取る
  Printfsn(".L.va_stack.%" PRIi64 ":", c);
  Printftn("ldr x1, [x0, #%d]", VA_STACK);
  if (align16) {
    Printstn("add x1, x1, #15");
    Printstn("and x1, x1, #-16");
  }
  int64_t stk_sz = ai.cls == AC_REF ? 8 : align_to(ty->size, 8);
  add_imm("x17", "x1", stk_sz);
  Printftn("str x17, [x0, #%d]", VA_STACK);

  Printfsn(".L.va_end.%" PRIi64 ":", c);
  if (ai.cls == AC_REF)
    Printstn("ldr x0, [x1]");
  else
    Printstn("mov x0, x1");
}

//
// alloca、defer
//

static void builtin_alloca(Node *node) {
  // x0 = 大きさ
  int align = node->m.var ? MAX(node->m.var->alt_align, 16) : 16;
  int64_t c = count();

  Printstn("mov x17, sp");
  Printstn("sub x16, x17, x0");
  Printftn("and x16, x16, #-%d", align);
  Printstn("mov sp, x16");

  if (node->kind == ND_ALLOCA_ZINIT) {
    Printfsn(".L.alloca.%" PRIi64 ":", c);
    Printstn("cmp x16, x17");
    Printftn("b.hs .L.alloca_end.%" PRIi64, c);
    Printstn("stp xzr, xzr, [x16], #16");
    Printftn("b .L.alloca.%" PRIi64, c);
  } else {
    // ガードページを飛び越えないよう、上から 4KB ごとに触る
    Printfsn(".L.alloca.%" PRIi64 ":", c);
    Printstn("sub x17, x17, #4096");
    Printstn("cmp x17, x16");
    Printftn("b.lo .L.alloca_end.%" PRIi64, c);
    Printstn("ldr xzr, [x17]");
    Printftn("b .L.alloca.%" PRIi64, c);
  }
  Printfsn(".L.alloca_end.%" PRIi64 ":", c);

  if (node->m.var) {
    char buf[STRBUF_SZ];
    Printstn("mov x16, sp");
    Printftn("str x16, %s", mem_op(buf, node->m.var->vptr->ofs, node->m.var->vptr->ptr, 8, "x17"));
  } else {
    Printstn("mov x0, sp");
  }
}

static void gen_defr(Node *node) {
  DeferStmt *defr = node->dfr_from;
  DeferStmt *end = node->dfr_dest;
  char buf[STRBUF_SZ];

  while (defr != end) {
    if (defr->kind == DF_VLA_DEALLOC) {
      while (defr->next != end && defr->next->kind == DF_VLA_DEALLOC)
        defr = defr->next;

      if (!codegen_fn->dealloc_vla) {
        defr = defr->next;
        continue;
      }

      Obj *vla = defr->next ? defr->next->vla : NULL;
      if (vla)
        Printftn("ldr x16, %s", mem_op(buf, vla->ofs, vla->ptr, 8, "x17"));
      else
        Printftn("ldr x16, %s", mem_op(buf, -vla_base_ofs, lvar_ptr, 8, "x17"));
      Printstn("mov sp, x16");

      defr = defr->next;
      continue;
    }
    if (defr->kind == DF_CLEANUP_FN) {
      gen_void_expr(defr->cleanup_fn);
      defr = defr->next;
      continue;
    }
    if (defr->kind == DF_DEFER_STMT) {
      gen_stmt(defr->stmt);
      defr = defr->next;
      continue;
    }
    internal_error();
  }
}

static void print_loc(Token *tok) {
  TLS static int file_no, line_no, id;

  if (file_no == tok->display_file_no && line_no == tok->display_line_no)
    return;

  line_no = tok->display_line_no;

  if (file_no != tok->display_file_no) {
    file_no = tok->display_file_no;
    if (!debug_file_used[file_no]) {
      debug_file_used[file_no] = true;
      debug_file_id[file_no] = id++;
      strarray_push(&debug_files, display_files.data[file_no]);
    }
  }
  Printftn(".loc %d %d", debug_file_id[file_no], line_no);
}

//
// 原子的な操作（LSE 命令は使わず、排他ロード・ストアのループで行う）
//

static const char *ldaxr_ins(int sz) {
  switch (sz) {
  case 1: return "ldaxrb";
  case 2: return "ldaxrh";
  case 4:
  case 8: return "ldaxr";
  }
  internal_error();
}

static const char *stlxr_ins(int sz) {
  switch (sz) {
  case 1: return "stlxrb";
  case 2: return "stlxrh";
  case 4:
  case 8: return "stlxr";
  }
  internal_error();
}

static void gen_cas(Node *node) {
  gen_expr(node->cas.addr);
  push();
  gen_expr(node->cas.old_val);
  push();
  gen_expr(node->cas.new_val);

  const char *old = pop_inreg("x2");
  const char *addr = pop_inreg("x3");

  Type *ty = node->cas.addr->ty->base;
  if (!(is_gp_ty(ty) || ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE))
    error_tok(node->tok, "unsupported type for atomic CAS");

  int sz = ty->size;
  if (ty->kind == TY_FLOAT)
    Printstn("fmov w0, s0");
  else if (ty->kind == TY_DOUBLE)
    Printstn("fmov x0, d0");

  const char *nv = gp_of("x0", sz);
  const char *ex = gp_of("x1", sz);
  const char *cur = gp_of("x16", sz);
  int64_t c = count();

  Printftn("%s %s, [%s]", ld_ins(sz, true), ex, old);
  Printfsn(".L.cas.%" PRIi64 ":", c);
  Printftn("%s %s, [%s]", ldaxr_ins(sz), cur, addr);
  Printftn("cmp %s, %s", cur, ex);
  Printftn("b.ne .L.cas_fail.%" PRIi64, c);
  Printftn("%s w17, %s, [%s]", stlxr_ins(sz), nv, addr);
  Printftn("cbnz w17, .L.cas.%" PRIi64, c);
  Printstn("mov w0, #1");
  Printftn("b .L.cas_end.%" PRIi64, c);
  Printfsn(".L.cas_fail.%" PRIi64 ":", c);
  Printstn("clrex");
  Printftn("%s %s, [%s]", st_ins(sz), cur, old);
  Printstn("mov w0, #0");
  Printfsn(".L.cas_end.%" PRIi64 ":", c);
}

static void gen_exch(Node *node, bool is_void) {
  gen_expr(node->m.lhs);
  push();
  gen_expr(node->m.rhs);
  const char *reg = pop_inreg("x1");

  Type *ty = node->ty;
  if (!(is_gp_ty(ty) || ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE))
    error_tok(node->tok, "unsupported type for atomic exchange");

  int sz = ty->size;
  if (ty->kind == TY_FLOAT)
    Printstn("fmov w0, s0");
  else if (ty->kind == TY_DOUBLE)
    Printstn("fmov x0, d0");

  int64_t c = count();
  Printfsn(".L.xchg.%" PRIi64 ":", c);
  Printftn("%s %s, [%s]", ldaxr_ins(sz), gp_of("x16", sz), reg);
  Printftn("%s w17, %s, [%s]", stlxr_ins(sz), gp_of("x0", sz), reg);
  Printftn("cbnz w17, .L.xchg.%" PRIi64, c);

  if (is_void)
    return;
  switch (ty->kind) {
  case TY_FLOAT:  Printstn("fmov s0, w16"); return;
  case TY_DOUBLE: Printstn("fmov d0, x16"); return;
  }
  Printstn("mov x0, x16");
  if (!ty->is_unsigned && sz < 4)
    Printftn("%s w0, w0", sz == 1 ? "sxtb" : "sxth");
}

//
// 式
//

// 値を使わない式のうち、副作用のない部分を省く（x86-64 版の gen_void_expr2 と同じ範囲）。
// 省けたら true
static bool gen_void_expr2(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
  case ND_LABEL_VAL:
  case ND_VAR:
  case ND_NUM:
    return true;
  case ND_POS:
  case ND_NEG:
  case ND_MEMBER:
  case ND_ADDR:
  case ND_DEREF:
  case ND_NOT:
  case ND_BITNOT:
  case ND_CAST:
    gen_void_expr(node->m.lhs);
    return true;
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_REM:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
  case ND_SAR:
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
    gen_void_expr(node->m.lhs);
    gen_void_expr(node->m.rhs);
    return true;
  }
  return false;
}

// レジスタに置いた 4・8 バイトの整数・ポインタの変数への「x += 定数」「x -= 定数」「x++」で、
// 結果を使わないときは 1 命令にする
static bool try_reg_update(Node *lhs, NodeKind kind, Node *rhs, bool is_void) {
  if (!is_void || lhs->kind != ND_VAR || !lhs->m.var->reg || lhs->m.var->reg >= REG_FP_BASE)
    return false;
  if (!is_direct_local(lhs) || (kind != ND_ADD && kind != ND_SUB))
    return false;
  int sz = lhs->ty->size;
  if (sz != 4 && sz != 8)
    return false;
  int64_t imm;
  if (!is_imm12_operand(rhs, &imm))
    return false;
  // ポインタの増減の大きさは、構文解析で掛け算済み（rhs は定数）
  const char *r = var_reg_name(lhs->m.var, sz);
  char reg[8];
  snprintf(reg, sizeof(reg), "%s", r);
  Printftn("%s %s, %s, #%" PRIi64, kind == ND_ADD ? "add" : "sub", reg, reg, imm);
  return true;
}

static void gen_expr2(Node *node, bool is_void) {
  if (opt_g)
    print_loc(node->tok);

  if (is_void && !(node->ty->qual & (Q_VOLATILE | Q_ATOMIC)))
    if (gen_void_expr2(node))
      return;

  switch (node->kind) {
  case ND_NULL_EXPR:
  case ND_UNREACHABLE: {
    return;
  }
  case ND_NUM: {
    if (node->ty->kind == TY_BITINT)
      load_bitint_val(node->ty, node->num.bitint_data);
    else if (is_flonum(node->ty))
      load_fval(node);
    else if (is_integer(node->ty) || is_ptr(node->ty))
      load_val(node->ty, node->num.val);
    else
      internal_error();
    return;
  }
  case ND_POS: {
    gen_expr(node->m.lhs);
    return;
  }
  case ND_VAR:
  case ND_MEMBER:
    if (is_direct_local(node)) {
      load_local(node->m.var, node->ty);
      return;
    }
    if (is_plain_access(node)) {
      Addr a;
      gen_load_addr(node, &a, node->ty->size);
      load_addr(node, &a);
      return;
    }
    gen_addr(node);
    load(node);
    return;
  case ND_DEREF:
    if (is_plain_access(node)) {
      Addr a;
      gen_load_addr(node, &a, node->ty->size);
      load_addr(node, &a);
      return;
    }
    gen_expr(node->m.lhs);
    load(node);
    return;
  case ND_ADDR: {
    gen_addr(node->m.lhs);
    return;
  }
  case ND_ASSIGN:
    if (is_direct_local(node->m.lhs)) {
      gen_expr(node->m.rhs);
      store_local(node->m.lhs->m.var, node->m.lhs->ty);
      return;
    }
    if (is_plain_access(node->m.lhs)) {
      Addr a;
      if (addr_nocode(node->m.lhs, &a)) {
        gen_expr(node->m.rhs);
        store_s(node->m.lhs->ty, a.ofs, a.base);
        return;
      }
    }
    gen_addr(node->m.lhs);
    push();
    gen_expr(node->m.rhs);
    store(node->m.lhs, is_void);
    return;
  case ND_ARITH_ASSIGN:
    if (try_reg_update(node->m.lhs, node->arith_kind, node->m.rhs, is_void))
      return;
    if (!is_direct_local(node->m.lhs) && is_plain_access(node->m.lhs)) {
      Addr a;
      if (addr_nocode(node->m.lhs, &a)) {
        load_s(node->m.lhs->ty, a.ofs, a.base);
        gen_expr_null_lhs(node->arith_kind, node->m.lhs->ty, node->m.rhs);
        store_s(node->m.lhs->ty, a.ofs, a.base);
        return;
      }
    }
    if (is_direct_local(node->m.lhs)) {
      Obj *var = node->m.lhs->m.var;
      load_local(var, node->m.lhs->ty);
      gen_expr_null_lhs(node->arith_kind, node->m.lhs->ty, node->m.rhs);
      store_local(var, node->m.lhs->ty);
      return;
    }
    gen_addr(node->m.lhs);
    push();
    load(node->m.lhs);
    gen_expr_null_lhs(node->arith_kind, node->m.lhs->ty, node->m.rhs);
    store(node->m.lhs, is_void);
    return;
  case ND_POST_INCDEC:
    if (try_reg_update(node->m.lhs, ND_ADD, node->m.rhs, is_void))
      return;
    if (is_direct_local(node->m.lhs)) {
      Obj *var = node->m.lhs->m.var;
      load_local(var, node->m.lhs->ty);
      push_by_ty(node->m.lhs->ty);
      gen_expr_null_lhs(ND_ADD, node->m.lhs->ty, node->m.rhs);
      store_local(var, node->m.lhs->ty);
      pop_by_ty(node->m.lhs->ty);
      return;
    }
    gen_addr(node->m.lhs);
    Printstn("mov x2, x0");
    load(node->m.lhs);
    push_by_ty(node->m.lhs->ty);
    push_from("x2");
    gen_expr_null_lhs(ND_ADD, node->m.lhs->ty, node->m.rhs);
    store(node->m.lhs, true);
    pop_by_ty(node->m.lhs->ty);
    return;
  case ND_CKD_ARITH: {
    Type *res_ty = node->m.target->ty->base;
    int32_t chk_bits = res_ty->is_unsigned + bit_size(res_ty);
    int32_t bits = MAX(chk_bits, res_ty->size * 8);
    int32_t l_hdr = ovf_headroom(node->m.lhs->ty, node->arith_kind);
    int32_t r_hdr = ovf_headroom(node->m.rhs->ty, node->arith_kind);
    bits = MAX(bits, l_hdr);
    bits = MAX(bits, r_hdr);
    bits = align_to(bits, 64);
    int32_t sz = bits / 8;
    tmpbuf(sz);

    gen_expr(node->m.target);
    push();
    gen_cast_to_bitint(bits, node->m.lhs);
    push_bitint(sz);
    gen_cast_to_bitint(bits, node->m.rhs);

    int pos = pop_bitint(sz);
    mov_imm("w0", bits);
    add_imm("x1", lvar_ptr, pos);
    Printstn("mov x2, x20");
    gen_bitint_builtin_call(node->arith_kind);

    pop("x3");
    gen_mem_copy("x20", "x3", res_ty->size);

    mov_imm("w0", bits);
    Printstn("mov x1, x20");
    mov_imm("w2", chk_bits);
    mov_imm("w3", res_ty->is_unsigned);
    gen_bitint_builtin_call2("__slimcc_bitint_overflow");
    return;
  }
  case ND_STMT_EXPR:
    name_labels(node->blk.local_labels);
    for (Node *n = node->blk.body; n != node->blk.result; n = n->next)
      gen_stmt(n);
    if (node->blk.result)
      gen_expr2(node->blk.result->m.lhs, is_void);
    if (has_defr(node)) {
      push_by_ty(node->ty);
      gen_defr(node);
      pop_by_ty(node->ty);
    }
    return;
  case ND_CHAIN:
  case ND_COMMA:
    gen_void_expr(node->m.lhs);
    gen_expr2(node->m.rhs, is_void);
    return;
  case ND_CAST: {
    gen_cast(node);
    return;
  }
  case ND_INIT_SEQ:
    pin_var(node->m.var);
    add_imm("x2", node->m.var->ptr, node->m.var->ofs);
    gen_mem_zero("x2", node->m.var->ty->size);
    for (Node *n = node->m.lhs; n; n = n->next)
      gen_void_expr(n);
    return;
  case ND_COND: {
    // 条件が定数なら、選ばれる側だけを出す（GCC と同じ。picolibc の fdlibm.h は、選ばれない側に
    // 定義のない関数 __undefined_shift_size の呼び出しを置いている）
    int64_t val;
    if (is_const_expr(node->ctrl.cond, &val)) {
      gen_expr2(val ? node->ctrl.then : node->ctrl.els, is_void);
      return;
    }
    int64_t c = count();
    char else_label[STRBUF_SZ];
    snprintf(else_label, STRBUF_SZ, ".L.else.%" PRIi64, c);

    gen_cond(node->ctrl.cond, false, else_label);
    gen_expr2(node->ctrl.then, is_void);
    Printftn("b .L.end.%" PRIi64, c);
    Printfsn("%s:", else_label);
    gen_expr2(node->ctrl.els, is_void);
    Printfsn(".L.end.%" PRIi64 ":", c);
    return;
  }
  case ND_NOT:
    gen_expr(node->m.lhs);
    Printstn("eor w0, w0, #1");
    return;
  case ND_LOGAND:
  case ND_LOGOR: {
    gen_logical(node, false);
    return;
  }
  case ND_FUNCALL: {
    gen_funcall(node);
    Type *ty = node->ty;

    if (ty->kind == TY_BITINT) {
      tmpbuf(ty->size);
      if (bitint_rtn_need_copy(ty->bit_cnt)) {
        add_imm("x2", node->call.rtn_buf->ptr, node->call.rtn_buf->ofs);
        gen_mem_copy("x2", "x20", ty->size);
      } else if (ty->bit_cnt > 64) {
        Printstn("stp x0, x1, [x20]");
      } else {
        Printstn("str x0, [x20]");
      }
      return;
    }
    if (is_void)
      return;
    if (is_integer(ty) && ty->size < 4) {
      // 1・2 バイトの戻り値は、上位ビットが不定なので拡張する
      static const char *const ext[2][3] = {{NULL, "sxtb", "sxth"}, {NULL, "uxtb", "uxth"}};
      Printftn("%s w0, w0", ext[ty->is_unsigned][ty->size]);
      return;
    }
    if (ty->kind == TY_BOOL) {
      Printstn("uxtb w0, w0");
      return;
    }
    if (node->call.rtn_buf) {
      if (!is_mem_rtn(ty))
        copy_ret_buffer(node->call.rtn_buf);
      add_imm("x0", node->call.rtn_buf->ptr, node->call.rtn_buf->ofs);
    }
    return;
  }
  case ND_LABEL_VAL: {
    Printftn("adr x0, .L.jmp.%" PRIi64, node->jmp.target->lbl.id);
    return;
  }
  case ND_CAS:
    gen_cas(node);
    return;
  case ND_EXCH:
    gen_exch(node, is_void);
    return;
  case ND_THREAD_FENCE: {
    Printstn("dmb ish");
    return;
  }
  case ND_ALLOCA:
  case ND_ALLOCA_ZINIT:
    gen_expr(node->m.lhs);
    builtin_alloca(node);
    return;
  case ND_FRAME_ADDR:
    // フレームレコード: [x29] = 呼び出し元の x29、[x29 + 8] = 戻り先
    Printstn("mov x0, x29");
    for (int64_t i = 0; i < node->num.val; i++)
      Printstn("ldr x0, [x0]");
    return;
  case ND_RTN_ADDR:
    Printstn("mov x0, x29");
    for (int64_t i = 0; i < node->num.val; i++)
      Printstn("ldr x0, [x0]");
    Printstn("ldr x0, [x0, #8]");
    return;
  case ND_VA_START: {
    gen_expr(node->m.lhs);
    gen_va_start();
    return;
  }
  case ND_VA_COPY: {
    gen_expr(node->m.lhs);
    push();
    gen_expr(node->m.rhs);
    const char *reg = pop_inreg("x1");
    gen_mem_copy("x0", reg, 32);
    return;
  }
  case ND_VA_ARG: {
    gen_expr(node->m.lhs);
    gen_va_arg(node);
    return;
  }
  }

  if (is_int_arith(node) || is_cmp(node)) {
    if (is_gp_ty(node->m.lhs->ty)) {
      gen_gp_arith(node);
      return;
    }
    if (node->m.lhs->ty->kind == TY_BITINT) {
      gen_bitint_arith(node);
      return;
    }
  }

  if (is_fp_arith(node) || is_cmp(node)) {
    switch (node->m.lhs->ty->kind) {
    case TY_FLOAT:
    case TY_DOUBLE:  gen_fp_arith(node); return;
    case TY_LDOUBLE: gen_f128_arith(node); return;
    }
  }

  internal_error();
}

static void gen_expr(Node *node) {
  gen_expr2(node, false);
}

static void gen_void_expr(Node *node) {
  gen_expr2(node, true);
}

//
// 文
//

// 到達しない文は生成しない（x86-64 版と同じ判定）。
// goto、break、continue、return、戻らない関数の呼び出しの後や、定数の条件で選ばれない側が対象。
// ラベルや case があれば、そこから再び到達しうる
static bool gen_reachable_stmt(Node *node);
static bool gen_unreachable_stmt(Node *node);

static bool gen_block_stmt(Node *node, bool is_reach) {
  name_labels(node->blk.local_labels);
  for (Node *n = node->blk.body; n; n = n->next) {
    if (is_reach)
      is_reach = gen_reachable_stmt(n);
    else
      is_reach = gen_unreachable_stmt(n);
  }
  if (is_reach)
    gen_defr(node);
  return is_reach;
}

static bool gen_skip_unreachable_stmt(Node *node, bool is_reach, const char *lbl) {
  if (!node)
    return is_reach;
  if (!is_reach)
    return gen_unreachable_stmt(node);

  Printftn("b %s", lbl);
  gen_unreachable_stmt(node);
  Printfsn("%s:", lbl);
  return true;
}

static bool gen_if_stmt_const(Node *node, bool cond, const char *else_label) {
  if (cond) {
    bool is_reach = gen_reachable_stmt(node->ctrl.then);
    return gen_skip_unreachable_stmt(node->ctrl.els, is_reach, else_label);
  }
  bool is_reach = node->ctrl.els ? gen_reachable_stmt(node->ctrl.els) : true;
  return gen_skip_unreachable_stmt(node->ctrl.then, is_reach, else_label);
}

static bool gen_if_stmt(Node *node, bool is_reach) {
  int64_t c = count();
  char else_label[STRBUF_SZ];
  snprintf(else_label, STRBUF_SZ, ".L.else.%" PRIi64, c);

  if (!is_reach) {
    is_reach = gen_unreachable_stmt(node->ctrl.then);
    return gen_skip_unreachable_stmt(node->ctrl.els, is_reach, else_label);
  }

  int64_t val;
  if (is_const_expr(node->ctrl.cond, &val))
    return gen_if_stmt_const(node, val, else_label);

  bool flip, is_void;
  Node *expr = bool_expr_opt(node->ctrl.cond, &flip, &is_void);
  if (expr && is_void) {
    gen_void_expr(expr);
    return gen_if_stmt_const(node, flip, else_label);
  }

  gen_cond(node->ctrl.cond, false, else_label);
  is_reach = gen_reachable_stmt(node->ctrl.then);
  if (!node->ctrl.els) {
    Printfsn("%s:", else_label);
    return true;
  }

  if (is_reach)
    Printftn("b .L.end.%" PRIi64, c);
  Printfsn("%s:", else_label);
  bool is_reach2 = gen_reachable_stmt(node->ctrl.els);
  if (is_reach)
    Printfsn(".L.end.%" PRIi64 ":", c);
  return is_reach | is_reach2;
}

static bool gen_reachable_stmt(Node *node) {
  if (opt_g)
    print_loc(node->tok);

  switch (node->kind) {
  case ND_IF:
    return gen_if_stmt(node, true);
  case ND_BLOCK:
    return gen_block_stmt(node, true);
  case ND_GOTO:
  case ND_GOTO_EXPR:
  case ND_BREAK:
  case ND_CONT:
  case ND_RETURN:
    gen_stmt(node);
    return false;
  case ND_EXPR_STMT:
    if (node->m.lhs->kind == ND_UNREACHABLE)
      return false;
    if (node->m.lhs->kind == ND_FUNCALL &&
        is_direct_fncall(node->m.lhs->call.expr) &&
        node->m.lhs->call.expr->m.var->is_noreturn) {
      gen_stmt(node);
      return false;
    }
  }
  gen_stmt(node);
  return true;
}

static bool gen_unreachable_stmt(Node *node) {
  if (opt_g)
    print_loc(node->tok);

  switch (node->kind) {
  case ND_IF:
    return gen_if_stmt(node, false);
  case ND_BLOCK:
    return gen_block_stmt(node, false);
  case ND_CASE:
  case ND_DEFAULT:
  case ND_LABEL:
    gen_stmt(node);
    return true;
  case ND_FOR:
  case ND_DO:
  case ND_SWITCH:
    if (node->ctrl.then->no_label)
      return false;
    gen_stmt(node);
    return true;
  }
  return false;
}

static void gen_stmt_naked(Node *node) {
  switch (node->kind) {
  case ND_NULL_STMT: {
    return;
  }
  case ND_BLOCK:
    if (has_defr(node))
      break;
    for (Node *n = node->blk.body; n; n = n->next)
      gen_stmt_naked(n);
    return;
  case ND_ASM:
    if (!is_plain_asm(node))
      break;
    gen_asm(node);
    return;
  }
  error_tok(node->tok, "unsupported statement in naked function");
}

static void gen_return(Node *node) {
  if (codegen_fn->is_noreturn)
    error_tok(node->tok, "function is noreturn");

  if (!node->m.lhs) {
    gen_defr(node);
    Printftn("b .L.rtn.%" PRIi64, rtn_label);
    return;
  }

  Type *ty = node->m.lhs->ty;

  if (ty->size <= 0) {
    gen_defr(node);
    gen_void_expr(node->m.lhs);
    Printftn("b .L.rtn.%" PRIi64, rtn_label);
    return;
  }

  gen_expr(node->m.lhs);

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_BITINT) {
    if (is_mem_rtn(ty)) {
      if (ty->kind == TY_BITINT)
        Printstn("mov x0, x20");
      copy_struct_mem();
      if (has_defr(node)) {
        push();
        gen_defr(node);
        pop("x0");
      }
      Printftn("b .L.rtn.%" PRIi64, rtn_label);
      return;
    }

    // 値を BUF に移してから（defer の間も残るように）レジスタに読み込む
    tmpbuf(ty->size);
    if (ty->kind != TY_BITINT)
      gen_mem_copy("x0", "x20", ty->size);
    if (has_defr(node)) {
      push_bitint(ty->size);
      gen_defr(node);
      int pos = pop_bitint(ty->size);
      add_imm("x2", lvar_ptr, pos);
      gen_mem_copy("x2", "x20", ty->size);
    }
    Printstn("mov x0, x20");
    copy_struct_reg();
    Printftn("b .L.rtn.%" PRIi64, rtn_label);
    return;
  }
  if (has_defr(node)) {
    push_by_ty(ty);
    gen_defr(node);
    pop_by_ty(ty);
  }
  Printftn("b .L.rtn.%" PRIi64, rtn_label);
}

static void gen_switch(Node *node) {
  int64_t c = node->ctrl.id = count();
  gen_expr(node->ctrl.cond);

  bool is64 = node->ctrl.cond->ty->size == 8;
  const char *a = is64 ? "x0" : "w0";
  const char *t = is64 ? "x1" : "w1";

  int64_t case_cnt = 1;
  Node *label = NULL;
  for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next) {
    if (cr->label == node->ctrl.sw_default)
      continue;

    if (label != cr->label) {
      label = cr->label;
      label->cases.id = ++case_cnt;
    }

    if (cr->hi == cr->lo) {
      cmp_imm(a, cr->lo);
      Printftn("b.eq .L.case.%" PRIi64 ".%" PRIi64, c, case_cnt);
      continue;
    }
    if (cr->lo == 0) {
      cmp_imm(a, cr->hi);
      Printftn("b.ls .L.case.%" PRIi64 ".%" PRIi64, c, case_cnt);
      continue;
    }
    mov_imm(is64 ? "x16" : "w16", cr->lo);
    Printftn("sub %s, %s, %s", t, a, is64 ? "x16" : "w16");
    cmp_imm(t, (uint64_t)cr->hi - cr->lo);
    Printftn("b.ls .L.case.%" PRIi64 ".%" PRIi64, c, case_cnt);
  }

  if (node->ctrl.sw_default)
    Printftn("b .L.default.%" PRIi64, c);
  else
    Printftn("b .L.brk.%" PRIi64, c);
  gen_stmt(node->ctrl.then);
  Printfsn(".L.brk.%" PRIi64 ":", c);
}

static void gen_stmt(Node *node) {
  if (opt_g)
    print_loc(node->tok);

  switch (node->kind) {
  case ND_NULL_STMT: {
    return;
  }
  case ND_IF: {
    gen_if_stmt(node, true);
    return;
  }
  case ND_FOR: {
    int64_t c = node->ctrl.id = count();
    char brk[STRBUF_SZ];
    snprintf(brk, STRBUF_SZ, ".L.brk.%" PRIi64, c);

    if (node->ctrl.for_init)
      gen_stmt(node->ctrl.for_init);
    loop_depth++;
    Printfsn(".L.begin.%" PRIi64 ":", c);
    if (node->ctrl.cond)
      gen_cond(node->ctrl.cond, false, brk);
    gen_stmt(node->ctrl.then);
    Printfsn(".L.cont.%" PRIi64 ":", c);
    if (node->ctrl.for_inc)
      gen_void_expr(node->ctrl.for_inc);
    gen_defr(node);
    loop_depth--;
    Printftn("b .L.begin.%" PRIi64, c);
    Printfsn("%s:", brk);
    return;
  }
  case ND_DO: {
    int64_t c = node->ctrl.id = count();
    char bg[STRBUF_SZ];
    snprintf(bg, STRBUF_SZ, ".L.begin.%" PRIi64, c);

    Printfsn("%s:", bg);
    loop_depth++;
    gen_stmt(node->ctrl.then);
    Printfsn(".L.cont.%" PRIi64 ":", c);
    gen_cond(node->ctrl.cond, true, bg);
    loop_depth--;
    Printfsn(".L.brk.%" PRIi64 ":", c);
    return;
  }
  case ND_SWITCH:
    gen_switch(node);
    return;
  case ND_BLOCK:
    gen_block_stmt(node, true);
    return;
  case ND_BREAK:
    gen_defr(node);
    Printftn("b .L.brk.%" PRIi64, node->jmp.parent_loop->ctrl.id);
    return;
  case ND_CONT:
    gen_defr(node);
    Printftn("b .L.cont.%" PRIi64, node->jmp.parent_loop->ctrl.id);
    return;
  case ND_GOTO:
    gen_defr(node);
    Printftn("b .L.jmp.%" PRIi64, node->jmp.target->lbl.id);
    return;
  case ND_GOTO_EXPR:
    gen_expr(node->m.lhs);
    Printstn("br x0");
    return;
  case ND_DEFAULT:
    Printfsn(".L.default.%" PRIi64 ":", node->cases.parent_sw->ctrl.id);
    return;
  case ND_CASE:
    Printfsn(".L.case.%" PRIi64 ".%" PRIi64 ":", node->cases.parent_sw->ctrl.id,
             node->cases.id);
    return;
  case ND_LABEL:
    if (node->lbl.id)
      Printfsn(".L.jmp.%" PRIi64 ":", node->lbl.id);
    return;
  case ND_RETURN:
    gen_return(node);
    return;
  case ND_EXPR_STMT: {
    gen_void_expr(node->m.lhs);
    return;
  }
  case ND_ASM: {
    gen_asm(node);
    return;
  }
  }

  error_tok(node->tok, "invalid statement");
}

//
// インラインアセンブリ（GNU 拡張）
//
// オペランドの評価結果（出力のアドレス、入力の値）は、パーサーの段階で用意した一時変数に置き、
// asm の直前にレジスタへ読み込む。asm の後で出力のレジスタを書き戻す。
// 対応する制約: r、w、m（Q など）、即値（i、n、I〜N）、Z、g、X、数字（出力との対応）、=、+、&
// 対応する修飾子: %w %x %s %d %q %h %b %c %a %l、%[name]、%=、%%

#define ASM_FP_BASE 32

typedef struct {
  bool out, inout, early;
  bool r, w, m, imm, zero, any;
  int match;
} AsmCons;

static AsmCons parse_asm_cons(Token *tok) {
  AsmCons c = {.match = -1};
  for (const char *p = tok->str; *p; p++) {
    switch (*p) {
    case '=': c.out = true; break;
    case '+': c.inout = true; break;
    case '&': c.early = true; break;
    case 'r': c.r = true; break;
    case 'w':
    case 'x':
    case 'y': c.w = true; break;
    case 'm':
    case 'Q':
    case 'o':
    case 'V': c.m = true; break;
    case 'i':
    case 'n':
    case 'I':
    case 'J':
    case 'K':
    case 'L':
    case 'M':
    case 'N': c.imm = true; break;
    case 'Z': c.zero = true; break;
    case 'g': c.r = c.m = c.imm = true; break;
    case 'X': c.any = true; break;
    default:
      if (Isdigit(*p))
        c.match = strtol(p, (char **)&p, 10), p--;
      break;
    }
  }
  return c;
}

static bool is_asm_lvalue(Node *node) {
  switch (node->kind) {
  case ND_VAR:
  case ND_DEREF:
  case ND_MEMBER: return true;
  }
  return false;
}

void prepare_inline_asm(Node *node) {
  int n_out = 0;
  for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next, n_out++) {
    AsmCons c = parse_asm_cons(ap->constraint);
    if (!c.out && !c.inout)
      error_tok(ap->constraint, "output operand constraint lacks '='");
    if (is_bitfield(ap->arg))
      error_tok(ap->arg->tok, "bit-field in asm output is not supported");
    ap->is_inout = c.inout;
    ap->is_early_clobber = c.early;
    ap->ptr = new_lvar(pointer_to(ap->arg->ty));
    if (c.r || c.w || c.any) {
      ap->kind = ASMOP_REG;
      ap->is_fp_reg = c.w && !c.r;
    } else if (c.m) {
      ap->kind = ASMOP_MEM;
    } else {
      error_tok(ap->constraint, "unsupported asm output constraint");
    }
  }

  for (AsmParam *ap = node->gasm.inputs; ap; ap = ap->next) {
    AsmCons c = parse_asm_cons(ap->constraint);
    int64_t val;

    // レジスタで渡す配列・関数は、ポインタに変換する
    if (!c.m || c.r || c.w || c.match >= 0)
      ptr_convert(&ap->arg);

    if (c.match >= 0) {
      if (c.match >= n_out)
        error_tok(ap->constraint, "matching constraint references an invalid operand");
      AsmParam *out = node->gasm.outputs;
      for (int i = 0; i < c.match; i++)
        out = out->next;
      if (out->kind != ASMOP_REG)
        error_tok(ap->constraint, "matching constraint for a memory operand is not supported");
      ap->match = out;
      ap->kind = ASMOP_REG;
      ap->is_fp_reg = out->is_fp_reg;
      ap->var = new_lvar(ap->arg->ty);
      continue;
    }
    if ((c.imm || c.zero) && is_integer(ap->arg->ty) && is_const_expr(ap->arg, &val) &&
        (c.imm || val == 0)) {
      ap->kind = ASMOP_NUM;
      ap->val = val;
      ap->use_zero_reg = c.zero && val == 0;
      continue;
    }
    if (c.r || c.w || c.any || (c.zero && !c.m)) {
      ap->kind = ASMOP_REG;
      ap->is_fp_reg = c.w && !c.r;
      ap->var = new_lvar(ap->arg->ty);
      continue;
    }
    if (c.m) {
      ap->kind = ASMOP_MEM;
      if (is_asm_lvalue(ap->arg))
        ap->ptr = new_lvar(pointer_to(ap->arg->ty));
      else
        ap->var = new_lvar(ap->arg->ty);
      continue;
    }
    error_tok(ap->constraint, "unsupported asm input constraint");
  }
}

// レジスタ名（x0、w0、v0、q0、d0、s0、h0、b0、fp、lr）を番号にする。SIMD は ASM_FP_BASE から
static int asm_reg_no(const char *name, Token *tok) {
  if (!strcmp(name, "fp"))
    return 29;
  if (!strcmp(name, "lr"))
    return 30;
  char *end;
  if ((name[0] == 'x' || name[0] == 'w') && Isdigit(name[1])) {
    long n = strtol(name + 1, &end, 10);
    if (!*end && n <= 30)
      return n;
  }
  if (strchr("vqdshb", name[0]) && Isdigit(name[1])) {
    long n = strtol(name + 1, &end, 10);
    if (!*end && n <= 31)
      return ASM_FP_BASE + n;
  }
  if (tok)
    error_tok(tok, "unknown register name '%s'", name);
  return -1;
}

static void asm_load(AsmParam *ap, const char *base, int64_t ofs) {
  Type *ty = ap->arg->ty;
  char buf[STRBUF_SZ];
  int sz = ty->size;
  if (ap->reg >= ASM_FP_BASE) {
    if (sz != 4 && sz != 8 && sz != 16)
      error_tok(ap->arg->tok, "invalid operand size for an SIMD register");
    Printftn("ldr %s, %s", freg(ap->reg - ASM_FP_BASE, sz), mem_op(buf, ofs, base, sz, "x17"));
    return;
  }
  if (sz != 1 && sz != 2 && sz != 4 && sz != 8)
    error_tok(ap->arg->tok, "invalid operand size for a general register");
  bool is_unsigned = !is_integer(ty) || ty->is_unsigned;
  Printftn("%s %s, %s", ld_ins(sz, is_unsigned), gp_of(xreg[ap->reg], sz),
           mem_op(buf, ofs, base, sz, "x17"));
}

static void asm_store(AsmParam *ap, const char *base) {
  Type *ty = ap->arg->ty;
  int sz = ty->size;
  if (ap->reg >= ASM_FP_BASE)
    Printftn("str %s, [%s]", freg(ap->reg - ASM_FP_BASE, sz), base);
  else
    Printftn("%s %s, [%s]", st_ins(sz), gp_of(xreg[ap->reg], sz), base);
}

TLS static AsmParam *asm_ops[64];
TLS static int asm_ops_cnt;
TLS static int asm_labels_start;

// asm goto に出力があるとき、ラベルへの分岐は中継ラベル（出力を書き戻してから飛ぶ）に向ける
TLS static int64_t asm_goto_tramp;

static int asm_label_index(AsmParam *ap) {
  for (int i = asm_labels_start; i < asm_ops_cnt; i++)
    if (asm_ops[i] == ap)
      return i - asm_labels_start;
  internal_error();
}

static AsmParam *asm_find_op(const char **rest, const char *p, bool is_label, Token *tok) {
  if (*p == '[') {
    p++;
    for (int i = 0; i < asm_ops_cnt; i++) {
      Token *name = is_label ? (i >= asm_labels_start ? asm_ops[i]->arg->tok : NULL)
                             : asm_ops[i]->name;
      if (name && !strncmp(p, name->loc, name->len) && p[name->len] == ']') {
        *rest = p + name->len + 1;
        return asm_ops[i];
      }
    }
  } else if (Isdigit(*p)) {
    char *end;
    long idx = strtol(p, &end, 10);
    *rest = end;
    if (idx < asm_ops_cnt)
      return asm_ops[idx];
  }
  error_tok(tok, "asm operand not found");
}

static void asm_body(Node *node, int64_t uniq) {
  Token *tok = node->gasm.str_tok;
  const char *p = tok->str;
  Printsts("");
  while (*p) {
    if (*p != '%') {
      if (*p == '\n')
        Printsts("\n\t");
      else
        fputc(*p, output_file);
      p++;
      continue;
    }
    p++;
    if (*p == '%') {
      fputc('%', output_file);
      p++;
      continue;
    }
    if (*p == '=') {
      Printf("%" PRIi64, uniq);
      p++;
      continue;
    }

    char mod = 0;
    if (strchr("wxsdqhbcalz", *p) && (p[1] == '[' || Isdigit(p[1])))
      mod = *p++;

    AsmParam *ap = asm_find_op(&p, p, mod == 'l', tok);

    if (mod == 'l') {
      if (asm_goto_tramp)
        Printf(".L.asm_goto.%" PRIi64 ".%d", asm_goto_tramp, asm_label_index(ap));
      else
        Printf(".L.jmp.%" PRIi64, ap->arg->jmp.target->lbl.id);
      continue;
    }
    switch (ap->kind) {
    case ASMOP_NUM:
      if (ap->use_zero_reg && mod != 'c')
        Printf("%s", mod == 'w' ? "wzr" : "xzr");
      else
        Printf("%" PRIi64, ap->val);
      continue;
    case ASMOP_MEM:
      Printf("[%s]", xreg[ap->reg]);
      continue;
    case ASMOP_REG:
      if (ap->reg >= ASM_FP_BASE) {
        int n = ap->reg - ASM_FP_BASE;
        if (mod && strchr("sdqhb", mod))
          Printf("%c%d", mod, n);
        else
          Printf("v%d", n);
      } else {
        Printf("%s", mod == 'w' ? wreg[ap->reg] : xreg[ap->reg]);
      }
      continue;
    }
    internal_error();
  }
  Printssn("");
}

static void gen_asm(Node *node) {
  if (is_plain_asm(node)) {
    Printftn("%s", node->gasm.str_tok->str);
    return;
  }

  // 1. オペランドを評価して一時変数に置く
  asm_ops_cnt = 0;
  for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next) {
    gen_addr(ap->arg);
    store_s(ap->ptr->ty, ap->ptr->ofs, ap->ptr->ptr);
    asm_ops[asm_ops_cnt++] = ap;
  }
  for (AsmParam *ap = node->gasm.inputs; ap; ap = ap->next) {
    if (ap->kind == ASMOP_MEM && ap->ptr) {
      gen_addr(ap->arg);
      store_s(ap->ptr->ty, ap->ptr->ofs, ap->ptr->ptr);
    } else if (ap->var) {
      gen_var_assign(ap->var, ap->arg);
    }
    asm_ops[asm_ops_cnt++] = ap;
  }
  asm_labels_start = asm_ops_cnt;
  for (AsmParam *ap = node->gasm.labels; ap; ap = ap->next)
    asm_ops[asm_ops_cnt++] = ap;
  clobber_all_regs();

  // 2. 使えないレジスタ（クロバー、x16〜x30、v8〜v15）を除いてレジスタを割り当てる
  bool used[64] = {0};
  for (int i = 16; i <= 30; i++)
    used[i] = true;
  for (int i = 8; i <= 15; i++)
    used[ASM_FP_BASE + i] = true;

  bool clobber[64] = {0};
  if (node->gasm.clobbers) {
    Token *start = node->gasm.clobbers;
    for (Token *t = start; t->kind != TK_COLON && t->kind != TK_COLON2 && t->kind != TK_RPAREN;
         t = t->next) {
      if (t != start)
        t = skip_tk(t, TK_COMMA);
      if (equal(t, "\"cc\"") || equal(t, "\"memory\""))
        continue;
      int r = asm_reg_no(t->str, NULL);
      if (r < 0)
        continue;
      clobber[r] = used[r] = true;
    }
  }

  static const int gp_pool[] = {9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 1, 0};
  static const int fp_pool[] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                                2,  3,  4,  5,  6,  7,  1,  0};

  for (int i = 0; i < asm_labels_start; i++) {
    AsmParam *ap = asm_ops[i];
    if (ap->kind == ASMOP_NUM)
      continue;
    if (ap->match) {
      ap->reg = ap->match->reg;
      continue;
    }
    // ローカルのレジスタ変数（register long x asm("x8") など）は、そのレジスタを使う
    if (ap->kind == ASMOP_REG && ap->arg->kind == ND_VAR && ap->arg->m.var->asm_name &&
        ap->arg->m.var->kind == OBJ_LOCAL) {
      ap->reg = asm_reg_no(ap->arg->m.var->asm_name, ap->arg->tok);
      used[ap->reg] = true;
      continue;
    }
    bool fp = ap->kind == ASMOP_REG && ap->is_fp_reg;
    const int *pool = fp ? fp_pool : gp_pool;
    int n = fp ? sizeof(fp_pool) / sizeof(*fp_pool) : sizeof(gp_pool) / sizeof(*gp_pool);
    int base = fp ? ASM_FP_BASE : 0;
    ap->reg = -1;
    for (int j = 0; j < n; j++) {
      if (!used[base + pool[j]]) {
        ap->reg = base + pool[j];
        used[ap->reg] = true;
        break;
      }
    }
    if (ap->reg < 0)
      error_tok(node->tok, "too many asm operands");
  }

  // 3. asm が壊す呼び出し先保存レジスタ（x19〜x28、d8〜d15）を退避する
  int saved[32];
  int n_saved = 0;
  for (int r = 19; r <= 28; r++)
    if (clobber[r])
      saved[n_saved++] = r;
  for (int r = 8; r <= 15; r++)
    if (clobber[ASM_FP_BASE + r])
      saved[n_saved++] = ASM_FP_BASE + r;
  for (int i = 0; i < n_saved; i++) {
    if (saved[i] >= ASM_FP_BASE)
      Printftn("str d%d, [sp, #-16]!", saved[i] - ASM_FP_BASE);
    else
      Printftn("str %s, [sp, #-16]!", xreg[saved[i]]);
  }

  // 4. オペランドをレジスタに読み込む
  char buf[STRBUF_SZ];
  for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next) {
    Printftn("ldr x16, %s", mem_op(buf, ap->ptr->ofs, ap->ptr->ptr, 8, "x17"));
    if (ap->kind == ASMOP_MEM)
      Printftn("mov %s, x16", xreg[ap->reg]);
    else if (ap->is_inout)
      asm_load(ap, "x16", 0);
  }
  for (AsmParam *ap = node->gasm.inputs; ap; ap = ap->next) {
    if (ap->kind == ASMOP_REG) {
      pin_var(ap->var);
      asm_load(ap, ap->var->ptr, ap->var->ofs);
    } else if (ap->kind == ASMOP_MEM) {
      if (ap->ptr)
        Printftn("ldr %s, %s", xreg[ap->reg], mem_op(buf, ap->ptr->ofs, ap->ptr->ptr, 8, "x17"));
      else {
        pin_var(ap->var);
        add_imm(xreg[ap->reg], ap->var->ptr, ap->var->ofs);
      }
    }
  }

  // 5. 本体
  bool tramp = node->gasm.labels && (node->gasm.outputs || n_saved);
  asm_goto_tramp = tramp ? count() : 0;
  int64_t tramp_id = asm_goto_tramp;
  asm_body(node, count());
  asm_goto_tramp = 0;

  // 6. 退避したレジスタを戻してから、出力を書き戻す（lvar_ptr が x19 のこともあるため）。
  //    asm goto では、ラベルへ飛ぶ経路ごとにも同じことをする
  int n_paths = 1;
  if (tramp)
    for (AsmParam *ap = node->gasm.labels; ap; ap = ap->next)
      n_paths++;

  for (int path = 0; path < n_paths; path++) {
    if (path > 0)
      Printfsn(".L.asm_goto.%" PRIi64 ".%d:", tramp_id, path - 1);
    for (int i = n_saved - 1; i >= 0; i--) {
      if (saved[i] >= ASM_FP_BASE)
        Printftn("ldr d%d, [sp], #16", saved[i] - ASM_FP_BASE);
      else
        Printftn("ldr %s, [sp], #16", xreg[saved[i]]);
    }
    for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next) {
      if (ap->kind != ASMOP_REG)
        continue;
      Printftn("ldr x16, %s", mem_op(buf, ap->ptr->ofs, ap->ptr->ptr, 8, "x17"));
      asm_store(ap, "x16");
    }
    if (path == 0) {
      if (tramp)
        Printftn("b .L.asm_goto_end.%" PRIi64, tramp_id);
    } else {
      AsmParam *lbl = asm_ops[asm_labels_start + path - 1];
      Printftn("b .L.jmp.%" PRIi64, lbl->arg->jmp.target->lbl.id);
    }
  }
  if (tramp)
    Printfsn(".L.asm_goto_end.%" PRIi64 ":", tramp_id);
}

//
// データとテキストの出力
//

static int get_lvar_align(Scope *scp, int32_t align) {
  for (Obj *var = scp->locals; var; var = var->next) {
    if (var->pass_by_stack)
      continue;
    int32_t a2 = get_align(var);
    align = MAX(align, a2);
  }
  for (Scope *sub = scp->children; sub; sub = sub->sibling_next) {
    int32_t a2 = get_lvar_align(sub, align);
    align = MAX(align, a2);
  }
  return align;
}

static int assign_lvar_offsets(Scope *sc, int bottom) {
  for (Obj *var = sc->locals; var; var = var->next) {
    if (var->pass_by_stack) {
      var->ofs = var->stack_offset + 16;
      var->ptr = x29;
      continue;
    }
    size_t sz = var->ty->size;
    bottom += sz ? sz : 1;
    bottom = align_to(bottom, get_align(var));
    var->ofs = -bottom;
    var->ptr = lvar_ptr;
  }

  int max_depth = bottom;
  for (Scope *sub = sc->children; sub; sub = sub->sibling_next) {
    int sub_depth = assign_lvar_offsets(sub, bottom);
    if (!opt_reuse_stack || codegen_fn->dont_reuse_stk)
      bottom = max_depth = sub_depth;
    else
      max_depth = MAX(max_depth, sub_depth);
  }
  return max_depth;
}

static void emit_symbol2(Obj *var, const char *name, const char *vis) {
  if (var->is_static)
    Printftn(".local \"%s\"", name);
  else if (var->is_weak)
    Printftn(".weak \"%s\"", name);
  else
    Printftn(".globl \"%s\"", name);

  if (vis &&
      (!strcmp(vis, "hidden") || !strcmp(vis, "internal") || !strcmp(vis, "protected")))
    Printftn(".%s \"%s\"", vis, name);

  if (var->alias_name)
    Printftn(".set \"%s\", \"%s\"", name, var->alias_name);
}

static void emit_symbol(Obj *var, const char *name) {
  emit_symbol2(var, name, var->visibility ? var->visibility : opt_visibility);
}

static bool is_rodata_obj(Obj *var) {
  if (var->is_string_lit)
    return true;
  if (!is_const_var(var))
    return false;
  if (opt_fpic || opt_fpie) {
    if (var->rel)
      return false;
    if (var->section_name)
      return !strcmp(var->section_name, ".interp") ||
             !strncmp(var->section_name, ".note", 5);
  }
  return true;
}

static void emit_data(Obj *var) {
  int64_t sz = var->ty->size;
  const char *var_name = get_symbol(var);

  if (var->ty->kind == TY_ARRAY && sz < 0)
    sz = var->ty->base->size;

  if (opt_femulated_tls && var->kind == OBJ_TLS) {
    char name_t[STRBUF_SZ], name_v[STRBUF_SZ];
    snprintf(name_t, STRBUF_SZ, "__emutls_t.%s", var_name);
    snprintf(name_v, STRBUF_SZ, "__emutls_v.%s", var_name);

    Obj tmp_var = *var;
    tmp_var.kind = OBJ_GLOBAL;
    tmp_var.asm_name = tmp_var.alias_name = NULL;

    emit_symbol(&tmp_var, name_v);
    Printstn(".data");
    Printftn(".size \"%s\", 32", name_v);
    Printstn(".balign 8");
    Printfsn("\"%s\":", name_v);
    Printftn(".quad %" PRIi64, sz);
    Printftn(".quad %d", get_align(var));
    Printstn(".quad 0");
    Printftn(".quad \"%s\"", name_t);

    tmp_var.name = name_t;
    emit_data(&tmp_var);
    return;
  }

  emit_symbol(var, var_name);

  if (!var->init_data &&
      !var->is_static_lvar &&
      var->kind != OBJ_TLS &&
      !(var->is_weak || var->section_name) &&
      (var->is_common || (!var->is_nocommon && opt_fcommon))) {
    Printftn(".comm \"%s\", %" PRIi64 ", %d", var_name, sz, get_align(var));
    return;
  }

  bool use_bss = !var->init_data;
  bool use_rodata = is_rodata_obj(var);

  if (var->section_name)
    Printfts(".section \"%s\"", var->section_name);
  else if (var->kind == OBJ_TLS)
    Printfts(".section .%s", use_bss ? "tbss" : "tdata");
  else if (use_rodata)
    Printsts(".section .rodata");
  else if ((opt_fpic || opt_fpie) && var->rel)
    Printfts(".section .data.rel%s%s", (is_const_var(var) ? ".ro" : ""),
             (opt_fpie ? ".local" : ""));
  else
    Printfts(".section .%s", use_bss ? "bss" : "data");

  if (opt_data_sections && !var->section_name)
    Printf(".\"%s\"", var_name);

  if (var->kind == OBJ_TLS)
    Printf(",\"awT\",%s\n", use_bss ? "@nobits" : "@progbits");
  else if (use_rodata)
    Printssn(",\"a\"");
  else
    Printssn(",\"aw\"");

  Printftn(".type \"%s\", @object", var_name);
  Printftn(".size \"%s\", %" PRIi64, var_name, sz);
  Printftn(".balign %d", get_align(var));
  Printfsn("\"%s\":", var_name);

  if (sz == 0)
    return;

  if (!var->init_data) {
    Printftn(".zero %" PRIi64, sz);
    return;
  }

  Relocation *rel = var->rel;
  for (int pos = 0;;) {
    int rem = (rel ? rel->offset : sz) - pos;

    while (rem > 0) {
      int p2 = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
      switch (p2) {
      case 1: Printftn(".byte %" PRIi8, BUFF_CAST(int8_t, &var->init_data[pos])); break;
      case 2: Printftn(".hword %" PRIi16, BUFF_CAST(int16_t, &var->init_data[pos])); break;
      case 4: Printftn(".word %" PRIi32, BUFF_CAST(int32_t, &var->init_data[pos])); break;
      case 8: Printftn(".xword %" PRIi64, BUFF_CAST(int64_t, &var->init_data[pos])); break;
      }
      pos += p2;
      rem -= p2;
    }

    if (!rel)
      break;

    if (rel->var)
      Printftn(".xword \"%s\"+%" PRIi64, get_symbol(rel->var), rel->addend);
    else
      Printftn(".xword .L.jmp.%" PRIi64 "+%" PRIi64, rel->label->jmp.target->lbl.id,
               rel->addend);

    pos += 8;
    rel = rel->next;
  }
}

static void emit_cdtor(const char *sec, uint16_t priority, const char *fn_name) {
  Printfts(".section .%s", sec);
  if (priority)
    Printf(".%" PRIu16, (uint16_t)(priority - 1));
  Printssn(",\"aw\"");
  Printstn(".balign 8");
  Printftn(".xword \"%s\"", fn_name);
}

// 関数の入口で、レジスタで渡された引数を局所変数に保存する
static void store_params(Obj *fn) {
  char buf[STRBUF_SZ];

  for (Obj *var = fn->ty->param_list; var; var = var->param_next) {
    if (var->pass_by_stack || var->is_zero_sized_arg || var->pass_by_ref)
      continue;

    Type *ty = var->ty;
    ArgInfo ai = classify(ty);
    if (var->reg) {
      // レジスタに置いた引数
      if (var->reg >= REG_FP_BASE) {
        Printftn("fmov %s, %s", var_reg_name(var, ty->size), freg(var->arg_reg, ty->size));
      } else {
        char w[8], x[8];
        snprintf(w, sizeof(w), "w%d", var->reg);
        snprintf(x, sizeof(x), "x%d", var->reg);
        set_var_reg(w, x, ty, xreg[var->arg_reg]);
      }
      continue;
    }
    if (ai.cls == AC_HFA || (ai.cls == AC_GP && ai.nreg != 1))
      pin_var(var);
    switch (ai.cls) {
    case AC_FP:
      Printftn("str %s, %s", freg(var->arg_reg, ty->size),
               mem_op(buf, var->ofs, var->ptr, ty->size, "x16"));
      continue;
    case AC_HFA:
      add_imm("x16", var->ptr, var->ofs);
      for (int i = 0; i < ai.nreg; i++)
        Printftn("str %s, [x16, #%d]", freg(var->arg_reg + i, ai.elem_sz), i * ai.elem_sz);
      continue;
    case AC_GP:
      add_imm("x16", var->ptr, var->ofs);
      for (int i = 0; i < ai.nreg; i++)
        store_gp_exact(xreg[var->arg_reg + i], "x16", i * 8, MIN(8, ty->size - i * 8));
      continue;
    case AC_REF:
      internal_error();
    }
  }

  // ポインタで渡された構造体を、局所変数にコピーする（引数レジスタをすべて保存した後）
  for (Obj *var = fn->ty->param_list; var; var = var->param_next) {
    if (!var->pass_by_ref)
      continue;
    const char *src;
    if (var->arg_reg >= 0) {
      src = xreg[var->arg_reg];
    } else {
      Printftn("ldr x15, %s", mem_op(buf, var->stack_offset + 16, x29, 8, "x16"));
      src = "x15";
    }
    add_imm("x14", var->ptr, var->ofs);
    gen_mem_copy(src, "x14", var->ty->size);
  }
}

// 関数の本体（プロローグからエピローグまで）を生成する。レジスタ割り当ての下見でも使う
TLS static bool reg_used[REG_FP_BASE + 32];

static void gen_fn_code(Obj *fn) {
  int gp_count = 0, fp_count = 0;
  int arg_stk_size = calling_convention(fn->ty->param_list, &gp_count, &fp_count);

  int lvar_align = get_lvar_align(fn->ty->scopes, 16);
  lvar_ptr = (lvar_align > 16) ? x19 : x29;
  lvar_base = (lvar_ptr == x29) ? 16 : 0;
  codegen_fn = fn;
  rtn_label = count();

  // プロローグ
  Printstn("stp x29, x30, [sp, #-16]!");
  Printstn("mov x29, sp");
  Printstn("stp x19, x20, [sp, #-16]!");
  if (lvar_align > 16) {
    Printstn("mov x16, sp");
    Printftn("and x16, x16, #-%d", lvar_align);
    Printstn("mov sp, x16");
    Printstn("mov x19, sp");
  }

  // フレームの確保と BUF の位置は、関数の終わりで決まる
  long stack_alloc_loc = resrvln();

  lvar_stk_sz = lvar_base;
  tmpbuf_sz = 0;

  if (fn->ty->is_variadic) {
    // 汎用レジスタ 8 本 × 8 バイト、SIMD レジスタ 8 本 × 16 バイトの保存領域
    va_gr_top_ofs = -lvar_stk_sz;
    va_vr_top_ofs = -(lvar_stk_sz + 64);
    lvar_stk_sz += 64 + 128;
    va_gp_named = gp_count;
    va_fp_named = fp_count;
    va_stack_ofs = 16 + arg_stk_size;
  }

  if (fn->dealloc_vla)
    vla_base_ofs = lvar_stk_sz += 8;

  bool rtn_by_stk = is_mem_rtn(fn->ty->return_ty);
  if (rtn_by_stk)
    rtn_ptr_ofs = lvar_stk_sz += 8;

  // レジスタに置いた変数のための、呼び出し先保存レジスタの退避場所
  int nsave = 0;
  int save_regs[16];
  for (int r = 21; r <= 28; r++)
    if (reg_used[r])
      save_regs[nsave++] = r;
  for (int r = 8; r <= 15; r++)
    if (reg_used[REG_FP_BASE + r])
      save_regs[nsave++] = REG_FP_BASE + r;
  int save_ofs = lvar_stk_sz += 8 * nsave;

  lvar_stk_sz = assign_lvar_offsets(fn->ty->scopes, lvar_stk_sz);
  peak_stk_usage = lvar_stk_sz = align_to(lvar_stk_sz, 8);

  char buf[STRBUF_SZ];

  if (fn->ty->is_variadic) {
    add_imm("x16", lvar_ptr, va_gr_top_ofs - 64);
    for (int i = 0; i < 8; i += 2)
      Printftn("stp x%d, x%d, [x16, #%d]", i, i + 1, i * 8);
    add_imm("x16", lvar_ptr, va_vr_top_ofs - 128);
    for (int i = 0; i < 8; i += 2)
      Printftn("stp q%d, q%d, [x16, #%d]", i, i + 1, i * 16);
  }

  if (fn->dealloc_vla) {
    Printstn("mov x16, sp");
    Printftn("str x16, %s", mem_op(buf, -vla_base_ofs, lvar_ptr, 8, "x17"));
  }

  if (rtn_by_stk)
    Printftn("str x8, %s", mem_op(buf, -rtn_ptr_ofs, lvar_ptr, 8, "x17"));

  for (int i = 0; i < nsave; i++) {
    int r = save_regs[i];
    Printftn("str %c%d, %s", r >= REG_FP_BASE ? 'd' : 'x', r % REG_FP_BASE,
             mem_op(buf, -save_ofs + 8 * i, lvar_ptr, 8, "x17"));
  }

  store_params(fn);

  // 本体
  bool is_reach = gen_reachable_stmt(fn->body);

  if (is_reach && !strcmp(fn->name, "main"))
    Printstn("mov w0, #0");

  // エピローグ
  Printfsn(".L.rtn.%" PRIi64 ":", rtn_label);
  for (int i = 0; i < nsave; i++) {
    int r = save_regs[i];
    Printftn("ldr %c%d, %s", r >= REG_FP_BASE ? 'd' : 'x', r % REG_FP_BASE,
             mem_op(buf, -save_ofs + 8 * i, lvar_ptr, 8, "x17"));
  }
  Printstn("ldp x19, x20, [x29, #-16]");
  Printstn("mov sp, x29");
  Printstn("ldp x29, x30, [sp], #16");
  Printstn("ret");

  if (tmp_stack.depth != 0)
    internal_error();

  int64_t frame = align_to(peak_stk_usage + tmpbuf_sz, 16);
  capture_begin();
  if (frame) {
    add_imm("sp", lvar_ptr, -frame);
    if (tmpbuf_sz)
      Printstn("mov x20, sp");
  }
  insert_captured(stack_alloc_loc);
}

static void clear_reg_info(Scope *sc) {
  for (Obj *var = sc->locals; var; var = var->next) {
    var->reg = 0;
    var->reg_pinned = false;
    var->reg_weight = 0;
  }
  for (Scope *sub = sc->children; sub; sub = sub->sibling_next)
    clear_reg_info(sub);
}

static bool is_reg_candidate(Obj *var, bool *is_fp) {
  if (var->kind != OBJ_LOCAL || var->reg_pinned || var->reg_weight < 2)
    return false;
  if (var->pass_by_stack || var->pass_by_ref || var->is_zero_sized_arg)
    return false;
  Type *ty = var->ty;
  if (ty->qual & (Q_VOLATILE | Q_ATOMIC))
    return false;
  if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE) {
    *is_fp = true;
    return true;
  }
  *is_fp = false;
  return (is_integer(ty) || ty->kind == TY_PTR) && ty->kind != TY_BITINT && ty->size <= 8;
}

static void collect_candidates(Scope *sc, Obj **list, int *n, int max) {
  for (Obj *var = sc->locals; var; var = var->next) {
    bool fp;
    if (*n < max && is_reg_candidate(var, &fp))
      list[(*n)++] = var;
  }
  for (Scope *sub = sc->children; sub; sub = sub->sibling_next)
    collect_candidates(sub, list, n, max);
}

// 下見のコード生成で調べ、使われる回数の多い局所変数からレジスタを割り当てる
static void reg_select(Obj *fn) {
  memset(reg_used, 0, sizeof(reg_used));
  clear_reg_info(fn->ty->scopes);

  FILE *real = output_file;
  char *buf = NULL;
  size_t len = 0;
  output_file = open_memstream(&buf, &len);
  reg_analysis = true;
  reg_disable = false;
  loop_depth = 0;
  gen_fn_code(fn);
  reg_analysis = false;
  fclose(output_file);
  free(buf);
  output_file = real;
  if (reg_disable)
    return;

  enum { MAX_CAND = 1024 };
  Obj *cand[MAX_CAND];
  int n = 0;
  collect_candidates(fn->ty->scopes, cand, &n, MAX_CAND);
  // 重い順（同じなら宣言の順）
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && cand[j - 1]->reg_weight < cand[j]->reg_weight; j--) {
      Obj *t = cand[j];
      cand[j] = cand[j - 1];
      cand[j - 1] = t;
    }
  int next_gp = 21, next_fp = 8;
  for (int i = 0; i < n; i++) {
    bool fp;
    is_reg_candidate(cand[i], &fp);
    if (fp && next_fp <= 15) {
      cand[i]->reg = REG_FP_BASE + next_fp++;
      reg_used[cand[i]->reg] = true;
    } else if (!fp && next_gp <= 28) {
      cand[i]->reg = next_gp++;
      reg_used[cand[i]->reg] = true;
    }
  }
}

// emit_text が作った関数ごとの出力。構文解析の途中（関数を読み終えるたび）に作られるので、
// エラーで中断したときは codegen_reset で解放する（ライブラリとして何度もコンパイルするため）
TLS static FuncObj **emitted;
TLS static int emitted_len, emitted_cap;
TLS static bool in_emit_text;

void emit_text(Obj *fn) {
  if (emitted_len == emitted_cap) {
    emitted_cap = emitted_cap ? emitted_cap * 2 : 64;
    emitted = realloc(emitted, emitted_cap * sizeof(*emitted));
  }
  if (!debug_file_used) {
    debug_file_used = calloc(1, display_files.len * sizeof(bool));
    debug_file_id = malloc(display_files.len * sizeof(int));
  }
  fn->output = calloc(1, sizeof(FuncObj));
  emitted[emitted_len++] = fn->output;
  output_file = open_memstream(&fn->output->buf, &fn->output->buflen);
  in_emit_text = true;

  const char *fn_name = asm_name(fn);

  if (fn->is_ctor)
    emit_cdtor("init_array", fn->ctor_prior, fn_name);
  if (fn->is_dtor)
    emit_cdtor("fini_array", fn->dtor_prior, fn_name);

  emit_symbol(fn, fn_name);

  if (fn->section_name)
    Printftn(".section \"%s\",\"ax\",@progbits", fn->section_name);
  else if (opt_func_sections)
    Printftn(".section .text.\"%s\",\"ax\",@progbits", fn_name);
  else
    Printstn(".text");

  int fnalign = fn->alt_align ? fn->alt_align : opt_fn_align;
  Printftn(".balign %d", MAX(fnalign, 4));
  Printftn(".type \"%s\", @function", fn_name);
  Printfsn("\"%s\":", fn_name);

  if (opt_g)
    print_loc(fn->body->tok);

  if (fn->is_naked) {
    gen_stmt_naked(fn->body);

    Printftn(".size \"%s\", .-\"%s\"", fn_name, fn_name);

    fclose(output_file);
    output_file = NULL;
    in_emit_text = false;
    return;
  }

  ext_refs = &(HashMap){0};
  if (!ir_gen_fn(fn)) {
    reg_select(fn);
    gen_fn_code(fn);
  }
#ifdef SLIMCC_LIBRARY
  if (slimcc_progress)
    slimcc_progress();    // 進み具合（libslimcc.h）
#endif

  Printftn(".size \"%s\", .-\"%s\"", fn_name, fn_name);

  {
    Obj head = {.next = fn->static_lvars};
    bool has_change;
    do {
      has_change = false;
      Obj *cur = &head;
      for (Obj *var = head.next; var; var = var->next) {
        if (var->is_live || var->is_used) {
          emit_data(var);
          has_change = true;
          continue;
        }
        cur = cur->next = var;
      }
      cur->next = NULL;
    } while (has_change);
  }

  if (ext_refs->buckets)
    free(ext_refs->buckets);
  ext_refs = NULL;
  codegen_fn = NULL;

  fclose(output_file);
  output_file = NULL;
  in_emit_text = false;
}

// 関数呼び出しの引数の置き場所を決める（gen_funcall と同じ規則で）。
// スタックで渡す引数は呼び出し時の sp からの位置に、それ以外は局所変数として置く
void prepare_funcall(Node *node, Scope *scope) {
  calling_convention(node->call.args, &(int){0}, &(int){0});

  if (scope->has_alloca) {
    Obj head = {0};
    Obj *cur = &head;
    for (Obj *var = node->call.args; var; var = var->param_next) {
      cur = cur->param_next = new_lvar(var->ty);
      cur->arg_expr = var->arg_expr;

      if (var->is_zero_sized_arg) {
        cur->is_zero_sized_arg = true;
        var->arg_expr = new_node(ND_NULL_EXPR, cur->arg_expr->tok);
      } else {
        var->arg_expr = new_var_node(cur, cur->arg_expr->tok);
      }
      add_type(var->arg_expr);
    }
    node->call.alloca_args = head.param_next;
  }

  for (Obj *var = node->call.args; var; var = var->param_next) {
    var->kind = OBJ_LOCAL;
    if (var->pass_by_stack) {
      var->ofs = var->stack_offset;
      var->ptr = "sp";
      continue;
    }
    if (var->is_zero_sized_arg)
      continue;
    var->next = scope->locals;
    scope->locals = var;
  }
}

static void peep_redunt_jmp(char *p) {
  while ((p = strstr(p, "\n\tb "))) {
    char *write_p = p;
    char *name_p = p + 4;
    p = strchr(name_p, '\n');
    size_t write_len = p - write_p;
    size_t name_len = p - name_p;

    while (*p == '\n') {
      char *name_p2 = p + 1;
      size_t ins_len = strcspn(name_p2, ":;#\n");
      if (name_p2[ins_len] != ':')
        break;
      p = name_p2 + ins_len + 1;
      if (ins_len == name_len && !memcmp(name_p2, name_p, name_len)) {
        memset(write_p, ' ', write_len);
        break;
      }
    }
  }
}

static void mark_live(Obj *var) {
  if (var->is_live)
    return;
  var->is_live = true;

  if (!var->is_definition) {
    if (var->alias_name) {
      Obj *alias = get_symbol_var(var->alias_name);
      if (alias && alias->is_definition)
        mark_live(alias);
    }
    return;
  }

  if (var->ty->kind == TY_FUNC) {
    for (int i = 0; i < var->output->ref_cnt; i++)
      mark_live(var->output->refs[i]);
  } else {
    for (Relocation *rel = var->rel; rel; rel = rel->next)
      if (rel->var)
        mark_live(rel->var);
  }
}

// 何度もコンパイルするとき（libslimcc.c）に、前回の状態を消す
static void ir_free_all(void);  // ir.inc

void codegen_reset(void) {
  // 中間表現の確保（関数 1 つ分）。関数の途中でエラー・中断で抜けると残り、ir_bump がゾーンの捨てた塊を指したままになる。
  // 使い回しの塊もゾーンのものなので忘れる（Linux 用のドライバでは解放せずに残る。1 回のコンパイルで終わるため）
  ir_rec_abort();
  ir_free_all();
  ir_pool_forget();
  if (in_emit_text) {
    fclose(output_file);
    in_emit_text = false;
  }
  output_file = NULL;
  for (int i = 0; i < emitted_len; i++)
    free(emitted[i]->buf);
  free(emitted);
  emitted = NULL;
  emitted_len = emitted_cap = 0;
  free(bitint_fn_map.buckets);
  bitint_fn_map = (HashMap){0};
  free(debug_file_used);
  free(debug_file_id);
  debug_file_used = NULL;
  debug_file_id = NULL;
  free(debug_files.data);
  debug_files = (StringArray){0};
  free(tmp_stack.data);
  tmp_stack.data = NULL;
  tmp_stack.capacity = tmp_stack.depth = 0;
  codegen_fn = NULL;
  label_count = 1;
}

int codegen(Obj *prog, FILE *out) {
  output_file = out;

  if (opt_g)
    for (int i = 0; i < debug_files.len; i++)
      Printftn(".file %d \"%s\"", i, debug_files.data[i]);

  for (Obj *var = prog; var; var = var->next) {
    if (var->is_definition && var->ty->kind == TY_FUNC && !var->is_static && !var->export_fn) {
      if (var->is_always_inline && opt_fake_always_inline) {
        var->is_static = true;
        char *p = ((FuncObj *)var->output)->buf;
        if ((p = strstr(p, ".globl ")))
          memcpy(p, ".local", 6);
        continue;
      }
      var->is_definition = false;
      free(((FuncObj *)var->output)->buf);
      ((FuncObj *)var->output)->buf = NULL;
      var->output = NULL;
      continue;
    }
    if (var->alias_name)
      var->is_definition = false;
  }

  for (Obj *var = prog; var; var = var->next)
    if ((!var->is_static || var->is_used) && (var->is_definition || var->alias_name))
      mark_live(var);

  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_definition) {
      if (var->is_live && (var->alias_name || var->visibility || var->is_weak))
        emit_symbol2(var, asm_name(var), var->visibility);
      continue;
    }
    if (var->ty->kind == TY_ASM) {
      Printfsn("%s", var->asm_name);
      continue;
    }
    if (var->ty->kind != TY_ARRAY && var->ty->size < 0)
      error("object \'%s\' has incomplete type", get_symbol(var));
    if (!var->is_live)
      continue;
    if (var->ty->kind == TY_FUNC) {
      FuncObj *fn = var->output;
      peep_redunt_jmp(fn->buf);
      fwrite(fn->buf, 1, fn->buflen, out);
      continue;
    }
    emit_data(var);
    continue;
  }
  Printstn(".section  .note.GNU-stack,\"\",@progbits");

  // 関数ごとの出力（open_memstream のバッファ）は codegen_reset で解放する
  return 0;
}

// 中間表現（SSA）を使うコード生成（-fslimcc-ir）
#include "ir.inc"

// 命令の並べ替え（-fschedule-insns2）
#include "sched.inc"
