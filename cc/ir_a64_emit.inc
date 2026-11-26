// slimcc-a64: 中間表現の関数から AArch64 の命令を出す（ir.inc が取り込む。2026-11-27 に ir.inc から分けた）
//
// 命令の記録（IrRec・AOp。内蔵アセンブラがかけらとして読む）、tbz の届く範囲の書き直し、命令ごとの出力
// （ir_emit_inst1）、比較と分岐・csel・tbz をまとめる印（ir_mark_fused）、フレームの入口と出口（ir_emit_fn2）。
// ターゲットの記述（レジスタ・即値）は ir_a64.inc。

//
// 出力
//

TLS static int64_t ir_label_base;
TLS static int ir_save_ofs; // 退避したレジスタの場所（x29 からの負のオフセットの先頭）

static int ir_rec_print(char *b, const IrRec *r);
static void ir_emit_fn2(Obj *fn);

// ir_emit_fn: 出力を記録にためる（emit_out が ir_rec_text を呼ぶ）
TLS static IrRec *ir_recs;
TLS static int ir_nrecs, ir_caprecs;
TLS bool ir_rec_on;                 // emit_out が記録に足す（codegen.c）
TLS static char *ir_rtext, *ir_rtext_end;
TLS static char *ir_rpart;          // 改行で終わっていない行の続き
TLS static int ir_rpart_len, ir_rpart_cap;

static char *ir_rec_store(const char *s, int len) {
  if (len + 1 > ir_rtext_end - ir_rtext) {
    size_t sz = len + 1 > 16384 ? (size_t)len + 1 : 16384;
    ir_rtext = ir_alloc_nz(sz);
    ir_rtext_end = ir_rtext + sz;
  }
  char *t = ir_rtext;
  memcpy(t, s, len);
  t[len] = '\0';
  ir_rtext += len + 1;
  return t;
}

// 記録の配列は関数をまたいで使い回す（0 にせず、広げるときだけ写す。2026-11-16。前は関数ごとに 4 から倍々に
// 広げ、そのたびに 0 にして写していた）。ゾーンのものなので、コンパイルの終わり（codegen_reset）に忘れる
static void ir_rec_reserve(void) {
  if (ir_nrecs < ir_caprecs)
    return;
  ir_caprecs = ir_caprecs ? ir_caprecs * 2 : 1024;
  ir_recs = realloc(ir_recs, sizeof(IrRec) * ir_caprecs);
  ir_recs_keep = ir_recs;
  ir_capkeep = ir_caprecs;
}

static void ir_rec_push(const char *s, int len, int kind) {
  ir_rec_reserve();
  ir_recs[ir_nrecs++] = (IrRec){len ? ir_rec_store(s, len) : "", len, kind};
}

// 書式済みの文字列を行に分けて記録する（emit_out から）
void ir_rec_text(const char *s, size_t n) {
  while (n) {
    const char *nl = memchr(s, '\n', n);
    size_t l = nl ? (size_t)(nl - s) : n;
    if (!nl || ir_rpart_len) {
      // 行の途中（続きは次の呼び出しで来る）か、前の途中の続き
      if (ir_rpart_len + (int)l > ir_rpart_cap) {
        int cap = (ir_rpart_len + (int)l) * 2 + 64;
        char *np = ir_alloc(cap);
        memcpy(np, ir_rpart, ir_rpart_len);
        ir_rpart = np;
        ir_rpart_cap = cap;
      }
      memcpy(ir_rpart + ir_rpart_len, s, l);
      ir_rpart_len += (int)l;
      if (!nl)
        return;
      ir_rec_push(ir_rpart, ir_rpart_len, IRR_LINE);
      ir_rpart_len = 0;
    } else {
      ir_rec_push(s, (int)l, IRR_LINE);
    }
    s = nl + 1;
    n -= l + 1;
  }
}

// 記録に移さず output_file（ir_emit_fn の間は作業用の文字列）に書かれたものを記録に移す
TLS static char *ir_side_buf;
TLS static size_t ir_side_len;

static void ir_rec_take_side(void) {
  fflush(output_file);
  long n = ftell(output_file);
  if (n > 0) {
    ir_rec_text(ir_side_buf, (size_t)n);
    fseek(output_file, 0, SEEK_SET);
  }
}

// --- かけら（段階 2。2026-11-16）: 中間表現の関数の記録を、コンパイルの終わりまで残る場所（ゾーンの外）に
// 写し、アセンブリの文字列には「.slimcc_frag 番号」だけを書く。内蔵アセンブラが記録を読んで機械語にする
// （文字列を作って読み直さない）。同じプロセスで .o まで作るときだけ（opt_direct。libslimcc と、
// Linux 用のドライバの -c）。-S などでは今までどおり文字列を書く
TLS bool opt_direct;
TLS static SlimccFrag *frags;
TLS static int nfrags, capfrags;
TLS static const IrRec **frag_order;    // ir_rec_write が集める、並べ替えた後の順
TLS static int nfrag_order, capfrag_order;

static void frag_collect(const IrRec *r) {
  if (nfrag_order == capfrag_order) {
    // 関数をまたいで使い回す（ir_recs と同じ。コンパイルの終わりに ir_pool_forget で忘れる）
    capfrag_order = capfrag_order ? capfrag_order * 2 : 1024;
    frag_order = realloc(frag_order, sizeof(*frag_order) * capfrag_order);
    frag_order_keep = frag_order;
    capfrag_keep = capfrag_order;
  }
  frag_order[nfrag_order++] = r;
}

// かけらの記録の数の合計（内蔵アセンブラが記号の表の大きさを見込む）
int slimcc_frag_nrecs(void) {
  int n = 0;
  for (int i = 0; i < nfrags; i++)
    n += frags[i].n;
  return n;
}

const SlimccFrag *slimcc_frag_get(int id) {
  return id >= 0 && id < nfrags ? &frags[id] : NULL;
}

void slimcc_frag_free_all(void) {
  for (int i = 0; i < nfrags; i++)
    zone_heap_free(frags[i].recs);
  zone_heap_free(frags);
  frags = NULL;
  nfrags = capfrags = 0;
}

int slimcc_ao_print(char *b, const AOp *o) {
  return ao_print(b, o);
}

// frag_order の記録を 1 つのかけらに写す（文字列も同じ塊に）
static int frag_make(void) {
  size_t strs = 0;
  for (int i = 0; i < nfrag_order; i++) {
    const IrRec *r = frag_order[i];
    if (r->kind == IRR_LINE)
      strs += r->len + 1;
    for (int k = 0; r->kind == IRR_INSN && k < r->nop; k++)
      if ((r->op[k].kind == AO_TEXT || r->op[k].kind == AO_SYM ||
           (r->op[k].kind == AO_MEM && r->op[k].mode == AOM_GOTLO)) && r->op[k].s)
        strs += strlen(r->op[k].s) + 1;
  }
  IrRec *recs = zone_heap_malloc(sizeof(IrRec) * nfrag_order + strs + 1);
  char *pool = (char *)(recs + nfrag_order);
  for (int i = 0; i < nfrag_order; i++) {
    IrRec *r = &recs[i];
    *r = *frag_order[i];
    if (r->kind == IRR_LINE) {
      memcpy(pool, r->s, r->len);
      pool[r->len] = '\0';
      r->s = pool;
      pool += r->len + 1;
    }
    for (int k = 0; r->kind == IRR_INSN && k < r->nop; k++) {
      AOp *o = &r->op[k];
      if ((o->kind == AO_TEXT || o->kind == AO_SYM || (o->kind == AO_MEM && o->mode == AOM_GOTLO)) && o->s) {
        size_t l = strlen(o->s);
        memcpy(pool, o->s, l + 1);
        o->s = pool;
        pool += l + 1;
      }
    }
  }
  if (nfrags == capfrags) {
    capfrags = capfrags ? capfrags * 2 : 64;
    SlimccFrag *nf = zone_heap_malloc(sizeof(SlimccFrag) * capfrags);
    if (nfrags)
      memcpy(nf, frags, sizeof(SlimccFrag) * nfrags);
    zone_heap_free(frags);
    frags = nf;
  }
  frags[nfrags] = (SlimccFrag){recs, nfrag_order};
  return nfrags++;
}

// 記録を書く（ループの中の範囲は並べ替える。並べ替えは行の文字列を読むので、型付きの記録は文字列にして渡す）
static void ir_rec_write_text(FILE *out);

// 分岐の行き先（b だけ。ほかは NULL）。名前の長さを *len に
static const char *frag_jmp_target(const IrRec *r, size_t *len) {
  if (r->kind == IRR_INSN) {
    if (r->nop != 1 || r->op[0].kind != AO_TEXT || strcmp(r->mn, "b"))
      return NULL;
    *len = strlen(r->op[0].s);
    return r->op[0].s;
  }
  if (r->kind == IRR_LINE && r->len > 3 && !memcmp(r->s, "\tb ", 3)) {
    *len = r->len - 3;
    return r->s + 3;
  }
  return NULL;
}

// codegen.c の peep_redunt_jmp と同じ: 「b L」のすぐ後に続くラベルの行に L があれば、その b を消す
// （文字列の道では関数の文字列全体にかかるので、かけらにも同じことをして結果をそろえる）
static void frag_peep_jmp(void) {
  int j = 0;
  for (int i = 0; i < nfrag_order; i++) {
    size_t nlen;
    const char *name = frag_jmp_target(frag_order[i], &nlen);
    bool drop = false;
    for (int k = i + 1; name && k < nfrag_order; k++) {
      const IrRec *l = frag_order[k];
      if (l->kind != IRR_LINE)
        break;
      size_t n = strcspn(l->s, ":;#\n");
      if ((int)n >= l->len || l->s[n] != ':')
        break;
      if (n == nlen && !memcmp(l->s, name, n)) {
        drop = true;
        break;
      }
    }
    if (!drop)
      frag_order[j++] = frag_order[i];
  }
  nfrag_order = j;
}

// tbz・tbnz の飛べる距離は ±32KB。関数の大きさの見積もり（記録 1 つを 1 命令、文字列の行は 1 行を 1 命令。
// ラベル・ディレクティブも数えるので多めに出る）が 28KB 以上なら、全部の tbz・tbnz を「逆の条件で次の命令を
// 飛び越し、b で飛ぶ」形にする（2026-11-27）
TLS static int ir_ntbz;     // この関数で出した tbz・tbnz の数

static void ir_relax_tbz(void) {
  int ntbz = ir_ntbz;
  ir_ntbz = 0;
  if (!ntbz || ir_nrecs * 4 < 28 * 1024)
    return;
  int n = ir_nrecs;
  for (int k = 0; k < 2 * ntbz; k++)
    ir_rec_reserve(), ir_nrecs++;
  int j = ir_nrecs;
  for (int i = n - 1; i >= 0; i--) {
    IrRec r = ir_recs[i];
    if (r.kind != IRR_INSN || (strcmp(r.mn, "tbz") && strcmp(r.mn, "tbnz"))) {
      ir_recs[--j] = r;
      continue;
    }
    char lb[64];
    int len = snprintf(lb, sizeof(lb), ".L.ir.tbz.%" PRIi64, count());
    char *name = ir_rec_store(lb, len);
    lb[len++] = ':';
    ir_recs[--j] = (IrRec){ir_rec_store(lb, len), len, IRR_LINE};
    IrRec b = r;
    b.mn = "b";
    b.nop = 1;
    b.op[0] = r.op[2];
    b.op[1] = b.op[2] = b.op[3] = (AOp){0};
    ir_recs[--j] = b;
    r.mn = !strcmp(r.mn, "tbz") ? "tbnz" : "tbz";
    r.op[2] = (AOp){.kind = AO_TEXT, .s = name};
    ir_recs[--j] = r;
  }
  if (j != 0)
    internal_error();
}

static void ir_rec_write(FILE *out) {
  if (!opt_direct) {
    ir_rec_write_text(out);
    return;
  }
  if (ir_rpart_len)
    internal_error();
  // 並べ替えた後の順に記録を集めて、かけらにする
  frag_order = frag_order_keep;
  capfrag_order = capfrag_keep;
  nfrag_order = 0;
  char line[1024];
  for (int i = 0; i < ir_nrecs; i++) {
    IrRec *r = &ir_recs[i];
    if (r->kind == IRR_HOT_BEGIN) {
      int j = i + 1;
      while (j < ir_nrecs && ir_recs[j].kind != IRR_HOT_END)
        j++;
      for (int k = i + 1; k < j; k++)
        if (ir_recs[k].kind == IRR_INSN) {
          int n = ir_rec_print(line, &ir_recs[k]);
          ir_recs[k].s = ir_rec_store(line, n);
          ir_recs[k].len = n;
        }
      sch_rec = frag_collect;
      sch_lines(ir_recs + i + 1, j - i - 1, out);
      sch_rec = NULL;
      i = j;
      continue;
    }
    if (r->kind == IRR_INSN || r->kind == IRR_LINE)
      frag_collect(r);
  }
  frag_peep_jmp();
  {
    char line[32];
    memcpy(line, "\t.slimcc_frag ", 14);
    int n = 14 + ao_putd(line + 14, frag_make());
    line[n++] = '\n';
    fwrite(line, 1, n, out);
  }
}

static void ir_rec_write_text(FILE *out) {
  if (ir_rpart_len)
    internal_error();
  char line[1024];
  for (int i = 0; i < ir_nrecs; i++) {
    IrRec *r = &ir_recs[i];
    if (r->kind == IRR_HOT_BEGIN) {
      int j = i + 1;
      while (j < ir_nrecs && ir_recs[j].kind != IRR_HOT_END)
        j++;
      for (int k = i + 1; k < j; k++)
        if (ir_recs[k].kind == IRR_INSN) {
          int n = ir_rec_print(line, &ir_recs[k]);
          ir_recs[k].s = ir_rec_store(line, n);
          ir_recs[k].len = n;
        }
      sch_lines(ir_recs + i + 1, j - i - 1, out);
      i = j;
      continue;
    }
    if (r->kind == IRR_INSN) {
      int n = ir_rec_print(line, r);
      line[n++] = '\n';
      fwrite(line, 1, n, out);
      continue;
    }
    if (r->kind != IRR_LINE)
      continue;
    fwrite(r->s, 1, r->len, out);
    fputc('\n', out);
  }
}

static bool ir_nsw_w(IrInst *in) {
  if (!in->nsw)
    return false;
  return in->low32 || in->op == IR_SDIV || in->op == IR_SREM || (in->a && in->a->low32) ||
         (in->b && in->b->low32);
}

// --- 型付きの記録（段階 1b。2026-11-16） ---
// 中間表現の出力は、命令の名前とオペランド（レジスタ・即値・番地・記号）の記録を作る。文字列にするのは
// 書き出すとき（ao_print・ir_rec_print）だけで、今までの Printftn と 1 文字も違わない文字列になる。

static AOp aR(char c, int r) { return (AOp){.kind = AO_REG, .c = c, .r = (uint8_t)r}; }
static AOp aX(int r) { return aR('x', r); }
static AOp aSP(void) { return aR('x', 32); }
static AOp aXZR(void) { return aR('x', 31); }
static AOp aI(int64_t v) { return (AOp){.kind = AO_IMM, .imm = v}; }
static AOp aFZero(void) { return (AOp){.kind = AO_FZERO}; }
static AOp aShift(const char *name, int n) { return (AOp){.kind = AO_SHIFT, .s = name, .sh = (uint8_t)n}; }
static AOp aCond(int c) { return (AOp){.kind = AO_COND, .imm = c}; }
static AOp aSym(const char *name, int mode) { return (AOp){.kind = AO_SYM, .s = name, .mode = (uint8_t)mode}; }
static AOp aMem(AOp base, int64_t off) { return (AOp){.kind = AO_MEM, .mode = AOM_OFF, .r = base.r, .imm = off}; }
static AOp aMem0(AOp base, int64_t off) { return (AOp){.kind = AO_MEM, .mode = AOM_OFF0, .r = base.r, .imm = off}; }
static AOp aMemIdx(AOp base, AOp idx, int sh) {
  return (AOp){.kind = AO_MEM, .mode = sh ? AOM_IDXLSL : AOM_IDX, .r = base.r, .idx = idx.r, .sh = (uint8_t)sh};
}
static AOp aMemPre(AOp base, int64_t off) { return (AOp){.kind = AO_MEM, .mode = AOM_PRE, .r = base.r, .imm = off}; }
static AOp aMemPost(AOp base, int64_t off) { return (AOp){.kind = AO_MEM, .mode = AOM_POST, .r = base.r, .imm = off}; }
static AOp aMemGot(AOp base, const char *sym) {
  return (AOp){.kind = AO_MEM, .mode = AOM_GOTLO, .r = base.r, .s = sym};
}
// ラベルなど（記録の文字列に写す）
static AOp aText(const char *s) { return (AOp){.kind = AO_TEXT, .s = ir_rec_store(s, (int)strlen(s))}; }
// 浮動小数点のレジスタ（fsz 4 は s、8 は d）
static AOp aF(int n, int fsz) { return aR(fsz == 4 ? 's' : fsz == 8 ? 'd' : 'q', n); }
// x のレジスタを w に
static AOp aWof(AOp r) {
  r.c = 'w';
  return r;
}
static bool ao_same(AOp a, AOp b) { return a.kind == AO_REG && b.kind == AO_REG && a.c == b.c && a.r == b.r; }

// レジスタの名前（文字列の出力を使う補助関数に渡すため。表の中の文字列なので消えない）
static const char *ao_name(AOp o) {
  static const char *const wreg[] = {
    "w0",  "w1",  "w2",  "w3",  "w4",  "w5",  "w6",  "w7",  "w8",  "w9",  "w10",
    "w11", "w12", "w13", "w14", "w15", "w16", "w17", "w18", "w19", "w20", "w21",
    "w22", "w23", "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wzr", "wsp",
  };
  if (o.c == 'x')
    return o.r == 31 ? "xzr" : o.r == 32 ? "sp" : xreg[o.r];
  if (o.c == 'w')
    return wreg[o.r];
  return freg(o.r, o.c == 's' ? 4 : o.c == 'd' ? 8 : 16);
}

// 文字列・10 進の数を b に書く（sprintf は picolibc の vfprintf を通って遅い）
static int ao_puts(char *b, const char *s) {
  int l = (int)strlen(s);
  memcpy(b, s, l);
  return l;
}

static int ao_putd(char *b, int64_t v) {
  char d[24];
  int k = 0, n = 0;
  uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
  do {
    d[k++] = '0' + u % 10;
    u /= 10;
  } while (u);
  if (v < 0)
    b[n++] = '-';
  while (k)
    b[n++] = d[--k];
  return n;
}

static int ao_print(char *b, const AOp *o) {
  int n = 0;
  switch (o->kind) {
  case AO_REG:
    return ao_puts(b, ao_name(*o));
  case AO_IMM:
    b[n++] = '#';
    return n + ao_putd(b + n, o->imm);
  case AO_FZERO:
    return ao_puts(b, "#0.0");
  case AO_SHIFT:
    n = ao_puts(b, o->s);
    b[n++] = ' ';
    b[n++] = '#';
    return n + ao_putd(b + n, o->sh);
  case AO_COND:
    return ao_puts(b, cc_name[o->imm]);
  case AO_TEXT:
    return ao_puts(b, o->s);
  case AO_SYM:
    if (o->mode == AS_LO12)
      n = ao_puts(b, ":lo12:");
    else if (o->mode == AS_GOT)
      n = ao_puts(b, ":got:");
    else if (o->mode == AS_TPHI)
      n = ao_puts(b, ":tprel_hi12:");
    else if (o->mode == AS_TPLO)
      n = ao_puts(b, ":tprel_lo12_nc:");
    b[n++] = '"';
    n += ao_puts(b + n, o->s);
    b[n++] = '"';
    return n;
  case AO_MEM: {
    b[n++] = '[';
    n += ao_puts(b + n, ao_name(aX(o->r)));
    switch (o->mode) {
    case AOM_OFF:
    case AOM_OFF0:
      if (o->imm || o->mode == AOM_OFF0) {
        n += ao_puts(b + n, ", #");
        n += ao_putd(b + n, o->imm);
      }
      b[n++] = ']';
      return n;
    case AOM_IDX:
    case AOM_IDXLSL:
      n += ao_puts(b + n, ", ");
      n += ao_puts(b + n, ao_name(aX(o->idx)));   // 31 は xzr（添字が定数 0 のとき）
      if (o->mode == AOM_IDXLSL) {
        n += ao_puts(b + n, ", lsl #");
        n += ao_putd(b + n, o->sh);
      }
      b[n++] = ']';
      return n;
    case AOM_PRE:
      n += ao_puts(b + n, ", #");
      n += ao_putd(b + n, o->imm);
      return n + ao_puts(b + n, "]!");
    case AOM_POST:
      n += ao_puts(b + n, "], #");
      return n + ao_putd(b + n, o->imm);
    case AOM_GOTLO:
      n += ao_puts(b + n, ", :got_lo12:\"");
      n += ao_puts(b + n, o->s);
      return n + ao_puts(b + n, "\"]");
    }
  }
  }
  internal_error();
}

// 型付きの記録を 1 行にする（タブから。改行なし）。長さを返す
static int ir_rec_print(char *b, const IrRec *r) {
  int n = 0;
  b[n++] = '\t';
  int l = (int)strlen(r->mn);
  memcpy(b + n, r->mn, l);
  n += l;
  for (int i = 0; i < r->nop; i++) {
    if (i) {
      b[n++] = ',';
      b[n++] = ' ';
    } else {
      b[n++] = ' ';
    }
    n += ao_print(b + n, &r->op[i]);
  }
  b[n] = '\0';
  return n;
}

static void ir_insn(const char *mn, int nop, AOp a, AOp b, AOp c, AOp d) {
  ir_rec_reserve();
  IrRec *r = &ir_recs[ir_nrecs++];
  r->kind = IRR_INSN;
  r->mn = mn;
  r->nop = (uint8_t)nop;
  r->s = NULL;
  r->len = 0;
  r->op[0] = a;
  r->op[1] = b;
  r->op[2] = c;
  r->op[3] = d;
}
#define AO_NONE ((AOp){0})
#define E0(mn) ir_insn(mn, 0, AO_NONE, AO_NONE, AO_NONE, AO_NONE)
#define E1(mn, a) ir_insn(mn, 1, a, AO_NONE, AO_NONE, AO_NONE)
#define E2(mn, a, b) ir_insn(mn, 2, a, b, AO_NONE, AO_NONE)
#define E3(mn, a, b, c) ir_insn(mn, 3, a, b, c, AO_NONE)
#define E4(mn, a, b, c, d) ir_insn(mn, 4, a, b, c, d)

// 型付きの補助（codegen.c の mov_imm・add_imm・mem_op・gen_sym_addr と同じ命令を出す）
static void ir_mov_imm(AOp reg, int64_t v) {
  bool is64 = reg.c == 'x';
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
        E3("movn", reg, aI((uint16_t)~c), aShift("lsl", 16 * i));
      else
        E3("movk", reg, aI(c), aShift("lsl", 16 * i));
      first = false;
    }
    if (first)
      E2("movn", reg, aI(0));
    return;
  }
  for (int i = 0; i < n; i++) {
    uint16_t c = val >> (16 * i);
    if (c == 0)
      continue;
    if (first)
      E3("movz", reg, aI(c), aShift("lsl", 16 * i));
    else
      E3("movk", reg, aI(c), aShift("lsl", 16 * i));
    first = false;
  }
  if (first)
    E2("movz", reg, aI(0));
}

static void ir_add_imm(AOp dst, AOp src, int64_t val) {
  if (val == 0) {
    if (!ao_same(dst, src))
      E2("mov", dst, src);
    return;
  }
  const char *op = val < 0 ? "sub" : "add";
  uint64_t a = val < 0 ? -(uint64_t)val : (uint64_t)val;
  if (a < 4096) {
    E3(op, dst, src, aI((int64_t)a));
    return;
  }
  if (a < (1 << 24)) {
    E4(op, dst, src, aI((int64_t)(a >> 12)), aShift("lsl", 12));
    if (a & 0xFFF)
      E3(op, dst, dst, aI((int64_t)(a & 0xFFF)));
    return;
  }
  AOp tmp = ao_same(src, aX(16)) ? aX(17) : aX(16);
  ir_mov_imm(tmp, (int64_t)a);
  E3(op, dst, src, tmp);
}

static AOp ir_memop(int64_t ofs, AOp base, int sz, AOp scratch) {
  if ((ofs >= -256 && ofs <= 255) || (ofs >= 0 && ofs % sz == 0 && ofs / sz <= 4095))
    return aMem(base, ofs);
  ir_add_imm(scratch, base, ofs);
  return aMem(scratch, 0);
}

// フレームの読み書き（局所変数・逃がした値。x29 + ofs、ofs は負）を 1 命令で（2026-11-21）。x29 からは
// -256 までしか届かず、遠いと sub と ldr の 2 命令になっていた（QuickJS の JS_CallInternal で命令の 1/4）。
// フレームの底（x29 - frame）を土台にすれば正のオフセットで、8 バイトなら 32KB まで 1 命令。土台は sp
// （関数の中で動かないとき。スタックの引数を書く間は ir_sp_adj だけ下がっている）か、alloca のある関数は
// x28（入口で mov x28, sp。割り当てない）
TLS static AOp ir_fbase;          // 土台（ir_fbase_on のとき）
TLS static bool ir_fbase_on;
TLS static int64_t ir_fbase_frame; // 土台は x29 - これ
TLS static int64_t ir_sp_adj;      // 土台が sp のとき、今 sp を下げている量

static AOp ir_fmem(int64_t ofs, int sz, AOp scratch) {
  if (ir_fbase_on) {
    int64_t x = ofs + ir_fbase_frame + (ir_fbase.r == 32 ? ir_sp_adj : 0);
    if (x >= 0 && x % sz == 0 && x / sz <= 4095)
      return aMem(ir_fbase, x);
  }
  return ir_memop(ofs, aX(29), sz, scratch);
}

static void ir_e_symaddr(AOp reg, Obj *var) {
  const char *sym = get_symbol(var);
  if (use_direct(var)) {
    E2("adrp", reg, aSym(sym, AS_PLAIN));
    E3("add", reg, reg, aSym(sym, AS_LO12));
  } else {
    E2("adrp", reg, aSym(sym, AS_GOT));
    E2("ldr", reg, aMemGot(reg, sym));
  }
}

// 関数を呼ぶ（直接か、GOT から）
static void ir_call_sym(Obj *fn) {
  if (use_direct(fn) || opt_use_plt) {
    E1("bl", aSym(get_symbol(fn), AS_PLAIN));
  } else {
    E2("adrp", aX(17), aSym(get_symbol(fn), AS_GOT));
    E2("ldr", aX(17), aMemGot(aX(17), get_symbol(fn)));
    E1("blr", aX(17));
  }
}

// 中間表現の値のレジスタ（型付き。ir_src・ir_dst などと同じ命令を出す）
// 値のレジスタの番号 → 本当の番号（IR_X0 は x0）
static int ir_phys(int r) { return r == IR_X0 ? 0 : r; }

static AOp ir_srcA(IrInst *v, AOp scratch) {
  if (v->reg)
    return aX(ir_phys(v->reg));
  if (v->op == IR_CONST && v->imm == 0)
    return aXZR();
  if (!v->spill) {
    if (v->op == IR_CONST) {
      ir_mov_imm(scratch, v->imm);
      return scratch;
    }
    internal_error();
  }
  E2("ldr", scratch, ir_fmem(v->spill, 8, scratch));
  return scratch;
}

static AOp ir_base_srcA(IrInst *v, AOp scratch) {
  AOp r = ir_srcA(v, scratch);
  if (r.r == 31) {
    E2("mov", scratch, aI(0));
    return scratch;
  }
  return r;
}

static AOp ir_dstA(IrInst *v) { return v->reg ? aX(ir_phys(v->reg)) : aX(16); }

// 並列の写し（2026-11-27）: 本当の番号 dst[i]（x0〜x7）へ値 src[i] を（imm[i] なら定数 src[i]->imm を作る）。
// 値が x0〜x7 にもあるので、写す先がまだ読んでいない元なら後回しにし、輪は x16 で切る
static void ir_par_move(int n, const int *dst, IrInst *const *src, const bool *imm) {
  int sreg[16];
  bool done[16];
  int left = 0;
  for (int i = 0; i < n; i++) {
    sreg[i] = !imm[i] && src[i]->reg ? ir_phys(src[i]->reg) : -1;
    done[i] = sreg[i] == dst[i];
    left += !done[i];
  }
  while (left) {
    bool progress = false;
    for (int i = 0; i < n; i++) {
      if (done[i])
        continue;
      bool blocked = false;
      for (int j = 0; j < n && !blocked; j++)
        blocked = j != i && !done[j] && sreg[j] == dst[i];
      if (blocked)
        continue;
      AOp d = aX(dst[i]);
      if (sreg[i] >= 0) {
        E2("mov", d, aX(sreg[i]));
      } else if (imm[i]) {
        ir_mov_imm(d, src[i]->imm);
      } else {
        AOp v = ir_srcA(src[i], d);
        if (!ao_same(v, d))
          E2("mov", d, v);
      }
      done[i] = true;
      left--;
      progress = true;
    }
    if (progress)
      continue;
    // 輪: 写す先になっている元を 1 つ x16 に逃がす
    for (int i = 0; i < n; i++) {
      if (done[i] || sreg[i] < 0)
        continue;
      bool is_dst = false;
      for (int j = 0; j < n; j++)
        is_dst |= !done[j] && dst[j] == sreg[i];
      if (!is_dst)
        continue;
      int r = sreg[i];
      E2("mov", aX(16), aX(r));
      for (int j = 0; j < n; j++)
        if (!done[j] && sreg[j] == r)
          sreg[j] = 16;
      break;
    }
  }
}


static void ir_finish_dstA(IrInst *v) {
  if (v->reg || !v->spill)
    return;
  E2("str", aX(16), ir_fmem(v->spill, 8, aX(17)));
}

static AOp ir_fsrcA(IrInst *v, int scratch) {
  if (v->reg)
    return aF(v->reg - 32, v->fsz);
  if (!v->spill)
    internal_error();
  AOp r = aF(scratch, v->fsz);
  E2("ldr", r, ir_fmem(v->spill, v->fsz, aX(16)));
  return r;
}

static AOp ir_fdstA(IrInst *v) { return v->reg ? aF(v->reg - 32, v->fsz) : aF(30, v->fsz); }

static void ir_finish_fdstA(IrInst *v) {
  if (v->reg || !v->spill)
    return;
  E2("str", aF(30, v->fsz), ir_fmem(v->spill, v->fsz, aX(16)));
}

static void ir_emit_extA(AOp d, AOp s, int size, bool sign) {
  if (sign) {
    E2(size == 1 ? "sxtb" : size == 2 ? "sxth" : "sxtw", d, aWof(s));
  } else if (size == 4) {
    E2("mov", aWof(d), aWof(s));
  } else {
    E2(size == 1 ? "uxtb" : "uxth", aWof(d), aWof(s));
  }
}

// ブロックのラベル（.L.ir.番号.ブロック）。長さを返す（snprintf を通さない。2026-11-16）
static int ir_label(char *buf, IrBlock *bb) {
  memcpy(buf, ".L.ir.", 6);
  int n = 6 + ao_putd(buf + 6, ir_label_base);
  buf[n++] = '.';
  n += ao_putd(buf + n, bb->id);
  buf[n] = '\0';
  return n;
}

static AOp ir_labelA(IrBlock *bb) {
  char buf[STRBUF_SZ];
  int n = ir_label(buf, bb);
  return (AOp){.kind = AO_TEXT, .s = ir_rec_store(buf, n)};
}


// 命令の並べ替え（sched.inc）で動かさないもの: インラインアセンブリ、volatile・アトミックの読み書き
TLS static bool ir_sched_on;    // ir_emit_fn が出力を取り込んでいる

static bool ir_sched_fixed(IrInst *in) {
  switch (in->op) {
  case IR_ASM: case IR_ASMOUT: case IR_CAS: case IR_XCHG: case IR_ALLOCA:
    return true;
  case IR_LOAD: case IR_STORE:
    return in->vol || in->acq;
  default:
    return false;
  }
}

static void ir_emit_inst1(IrInst *in, IrBlock *bb, IrBlock *next_bb);
static void ir_rec_take_side(void);

static void ir_emit_inst(IrInst *in, IrBlock *bb, IrBlock *next_bb) {
  bool fixed = ir_sched_on && ir_sched_fixed(in);
  if (fixed)
    Printstn(SCH_FIXED_BEGIN);
  if (in->op == IR_ASM) {
    // インラインアセンブリの本体は output_file に直接書かれるので、書かせてから記録に移す
    ir_rec_on = false;
    ir_emit_inst1(in, bb, next_bb);
    ir_rec_take_side();
    ir_rec_on = true;
  } else {
    ir_emit_inst1(in, bb, next_bb);
  }
  if (fixed)
    Printstn(SCH_FIXED_END);
}

static bool ir_emits_nothing(IrInst *in);

// 並びの i 番目のブロックの終わりから落ちて着くブロック（間の、何も出さずに次へ落ちるだけのブロックは通る）
static IrBlock *ir_fallthrough(int i) {
  int j = i + 1;
  while (j + 1 < ir_norder) {
    IrBlock *b = ir_order[j];
    if (!b->last || b->last->op != IR_BR || b->succ[0] != ir_order[j + 1])
      break;
    IrInst *in = b->first;
    while (in != b->last && ir_emits_nothing(in))
      in = in->next;
    if (in != b->last)
      break;
    j++;
  }
  return j < ir_norder ? ir_order[j] : NULL;
}

// 何も出さない命令（phi、同じレジスタへのコピー、使われないコピー、即値の定数）
static bool ir_emits_nothing(IrInst *in) {
  switch (in->op) {
  case IR_PHI:
    return true;
  case IR_COPY: {
    IrInst *dst = in->b ? in->b : in;
    if (!dst->reg && !dst->spill)
      return true;
    return dst->reg && in->a->reg == dst->reg && !in->a->spill;
  }
  case IR_CONST: case IR_UNDEF: case IR_FCONST:
    return !in->reg && !in->spill;
  default:
    return false;
  }
}

// 中身が分岐だけのブロック（何も出さない命令はあってよい）をたどった先（輪でも 8 回で止める）
static IrBlock *ir_thread(IrBlock *b) {
  for (int i = 0; i < 8 && b->last && b->last->op == IR_BR; i++) {
    IrInst *in = b->first;
    while (in != b->last && ir_emits_nothing(in))
      in = in->next;
    if (in != b->last)
      break;
    b = b->succ[0];
  }
  return b;
}

static void ir_emit_inst1(IrInst *in, IrBlock *bb, IrBlock *next_bb) {
  switch (in->op) {
  case IR_PHI:
    return; // phi は前任のコピー
  case IR_PARAM:
    // 入口のブロックで、関数の呼び出しより前にあるので、x0〜x7 はまだ引数のまま
    if (!in->reg && !in->spill)
      return;
    if (in->fp) {
      AOp r = aF((int)in->imm, in->fsz);
      if (in->reg)
        E2("fmov", ir_fdstA(in), r);
      else
        E2("str", r, ir_fmem(in->spill, in->fsz, aX(16)));
      return;
    }
    if (in->reg) {
      if (ir_phys(in->reg) != in->imm)
        E2("mov", aX(ir_phys(in->reg)), aX((int)in->imm));
    } else
      E2("str", aX((int)in->imm), ir_fmem(in->spill, 8, aX(16)));
    return;
  case IR_CONST:
  case IR_UNDEF:
  case IR_FCONST:
    if (!in->reg && !in->spill)
      return; // 即値として埋め込んだ
    if (in->fp) {
      uint64_t bits = in->op == IR_FCONST ? (uint64_t)in->imm : 0;
      if (!bits) {
        E2("fmov", ir_fdstA(in), aR(in->fsz == 4 ? 'w' : 'x', 31));
      } else {
        AOp t = aR(in->fsz == 4 ? 'w' : 'x', 16);
        ir_mov_imm(t, in->fsz == 4 ? (int64_t)(uint32_t)bits : (int64_t)bits);
        E2("fmov", ir_fdstA(in), t);
      }
      ir_finish_fdstA(in);
      return;
    }
    ir_mov_imm(ir_dstA(in), in->op == IR_CONST ? in->imm : 0);
    ir_finish_dstA(in);
    return;
  case IR_COPY: {
    IrInst *dst = in->b ? in->b : in;
    if (!dst->reg && !dst->spill)
      return; // 使われない
    if (dst->fp) {
      AOp s = ir_fsrcA(in->a, 31);
      if (dst->reg) {
        AOp d = aF(dst->reg - 32, dst->fsz);
        if (!ao_same(d, s))
          E2("fmov", d, s);
      } else {
        E2("str", s, ir_fmem(dst->spill, dst->fsz, aX(16)));
      }
      return;
    }
    AOp s = ir_srcA(in->a, aX(17));
    if (dst->reg) {
      if (!ao_same(aX(ir_phys(dst->reg)), s))
        E2("mov", aX(ir_phys(dst->reg)), s);
    } else {
      E2("str", s, ir_fmem(dst->spill, 8, aX(16)));
    }
    return;
  }
  case IR_FMADD:
  case IR_FMSUB:
  case IR_FNMSUB: {
    // 入力をすべて読んでから結果を書く（1 命令）
    AOp n = ir_fsrcA(in->a, 30), m = ir_fsrcA(in->b, 31), acc = ir_fsrcA(in->c, 29);
    const char *name = in->op == IR_FMADD ? "fmadd" : in->op == IR_FMSUB ? "fmsub" : "fnmsub";
    E4(name, ir_fdstA(in), n, m, acc);
    ir_finish_fdstA(in);
    return;
  }
  case IR_FADD:
  case IR_FSUB:
  case IR_FMUL:
  case IR_FDIV: {
    static const char *const fnames[] = {[IR_FADD] = "fadd", [IR_FSUB] = "fsub",
                                         [IR_FMUL] = "fmul", [IR_FDIV] = "fdiv"};
    AOp a = ir_fsrcA(in->a, 30);
    AOp b = ir_fsrcA(in->b, 31);
    E3(fnames[in->op], ir_fdstA(in), a, b);
    ir_finish_fdstA(in);
    return;
  }
  case IR_FNEG:
  case IR_FABS:
  case IR_F2F: {
    AOp a = ir_fsrcA(in->a, 30);
    E2(in->op == IR_FNEG ? "fneg" : in->op == IR_FABS ? "fabs" : "fcvt", ir_fdstA(in), a);
    ir_finish_fdstA(in);
    return;
  }
  case IR_FSQRT: {
    if (in->sqrt_nonneg) {  // 引数が負にならない: errno を設定することはない
      AOp d = ir_fdstA(in);
      E2("fsqrt", d, ir_fsrcA(in->a, 30));
      ir_finish_fdstA(in);
      return;
    }
    // 負の数（結果が NaN）のときだけ、errno のために sqrt を呼ぶ（GCC と同じ）。
    // 結果は v30 に作る（結果のレジスタが引数と同じでも、遅い経路で引数を渡せるように）
    AOp a = ir_fsrcA(in->a, 31);
    AOp t = aF(30, in->fsz);
    int64_t c = count();
    char lb[STRBUF_SZ];
    snprintf(lb, STRBUF_SZ, ".L.ir.sqrt.%" PRIi64, c);
    E2("fsqrt", t, a);
    E2("fcmp", t, t);
    E1("b.vc", aText(lb));
    E2("fmov", aF(0, in->fsz), a);
    ir_call_sym(in->sym);
    E2("fmov", t, aF(0, in->fsz));
    Printfsn("%s:", lb);
    if (in->reg)
      E2("fmov", ir_fdstA(in), t);
    ir_finish_fdstA(in);
    return;
  }
  case IR_FCMP: {
    AOp a = ir_fsrcA(in->a, 30);
    if (ir_imm_operand(in, in->b) && !in->b->reg && !in->b->spill) {
      E2("fcmp", a, aFZero());
    } else {
      AOp b = ir_fsrcA(in->b, 31);
      E2("fcmp", a, b);
    }
    if (ir_fused[in->id])
      return;
    E2("cset", ir_dstA(in), aCond(in->cond));
    ir_finish_dstA(in);
    return;
  }
  case IR_I2F: {
    AOp d = ir_fdstA(in);
    E2(in->sign ? "scvtf" : "ucvtf", d, ir_srcA(in->a, aX(16)));
    ir_finish_fdstA(in);
    return;
  }
  case IR_F2I: {
    AOp d = ir_dstA(in);
    E2(in->sign ? "fcvtzs" : "fcvtzu", d, ir_fsrcA(in->a, 30));
    ir_finish_dstA(in);
    return;
  }
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_SDIV:
  case IR_UDIV:
  case IR_AND:
  case IR_OR:
  case IR_XOR:
  case IR_SHL:
  case IR_SHR:
  case IR_SAR: {
    static const char *const names[] = {
      [IR_ADD] = "add", [IR_SUB] = "sub", [IR_MUL] = "mul", [IR_SDIV] = "sdiv",
      [IR_UDIV] = "udiv", [IR_AND] = "and", [IR_OR] = "orr", [IR_XOR] = "eor",
      [IR_SHL] = "lsl", [IR_SHR] = "lsr", [IR_SAR] = "asr",
    };
    if (ir_fused[in->id])
      return;   // x & 2^k を分岐（tbz・tbnz）とまとめた（IR_CBR が出す）
    // add・sub の即値の形では、番号 31 は xzr ではなく sp（定数 0 の入力は mov で x16 に。2026-11-21、QuickJS）
    bool imm_b = ir_imm_operand(in, in->b);  // 定数がレジスタを持っていても即値で（2026-11-21）
    AOp a = imm_b && (in->op == IR_ADD || in->op == IR_SUB) ? ir_base_srcA(in->a, aX(16)) : ir_srcA(in->a, aX(16));
    AOp d = ir_dstA(in);
    bool w = in->wform || ir_nsw_w(in);
    if (w) {
      a = aWof(a);
      d = aWof(d);
    }
    if (imm_b && in->op == IR_MUL) {
      int k, m;
      if (ir_mul_shape(in->b->imm, &k, &m, NULL) > 0) {
        E4("add", d, a, a, aShift("lsl", k));             // a × (2^k + 1)
      } else {
        AOp t = w ? aR('w', 17) : aX(17);
        E3("lsl", t, a, aI(k));
        E3("sub", d, t, a);                               // a × (2^k - 1)
      }
      if (m)
        E3("lsl", d, d, aI(m));
    } else if (imm_b && in->b->imm < 0 && (in->op == IR_ADD || in->op == IR_SUB)) {
      E3(in->op == IR_ADD ? "sub" : "add", d, a, aI(-in->b->imm));
    } else if (imm_b) {
      E3(names[in->op], d, a, aI(in->b->imm));
    } else {
      AOp b = ir_srcA(in->b, aX(17));
      E3(names[in->op], d, a, w ? aWof(b) : b);
    }
    if (w && in->nsw && !in->low32)
      E2("sxtw", ir_dstA(in), d);
    ir_finish_dstA(in);
    return;
  }
  case IR_SREM:
  case IR_UREM: {
    AOp a = ir_srcA(in->a, aX(16));
    AOp b = ir_srcA(in->b, aX(17));
    if (ir_nsw_w(in)) {
      AOp d = ir_dstA(in);
      E3("sdiv", aR('w', 8), aWof(a), aWof(b));
      E4("msub", aWof(d), aR('w', 8), aWof(b), aWof(a));
      if (!in->low32)
        E2("sxtw", d, aWof(d));
    } else {
      E3(in->op == IR_SREM ? "sdiv" : "udiv", aX(8), a, b);
      E4("msub", ir_dstA(in), aX(8), b, a);
    }
    ir_finish_dstA(in);
    return;
  }
  case IR_NEG:
  case IR_NOT: {
    AOp d = ir_dstA(in), a = ir_srcA(in->a, aX(16));
    if (in->wform) {
      d = aWof(d);
      a = aWof(a);
    }
    E2(in->op == IR_NEG ? "neg" : "mvn", d, a);
    ir_finish_dstA(in);
    return;
  }
  case IR_EXT:
    if (in->low32 && in->size == 4) {
      // 下位 32 ビットだけが使われるので、拡張しない
      AOp d = ir_dstA(in), a = ir_srcA(in->a, aX(16));
      if (!ao_same(d, a))
        E2("mov", d, a);
      ir_finish_dstA(in);
      return;
    }
    {
      AOp d = ir_dstA(in);
      ir_emit_extA(d, ir_srcA(in->a, aX(16)), in->size, in->sign);
    }
    ir_finish_dstA(in);
    return;
  case IR_BOOL:
    if (ir_fits(in->a, 32, true) || ir_fits(in->a, 32, false))
      E2("cmp", aWof(ir_base_srcA(in->a, aX(16))), aI(0));    // cmp の即値の形も 31 は sp
    else
      E2("cmp", ir_base_srcA(in->a, aX(16)), aI(0));
    E2("cset", ir_dstA(in), aCond(CC_NE));
    ir_finish_dstA(in);
    return;
  case IR_CMP: {
    bool imm_b = ir_imm_operand(in, in->b);  // 定数がレジスタを持っていても即値で（2026-11-21）
    AOp a = imm_b ? ir_base_srcA(in->a, aX(16)) : ir_srcA(in->a, aX(16));    // 即値の形では 31 は sp
    bool w = ir_cmp_w(in);
    if (in->c) {
      // ccmp a, b|#imm5, #nzcv, ccond: ccond が成り立たなければ、cond が偽になるフラグ（ir_ccmp_chain）
      static const int8_t nzcv_false[] = {
        [CC_EQ] = 0, [CC_NE] = 4, [CC_LT] = 0, [CC_LE] = 0, [CC_GT] = 4,
        [CC_GE] = 8, [CC_LO] = 2, [CC_LS] = 2, [CC_HI] = 0, [CC_HS] = 0,
      };
      if (in->cond > CC_HS || in->ccond > CC_HS)
        internal_error();
      AOp b = imm_b && in->b->imm >= 0 && in->b->imm < 32 ? aI(in->b->imm) : ir_srcA(in->b, aX(17));
      if (w) {
        a = aWof(a);
        if (b.kind == AO_REG)
          b = aWof(b);
      }
      E4("ccmp", a, b, aI(nzcv_false[in->cond]), aCond(in->ccond));
    } else if (imm_b) {
      if (in->b->imm < 0)
        E2("cmn", w ? aWof(a) : a, aI(-in->b->imm));
      else
        E2("cmp", w ? aWof(a) : a, aI(in->b->imm));
    } else {
      AOp b = ir_srcA(in->b, aX(17));
      E2("cmp", w ? aWof(a) : a, w ? aWof(b) : b);
    }
    if (ir_fused[in->id])
      return; // 次の条件分岐が b.cond にする
    E2("cset", ir_dstA(in), aCond(in->cond));
    ir_finish_dstA(in);
    return;
  }
  case IR_SELECT: {
    // a が 0 でなければ b、0 なら c（2026-11-26）。a が直前の比較ならその条件のまま（ir_mark_fused）、
    // ほかは a を 0 と比べる（IR_CBR の cbnz と同じく、32 ビットに収まる値は w で）
    int cond = CC_NE;
    if (ir_fused[in->a->id]) {
      cond = in->a->cond;
    } else if (ir_fits(in->a, 32, true) || ir_fits(in->a, 32, false)) {
      E2("cmp", aWof(ir_base_srcA(in->a, aX(16))), aI(0));    // cmp の即値の形も 31 は sp
    } else {
      E2("cmp", ir_base_srcA(in->a, aX(16)), aI(0));
    }
    int k = ir_select_cset(in);
    if (k) {
      // 1・0（-1・0）を選ぶだけなら cset（csetm）。逆の並びは条件を逆に（2026-11-27）
      E2(k == 1 || k == -1 ? "cset" : "csetm", ir_dstA(in), aCond(k > 0 ? cond : cc_inverse[cond]));
      ir_finish_dstA(in);
      return;
    }
    AOp t = ir_srcA(in->b, aX(16));
    AOp f = ir_srcA(in->c, aX(17));
    E4("csel", ir_dstA(in), t, f, aCond(cond));
    ir_finish_dstA(in);
    return;
  }
  case IR_LADDR:
    ir_add_imm(ir_dstA(in), aX(29), in->imm);
    ir_finish_dstA(in);
    return;
  case IR_GADDR:
    ir_e_symaddr(ir_dstA(in), in->sym);
    ir_finish_dstA(in);
    return;
  case IR_TP:
    E2("mrs", ir_dstA(in), aText("tpidr_el0"));
    ir_finish_dstA(in);
    return;
  case IR_TLSADDR: {
    // codegen.c の gen_tls_addr と同じ計算を、スレッドポインタ（a）から
    AOp tp = ir_srcA(in->a, aX(16));
    AOp d = ir_dstA(in);
    const char *sym = get_symbol(in->sym);
    if (in->sym->is_definition) {
      E4("add", d, tp, aSym(sym, AS_TPHI), aShift("lsl", 12));
      E3("add", d, d, aSym(sym, AS_TPLO));
    } else {
      Printftn("adrp x17, :gottprel:\"%s\"", sym);
      Printftn("ldr x17, [x17, :gottprel_lo12:\"%s\"]", sym);
      E3("add", d, tp, aX(17));
    }
    ir_finish_dstA(in);
    return;
  }
  case IR_LOAD:
    if (in->acq) {
      // codegen.c の load と同じ ldar（seq_cst の読み出し）。ldar はアドレッシングモードを持たない
      AOp base = ir_base_srcA(in->a, aX(16));
      AOp d = ir_dstA(in);
      static const char *const ldar[] = {[1] = "ldarb", [2] = "ldarh", [4] = "ldar", [8] = "ldar"};
      E2(ldar[in->size], in->size == 8 ? d : aWof(d), aMem(base, 0));
      if (in->sign && in->size < 8)
        ir_emit_extA(d, d, in->size, true);
      ir_finish_dstA(in);
      return;
    }
    [[fallthrough]];
  case IR_STORE: {
    // 値（ストア）は x17、土台は x16、アドレス計算の作業用は x8
    AOp v = AO_NONE;
    if (in->op == IR_STORE)
      v = in->b->fp ? ir_fsrcA(in->b, 31) : ir_srcA(in->b, aX(17));
    AOp base = in->fp_base ? aX(29) : ir_base_srcA(in->a, aX(16));
    AOp mem;
    if (in->c)
      mem = aMemIdx(base, ir_srcA(in->c, aX(8)), in->shift);
    else if (in->fp_base)
      mem = ir_fmem(in->imm, in->size, aX(8));
    else
      mem = ir_memop(in->imm, base, in->size, aX(8));
    if (in->op == IR_STORE && in->b->fp) {
      E2("str", v, mem);
      return;
    }
    if (in->op == IR_LOAD && in->fp) {
      E2("ldr", ir_fdstA(in), mem);
      ir_finish_fdstA(in);
      return;
    }
    if (in->op == IR_STORE) {
      switch (in->size) {
      case 1: E2("strb", aWof(v), mem); break;
      case 2: E2("strh", aWof(v), mem); break;
      case 4: E2("str", aWof(v), mem); break;
      default: E2("str", v, mem); break;
      }
      return;
    }
    AOp d = ir_dstA(in);
    switch (in->size) {
    case 1: in->sign ? E2("ldrsb", d, mem) : E2("ldrb", aWof(d), mem); break;
    case 2: in->sign ? E2("ldrsh", d, mem) : E2("ldrh", aWof(d), mem); break;
    case 4: in->sign && !in->low32 ? E2("ldrsw", d, mem) : E2("ldr", aWof(d), mem); break;
    default: E2("ldr", d, mem); break;
    }
    ir_finish_dstA(in);
    return;
  }
  case IR_CALL: {
    // スタックで渡す引数（2026-11-21）: sp を下げて [sp + オフセット] に書く。作業用は x16・x17・x8 だけ
    // （値に割り当てる x9〜x15・x19〜x28 を壊さない。x8 の大きな戻り値の領域はこの後で入れる）
    if (in->stkargs) {
      ir_add_imm(aSP(), aSP(), -in->stkargs);
      ir_sp_adj = in->stkargs;     // 戻ったら 0（ir_fmem）
      for (int i = 0; i < in->nargs; i++) {
        if (in->argregs[i] >= 0)
          continue;
        int64_t off = -1 - (int64_t)in->argregs[i];
        IrInst *a = in->args[i];
        Type *aty = in->argtys ? in->argtys[i] : NULL;
        if (aty) {
          // 構造体（16 バイトまで・HFA）: アドレスから中身をそのまま写す
          AOp base = ir_base_srcA(a, aX(16));
          if (!ao_same(base, aX(16)))
            E2("mov", aX(16), base);
          for (int k = 0; k < aty->size; k += 8) {
            load_gp_exact("x8", "x16", k, MIN(8, aty->size - k));
            store_gp_exact("x8", "sp", off + k, MIN(8, aty->size - k));
          }
        } else if (a->fp) {
          E2("str", ir_fsrcA(a, 30), ir_memop(off, aSP(), a->fsz, aX(17)));
        } else if (ir_arg_const(in, i)) {
          ir_mov_imm(aX(16), a->imm);
          E2("str", aX(16), ir_memop(off, aSP(), 8, aX(17)));
        } else {
          E2("str", ir_srcA(a, aX(16)), ir_memop(off, aSP(), 8, aX(17)));
        }
      }
    }
    // 関数のポインタが x0〜x7 にあれば、引数を写す前に x17 へ
    bool fn_x17 = false;
    if (!in->sym && in->a->reg && ir_is_low_reg(in->a->reg)) {
      E2("mov", aX(17), aX(ir_phys(in->a->reg)));
      fn_x17 = true;
    }
    int pdst[16];
    IrInst *psrc[16];
    bool pimm[16];
    int np = 0;
    for (int i = 0; i < in->nargs; i++) {
      IrInst *a = in->args[i];
      Type *aty = in->argtys ? in->argtys[i] : NULL;
      if (in->argregs[i] < 0)
        continue;
      if (!aty && !a->fp) {
        // 整数の引数は、最後にまとめて並列に写す（値が x0〜x7 にもあるため）
        pdst[np] = in->argregs[i];
        psrc[np] = a;
        pimm[np++] = ir_arg_const(in, i);
        continue;
      }
      if (aty) {
        // 構造体: アドレスから読み込む（HFA は v、それ以外は x。x16 と x17 だけを使う）
        ArgInfo ai = classify(aty);
        AOp base = ir_base_srcA(a, aX(16));
        if (ai.cls == AC_HFA) {
          for (int k = 0; k < ai.nreg; k++)
            E2("ldr", aF(in->argregs[i] + k, ai.elem_sz), aMem(base, k * ai.elem_sz));
        } else {
          if (!ao_same(base, aX(16)))
            E2("mov", aX(16), base);
          for (int k = 0; k < ai.nreg; k++)
            load_gp_exact(xreg[in->argregs[i] + k], "x16", k * 8, MIN(8, aty->size - k * 8));
        }
        continue;
      }
      AOp v = ir_fsrcA(a, 30);
      E2("fmov", aF(in->argregs[i], a->fsz), v);
    }
    ir_par_move(np, pdst, psrc, pimm);
    if (in->ty && is_mem_rtn(in->ty))
      ir_add_imm(aX(8), aX(29), in->rtn_slot); // 大きな構造体の戻り値の領域
    if (in->sym) {
      ir_call_sym(in->sym);
    } else {
      AOp f = fn_x17 ? aX(17) : ir_base_srcA(in->a, aX(17));
      E1("blr", f);
    }
    if (in->stkargs) {
      ir_add_imm(aSP(), aSP(), in->stkargs);
      ir_sp_adj = 0;
    }
    if (in->ty && !is_mem_rtn(in->ty)) {
      // レジスタで返された構造体を、戻り値の領域に書く
      ArgInfo ai = classify(in->ty);
      ir_add_imm(aX(16), aX(29), in->rtn_slot);
      if (ai.cls == AC_HFA) {
        for (int k = 0; k < ai.nreg; k++)
          E2("str", aF(k, ai.elem_sz), aMem(aX(16), k * ai.elem_sz));
      } else {
        for (int k = 0; k < ai.nreg; k++)
          store_gp_exact(xreg[k], "x16", k * 8, MIN(8, in->ty->size - k * 8));
      }
      return;
    }
    if (!in->size || (!in->reg && !in->spill) || ir_fused[in->id])
      return;
    if (in->fp) {
      E2("fmov", ir_fdstA(in), aF(0, in->fsz));
      ir_finish_fdstA(in);
      return;
    }
    // AAPCS64 では 8 バイトより小さい戻り値の上位ビットは不定
    if (in->size < 8 && !(in->size == 4 && in->low32))
      ir_emit_extA(ir_dstA(in), aX(0), in->size, in->sign);
    else if (!ao_same(ir_dstA(in), aX(0)))
      E2("mov", ir_dstA(in), aX(0));
    ir_finish_dstA(in);
    return;
  }
  case IR_X1:
    if (!in->prev || in->prev->op != IR_CALL)
      internal_error();   // 呼び出しのすぐ後に置く（ir_funcall）
    if (!in->reg && !in->spill)
      return;
    E2("mov", ir_dstA(in), aX(1));
    ir_finish_dstA(in);
    return;
  case IR_ALLOCA: {
    // codegen.c の builtin_alloca と同じ（2026-11-21）。sp を下げ、ガードページを飛び越えないよう上から 4KB
    // ごとに触る。使うのは x16・x17 だけ。sp は関数の出口で x29 から戻る
    AOp n = ir_srcA(in->a, aX(16));
    int64_t c = count();
    E2("mov", aX(17), aSP());
    E3("sub", aX(16), aX(17), n);
    Printstn("and x16, x16, #-16");
    E2("mov", aSP(), aX(16));
    Printfsn(".L.alloca.%" PRIi64 ":", c);
    Printstn("sub x17, x17, #4096");
    Printstn("cmp x17, x16");
    Printftn("b.lo .L.alloca_end.%" PRIi64, c);
    Printstn("ldr xzr, [x17]");
    Printftn("b .L.alloca.%" PRIi64, c);
    Printfsn(".L.alloca_end.%" PRIi64 ":", c);
    E2("mov", ir_dstA(in), aX(16));
    ir_finish_dstA(in);
    return;
  }
  case IR_UNREACH:
    return;
  case IR_MEMZERO: {
    AOp dst = ir_base_srcA(in->a, aX(16));
    if (in->imm > 256) {
      gen_mem_zero(ao_name(dst), in->imm); // 使うのは土台と x17 だけ（大きいものは文字列の行のまま）
      return;
    }
    // codegen.c の gen_mem_zero と同じ命令を、型付きの記録で（2026-11-16）
    for (int64_t i = 0; i < in->imm;) {
      int64_t rem = in->imm - i;
      int p2 = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
      E2(st_ins(p2), aR(p2 == 8 ? 'x' : 'w', 31), aMem0(dst, i));
      i += p2;
    }
    return;
  }
  case IR_CAS: {
    // codegen.c の gen_cas と同じ。x3 = アドレス、x2 = 期待する値のアドレス、x0 = 新しい値、x1 = 期待する値、
    // x4 = 今の値、w5 = stlxr の結果（x0〜x7 は割り当てないので、作業用に使える）
    int sz = in->size;
    E2("mov", aX(3), ir_srcA(in->a, aX(16)));
    E2("mov", aX(2), ir_srcA(in->b, aX(16)));
    E2("mov", aX(0), ir_srcA(in->c, aX(16)));
    const char *nv = gp_of("x0", sz), *ex = gp_of("x1", sz), *cur = gp_of("x4", sz);
    int64_t c = count();
    const char *d = ao_name(ir_dstA(in));
    Printftn("%s %s, [x2]", ld_ins(sz, true), ex);
    Printfsn(".L.cas.%" PRIi64 ":", c);
    Printftn("%s %s, [x3]", ldaxr_ins(sz), cur);
    Printftn("cmp %s, %s", cur, ex);
    Printftn("b.ne .L.cas_fail.%" PRIi64, c);
    Printftn("%s w5, %s, [x3]", stlxr_ins(sz), nv);
    Printftn("cbnz w5, .L.cas.%" PRIi64, c);
    Printftn("mov %s, #1", d);
    Printftn("b .L.cas_end.%" PRIi64, c);
    Printfsn(".L.cas_fail.%" PRIi64 ":", c);
    Printstn("clrex");
    Printftn("%s %s, [x2]", st_ins(sz), cur);
    Printftn("mov %s, #0", d);
    Printfsn(".L.cas_end.%" PRIi64 ":", c);
    ir_finish_dstA(in);
    return;
  }
  case IR_XCHG: {
    // codegen.c の gen_exch と同じ。x3 = アドレス、x0 = 書く値、x4 = 前の値、w5 = stlxr の結果
    int sz = in->size;
    E2("mov", aX(3), ir_srcA(in->a, aX(16)));
    E2("mov", aX(0), ir_srcA(in->b, aX(16)));
    int64_t c = count();
    Printfsn(".L.xchg.%" PRIi64 ":", c);
    Printftn("%s %s, [x3]", ldaxr_ins(sz), gp_of("x4", sz));
    Printftn("%s w5, %s, [x3]", stlxr_ins(sz), gp_of("x0", sz));
    Printftn("cbnz w5, .L.xchg.%" PRIi64, c);
    if (!in->reg && !in->spill)
      return;
    if (sz == 8)
      E2("mov", ir_dstA(in), aX(4));
    else
      ir_emit_extA(ir_dstA(in), aX(4), sz, in->sign);
    ir_finish_dstA(in);
    return;
  }
  case IR_MEMCPY: {
    // gen_mem_copy は x16・x17 を使うので、逃がした値は x6・x7 に読み込む（x0〜x7 は割り当てない。
    // コピーそのものは文字列の行のまま）
    AOp dst = ir_base_srcA(in->a, aX(7));
    AOp src = ir_base_srcA(in->b, aX(6));
    if (in->imm > 256) {
      gen_mem_copy(ao_name(src), ao_name(dst), in->imm);
      return;
    }
    // codegen.c の gen_mem_copy と同じ命令を、型付きの記録で（2026-11-16）
    for (int64_t i = 0; i < in->imm;) {
      int64_t rem = in->imm - i;
      int p2 = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
      AOp r = aR(p2 == 8 ? 'x' : 'w', 16);
      E2(ld_ins(p2, true), r, aMem0(src, i));
      E2(st_ins(p2), r, aMem0(dst, i));
      i += p2;
    }
    return;
  }
  case IR_ASM: {
    Node *node = in->asm_node;
    for (int i = 0; i < in->nargs; i++) {
      IrInst *a = in->args[i];
      int r = in->argregs[i];
      if (r >= ASM_FP_BASE) {
        AOp v = ir_fsrcA(a, 30);
        E2("fmov", aF(r - ASM_FP_BASE, a->fsz), v);
      } else {
        AOp v = ir_srcA(a, aX(16));
        E2("mov", aX(r), v);
      }
    }
    if (is_plain_asm(node)) {
      Printftn("%s", node->gasm.str_tok->str);
      return;
    }
    asm_ops_cnt = 0;
    for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next)
      asm_ops[asm_ops_cnt++] = ap;
    for (AsmParam *ap = node->gasm.inputs; ap; ap = ap->next)
      asm_ops[asm_ops_cnt++] = ap;
    asm_labels_start = asm_ops_cnt;
    asm_goto_tramp = 0;
    asm_body(node, count());
    return;
  }
  case IR_ASMOUT: {
    int r = (int)in->imm;
    if (!in->reg && !in->spill)
      return;
    if (in->fp) {
      E2("fmov", ir_fdstA(in), aF(r - ASM_FP_BASE, in->fsz));
      ir_finish_fdstA(in);
    } else {
      E2("mov", ir_dstA(in), aX(r));
      ir_finish_dstA(in);
    }
    return;
  }
  case IR_PARAM_STRUCT: {
    ArgInfo ai = classify(in->ty);
    ir_add_imm(aX(16), aX(29), in->rtn_slot);
    if (ai.cls == AC_HFA) {
      for (int i = 0; i < ai.nreg; i++)
        E2("str", aF((int)in->imm + i, ai.elem_sz), aMem(aX(16), i * ai.elem_sz));
    } else {
      for (int i = 0; i < ai.nreg; i++)
        store_gp_exact(xreg[in->imm + i], "x16", i * 8, MIN(8, in->ty->size - i * 8));
    }
    return;
  }
  case IR_RET:
    if (in->nargs) {
      // 語に分けた構造体（x0・x1。値が x0〜x7 にもあるので、並列に写す）
      int pdst[8];
      bool pimm[8] = {0};
      for (int k = 0; k < in->nargs; k++)
        pdst[k] = k;
      ir_par_move(in->nargs, pdst, in->args, pimm);
    } else if (in->a && in->ty) {
      // 構造体: アドレスから x0・x1（HFA なら v0〜v3）に読み込む（x16・x17 だけを使う）
      ArgInfo ai = classify(in->ty);
      AOp base = ir_base_srcA(in->a, aX(16));
      if (!ao_same(base, aX(16)))
        E2("mov", aX(16), base);
      if (ai.cls == AC_HFA) {
        for (int k = 0; k < ai.nreg; k++)
          E2("ldr", aF(k, ai.elem_sz), aMem(aX(16), k * ai.elem_sz));
      } else {
        for (int k = 0; k < ai.nreg; k++)
          load_gp_exact(xreg[k], "x16", k * 8, MIN(8, in->ty->size - k * 8));
      }
    } else if (in->a && in->a->fp) {
      AOp v = ir_fsrcA(in->a, 30);
      E2("fmov", aF(0, in->a->fsz), v);
    } else if (in->a) {
      AOp v = ir_srcA(in->a, aX(16));
      if (!ao_same(v, aX(0)))
        E2("mov", aX(0), v);
    }
    {
      char lb[STRBUF_SZ];
      snprintf(lb, STRBUF_SZ, ".L.ir.%" PRIi64 ".rtn", ir_label_base);
      E1("b", aText(lb));
    }
    return;
  case IR_BR:
    if (bb->succ[0] != next_bb && ir_thread(bb->succ[0]) != next_bb)
      E1("b", ir_labelA(ir_thread(bb->succ[0])));
    return;
  case IR_INDBR:
    E1("br", ir_base_srcA(in->a, aX(16)));
    return;
  case IR_SWTAB: {
    // 表引きの switch（2026-11-27）: 表は行き先のラベルと表の頭の差（4 バイト）。adr・ldrsw・add・br
    char tb[STRBUF_SZ], lb[STRBUF_SZ];
    snprintf(tb, STRBUF_SZ, ".L.swt.%" PRIi64, count());
    AOp ix = ir_srcA(in->a, aX(17));
    E2("adr", aX(16), aText(tb));
    E2("ldrsw", aX(17), aMemIdx(aX(16), ix, 2));
    E3("add", aX(16), aX(16), aX(17));
    E1("br", aX(16));
    Printfsn("%s:", tb);
    for (int i = 0; i < in->nswtab; i++) {
      ir_label(lb, ir_thread(bb->succ[in->swtab[i]]));
      Printftn(".word %s - %s", lb, tb);
    }
    return;
  }
  case IR_LABELADDR:
    Printftn("adr %s, .L.jmp.%" PRIi64, ao_name(ir_dstA(in)), in->imm);
    ir_finish_dstA(in);
    return;
  case IR_CBR: {
    // 行き先が分岐だけのブロック（SSA を抜けるときに分けた辺など）なら、その先へ直接飛ぶ（次のブロック
    // でも、落ちるとそこでもう 1 回飛ぶので、先へ直接飛ぶ。ループの戻りの分岐が 1 つになる）
    IrBlock *t = ir_thread(bb->succ[0]);
    IrBlock *f = ir_thread(bb->succ[1]);
    // cc_name と同じ並び（浮動小数点の比較の mi・pl なども）
    static const char *const bcc[] = {"b.eq", "b.ne", "b.lt", "b.le", "b.gt", "b.ge", "b.lo", "b.ls",
                                      "b.hi", "b.hs", "b.mi", "b.ls", "b.gt", "b.ge", "b.pl"};
    _Static_assert(sizeof(bcc) / sizeof(*bcc) == sizeof(cc_name) / sizeof(*cc_name), "bcc");
    if (ir_fused[in->a->id] && (in->a->op == IR_CALL || in->a->op == IR_AND)) {
      // tbz・tbnz（大きな関数では、出力の前に ir_relax_tbz が届く形に直す）
      AOp r;
      int k;
      if (in->a->op == IR_CALL) {
        r = aR('w', 0);
        k = 0;
      } else {
        k = ir_tbz_bit(in->a);
        r = ir_srcA(in->a->a, aX(16));
        if (k < 32)
          r = aWof(r);
      }
      ir_ntbz++;
      if (t == next_bb) {
        E3("tbz", r, aI(k), ir_labelA(f));
      } else {
        E3("tbnz", r, aI(k), ir_labelA(t));
        if (f != next_bb)
          E1("b", ir_labelA(f));
      }
      return;
    }
    if (ir_fused[in->a->id]) {
      int c = in->a->cond;
      if (t == next_bb) {
        E1(bcc[cc_inverse[c]], ir_labelA(f));
      } else {
        E1(bcc[c], ir_labelA(t));
        if (f != next_bb)
          E1("b", ir_labelA(f));
      }
      return;
    }
    AOp a = ir_srcA(in->a, aX(16));
    if (ir_fits(in->a, 32, true) || ir_fits(in->a, 32, false))
      a = aWof(a);
    if (t == next_bb) {
      E2("cbz", a, ir_labelA(f));
    } else {
      E2("cbnz", a, ir_labelA(t));
      if (f != next_bb)
        E1("b", ir_labelA(f));
    }
    return;
  }
  }
  internal_error();
}

// x & 2^k（定数）なら k、ほかは -1
static int ir_tbz_bit(IrInst *c) {
  if (c->op != IR_AND || c->fp || !c->b || c->b->op != IR_CONST || c->a->op == IR_CONST)
    return -1;
  uint64_t m = (uint64_t)c->b->imm;
  if (!m || (m & (m - 1)))
    return -1;
  int k = 0;
  while (!(m >> k & 1))
    k++;
  return k;
}

// 比較のすぐ後の条件分岐で、その比較の値がほかで使われなければ、まとめて b.cond にする
static void ir_mark_fused(void) {
  for (int i = 0; i < ir_norder; i++) {
    IrInst *br = ir_order[i]->last;
    if (!br || br->op != IR_CBR)
      continue;
    IrInst *c = br->a;
    if ((c->op == IR_CMP || c->op == IR_FCMP) && c->bb == br->bb && c->next == br && c->uses == 1)
      ir_fused[c->id] = true;
    // 1 ビットを見る分岐（tbz・tbnz。2026-11-27）: _Bool を返す呼び出しの結果、x & 2^k
    if (c->op == IR_CALL && c->rbool && c->next == br && c->uses == 1)
      ir_fused[c->id] = true;
    if (ir_tbz_bit(c) >= 0 && c->next == br && c->uses == 1) {
      ir_fused[c->id] = true;
      c->b->uses--;     // 定数を作らない
    }
  }
  // cset にする IR_SELECT の定数は作らない
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      if (in->op == IR_SELECT && ir_select_cset(in)) {
        in->b->uses--;
        in->c->uses--;
      }
  // ccmp の前の比較（ir_ccmp_chain）は、フラグだけを作る
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      if (in->op == IR_CMP && in->c) {
        if (in->c->op != IR_CMP || in->c->next != in)
          internal_error();
        ir_fused[in->c->id] = true;
      }
  // csel（IR_SELECT）のすぐ前の比較も同じ（2026-11-26）
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next) {
      IrInst *c = in->op == IR_SELECT ? in->a : NULL;
      if (c && c->op == IR_CMP && c->next == in && c->uses == 1)
        ir_fused[c->id] = true;
    }
}

// 呼び出しになりうる命令がないか（葉の関数のフレームを省くため。ir_emit_fn）
static bool ir_is_leaf(void) {
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      switch (in->op) {
      case IR_CALL: case IR_MEMCPY: case IR_MEMZERO: case IR_ASM: case IR_ASMOUT:
      case IR_PARAM_STRUCT: case IR_ALLOCA:   // alloca は sp を動かす（戻るときに x29 から戻す）
        return false;
      case IR_FSQRT:
        if (!in->sqrt_nonneg)
          return false;  // 負の数では sqrt を呼ぶ
        break;
      default:
        break;
      }
  return !ir_rtn_ptr;
}

TLS static FILE *ir_rec_out;  // 記録の間の本当の出力先

// 記録をやめて、output_file を元に戻す（途中であきらめたとき・エラーで抜けたとき）
static void ir_rec_abort(void) {
  ir_ntbz = 0;
  if (!ir_rec_out)
    return;
  ir_rec_on = false;
  fclose(output_file);
  free(ir_side_buf);
  ir_side_buf = NULL;
  output_file = ir_rec_out;
  ir_rec_out = NULL;
}

static void ir_emit_fn(Obj *fn) {
  // 出力を記録にためる。記録にならずに output_file に書かれるもの（インラインアセンブリの本体）は、
  // 作業用の文字列に受けてから記録に移す（ir_emit_inst）
  FILE *out = output_file;
  ir_rec_out = out;
  ir_recs = ir_recs_keep;
  ir_caprecs = ir_capkeep;
  ir_nrecs = 0;
  ir_rtext = ir_rtext_end = NULL;
  ir_rpart = NULL;
  ir_rpart_len = ir_rpart_cap = 0;
  ir_side_buf = NULL;
  ir_side_len = 0;
  output_file = open_memstream(&ir_side_buf, &ir_side_len);
  ir_rec_on = true;
  ir_emit_fn2(fn);
  ir_rec_on = false;
  ir_rec_take_side();
  fclose(output_file);
  free(ir_side_buf);
  ir_side_buf = NULL;
  output_file = out;
  ir_rec_out = NULL;
  ir_relax_tbz();
  ir_rec_write(out);
}

// 退避の要るレジスタの保存（restore なら復元）。同じ種類の隣どうしは stp・ldp で 2 つずつ（オフセットが
// -512〜504 のとき。2026-11-27。前は 1 つずつ str・ldr で、入口と出口の命令の 5% あまりがこれだった）
static void ir_save_regs(const int *save, int nsave, bool restore) {
  for (int i = 0; i < nsave; i++) {
    int ofs = ir_save_ofs + 8 * i;
    char c = save[i] >= 32 ? 'd' : 'x';
    if (i + 1 < nsave && (save[i + 1] >= 32) == (save[i] >= 32) && ofs >= -512 && ofs + 8 <= 504) {
      E3(restore ? "ldp" : "stp", aR(c, save[i] % 32), aR(c, save[i + 1] % 32), aMem(aX(29), ofs));
      i++;
      continue;
    }
    E2(restore ? "ldr" : "str", aR(c, save[i] % 32), ir_memop(ofs, aX(29), 8, aX(16)));
  }
}

static void ir_emit_fn2(Obj *fn) {
  // 入口: 退避するレジスタの場所を決めてからフレームを作る
  ir_fbase_on = false;
  if (ir_has_alloca && !ir_has_asm)
    ir_reg_used[28] = true;   // フレームの土台にする（割り当てていない。ir_fmem）
  int nsave = 0;
  int save[32];
  for (int r = ir_has_asm ? 21 : 19; r <= 28; r++)
    if (ir_reg_used[r])
      save[nsave++] = r;
  for (int r = 40; r <= 47; r++)
    if (ir_reg_used[r])
      save[nsave++] = r;
  ir_frame = align_to(ir_frame, 8);
  ir_save_ofs = -(ir_frame + 8 * nsave);
  int frame = align_to(ir_frame + 8 * nsave, 16);

  // 葉の関数（呼び出しになりうる命令がなく、スタックの上の変数・逃がした値・退避するレジスタもない）は
  // フレームを作らない（GCC と同じ。2026-11-15）。x29 は呼び出し元のまま、x30 はそのまま戻り先
  bool leaf = !frame && !ir_has_asm && !ir_stk_params && ir_is_leaf();
  if (!leaf) {
    E3("stp", aX(29), aX(30), aMemPre(aSP(), -16));
    E2("mov", aX(29), aSP());
  }
  if (frame)
    ir_add_imm(aSP(), aSP(), -frame);
  char buf[STRBUF_SZ];
  if (ir_has_asm)
    E3("stp", aX(19), aX(20), aMem(aX(29), -16));
  ir_save_regs(save, nsave, false);
  // フレームの土台（ir_fmem）。x29 から届かない所（-256 より先）があるときだけ
  ir_fbase_on = false;
  ir_fbase_frame = frame;
  ir_sp_adj = 0;
  if (frame > 256 && !ir_has_asm) {
    if (!ir_has_alloca) {
      ir_fbase = aSP();
      ir_fbase_on = true;
    } else if (ir_reg_used[28]) {
      E2("mov", aX(28), aSP());
      ir_fbase = aX(28);
      ir_fbase_on = true;
    }
  }

  bool *hot = opt_sched ? ir_hot_blocks() : NULL;
  // 分岐の行き先になるブロック（IR_BR・IR_CBR の出力と同じ決め方）。ならないブロックのラベルは、
  // 並べ替えの区切りにしない（SCH_FREE_LABEL の行を前に置く）
  // どのブロックからも飛んでこず、前のブロックからも落ちてこないブロック（分岐を飛び越して先へ直接飛ぶように
  // したので、使われなくなった分けた辺など）は出さない（2026-11-16。前は届かない b が残っていた）
  bool *ref = ir_alloc(ir_nblocks);
  for (int i = 0; i < ir_norder; i++) {
    IrBlock *bb = ir_order[i], *next = ir_fallthrough(i);
    IrInst *t = bb->last;
    if (!t)
      continue;
    if (t->op == IR_BR) {
      if (bb->succ[0] != next && ir_thread(bb->succ[0]) != next)
        ref[ir_thread(bb->succ[0])->id] = true;
    } else if (t->op == IR_INDBR) {
      for (int k = 0; k < bb->nsucc; k++)
        ref[bb->succ[k]->id] = true;    // 番地で飛んでくる（間を飛ばさない）
    } else if (t->op == IR_SWTAB) {
      for (int k = 0; k < bb->nsucc; k++)
        ref[ir_thread(bb->succ[k])->id] = true;   // 表から飛んでくる
    } else if (t->op == IR_CBR) {
      IrBlock *tb = ir_thread(bb->succ[0]), *fb = ir_thread(bb->succ[1]);
      if (tb == next) {
        ref[fb->id] = true;
      } else {
        ref[tb->id] = true;
        if (fb != next)
          ref[fb->id] = true;
      }
    }
  }
  bool falls = true;   // 前に出したブロックの終わりから落ちてくる
  for (int i = 0; i < ir_norder; i++) {
    IrBlock *bb = ir_order[i];
    if (i > 0 && !falls && !ref[bb->id])
      continue;
    {
      IrInst *t = bb->last;
      IrBlock *next = ir_fallthrough(i);
      if (t && (t->op == IR_RET || t->op == IR_INDBR || t->op == IR_SWTAB))
        falls = false;
      else if (t && t->op == IR_BR)
        falls = bb->succ[0] == next || ir_thread(bb->succ[0]) == next;
      else if (t && t->op == IR_CBR)
        falls = ir_thread(bb->succ[0]) == next || ir_thread(bb->succ[1]) == next;
      else
        falls = true;
    }
    if (hot && hot[i] && !ir_sched_on) {
      ir_rec_push(NULL, 0, IRR_HOT_BEGIN);
      ir_sched_on = true;
    } else if (ir_sched_on && !(hot && hot[i])) {
      ir_rec_push(NULL, 0, IRR_HOT_END);
      ir_sched_on = false;
    }
    if (ir_sched_on && !ref[bb->id])
      Printstn(SCH_FREE_LABEL);
    {
      int n = ir_label(buf, bb);
      buf[n++] = ':';
      if (ir_rpart_len || reg_analysis) {
        buf[n++] = '\n';
        emit_out("%.*s", n, buf);   // 行の途中に続ける（ふつうは来ない）
      } else {
        ir_rec_push(buf, n, IRR_LINE);
      }
    }
    if (bb->jmp_id)
      Printfsn(".L.jmp.%" PRIi64 ":", bb->jmp_id);   // &&label の番地（goto *p の飛び先）
    IrBlock *next_bb = ir_fallthrough(i);
    for (IrInst *in = bb->first; in; in = in->next)
      ir_emit_inst(in, bb, next_bb);
  }
  if (ir_sched_on) {
    ir_rec_push(NULL, 0, IRR_HOT_END);
    ir_sched_on = false;
  }

  Printfsn(".L.ir.%" PRIi64 ".rtn:", ir_label_base);
  ir_save_regs(save, nsave, true);
  if (ir_has_asm)
    E3("ldp", aX(19), aX(20), aMem(aX(29), -16));
  if (!leaf) {
    E2("mov", aSP(), aX(29));
    E3("ldp", aX(29), aX(30), aMemPost(aSP(), 16));
  }
  E0("ret");
}

