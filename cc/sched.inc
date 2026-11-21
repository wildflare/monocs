// 命令の並べ替え（-fschedule-insns2。既定で使う。-fno-schedule-insns2 でしない。2026-11-15）
//
// 中間表現から出した関数のうち、ループの中のブロックのアセンブリ（レジスタを割り当てた後の文字列。
// ir_emit_fn が取り込む。ir_hot_blocks）を、基本ブロックの中で Cortex-A53（Zero 2 W）に合わせて並べ替える。A53 は命令を順にしか実行せず
// （1 サイクルに 2 つまで）、結果がまだできていない値を使う命令が来ると、そこで後ろも全部止まる。
// そこで、結果が遅い命令（ロード・掛け算・浮動小数点）を前へ、その結果を使う命令を後ろへ動かし、
// 間にほかの仕事を入れる。
//
// 遅延と同時に出せる組は、実機で測った値（tools/perf/a53lat.c。ARM は A53 の表を公開していない）:
// - 出す数: 1 サイクルに 2 つ。ロード・ストアは 1 つ（ldp・stp・q レジスタは 2 サイクル）、掛け算は
//   w が毎サイクル・x が 2 サイクルに 1 つ、割り算は終わるまで次を受けない。浮動小数点の足し算・
//   掛け算は 2 つ、積和は 1 つ（両方を使う）、割り算・平方根は終わるまで次を受けない。
// - 結果: ふつうの演算 1、ロード 2（浮動小数点のレジスタへは 3、ldp は 3）、掛け算 w 2・x 3、
//   umulh・smulh 5、割り算 w 11・x 19（値が大きいとき。小さいと 3）、浮動小数点 4（fmov 1）、積和 8、
//   scvtf 5、fcvtzs 2、fdiv・fsqrt d 22・s 13。
// - 受け取る側: ロード・ストアの番地、ずらす・取り出す命令（lsl・ubfx・sxtw などの即値の形）の入力、
//   add などのずらす・広げる側の入力、掛け算・割り算の入力は 1 サイクル早く要る（+1）。ストアの値・
//   浮動小数点から整数への fmov は 1 遅くてよい（-1）。積和の足す側は 4 遅くてよい。
//
// 区切り（動かさない。その前の命令はすべて前に、後の命令はすべて後に残る）: ラベル、ディレクティブ、
// 分岐・呼び出し・ret、表にない命令（dmb・ldar・mrs・ベクトルの命令など）、sp・x29・x30 を書く命令
// （割り込みが同じスタックを使うので、sp より下への読み書きを sp を動かす命令の向こうへ出さない）、
// インラインアセンブリと volatile・アトミックの読み書き（ir_emit_inst が SCH_FIXED_BEGIN・END の行で囲む）。
// メモリ: ロードどうしは入れ替える。ストアとは、同じ土台のレジスタ（間で書き換えていない）・即値の
// オフセットで範囲が重ならないとわかるときだけ入れ替える。
// 並べ替えた順が（同じ模型で数えて）元の順より速くならなければ、元の順のまま出す。2 命令までの並びは
// 読み解かない（並べ替えても速くならない）。出力は入力の行の並べ替えだけで、行の中身は変えない。
//
// 効果（実機、2026-11-15）: nbody（浮動小数点）5.66 → 4.98 秒。整数・分岐の多いもの（primes・strhash・
// primes_mc）は変わらない（ブロックが小さく、動かせる命令がほとんどない）。コンパイルの時間は 2〜3% 増える
// （ループの外にもかけると 10〜12%）。

TLS bool opt_sched = true;  // slimcc.h

// SCH_FIXED_BEGIN・SCH_FIXED_END・SCH_FREE_LABEL は ir.inc にある（ir_emit_inst・ir_emit_fn が出す）
#define SCH_MAX 64          // 1 回に並べ替える命令の数（長いブロックはここで切る）

enum {
  SCH_SP = 31,              // レジスタの番号: 0〜30 x・w、31 sp、32〜63 v（b・h・s・d・q）、64 フラグ
  SCH_FLAGS = 64,
  SCH_ZERO = -2,            // xzr・wzr
};

typedef struct {
  uint64_t w[2];
} SchSet;

static inline void sch_add(SchSet *s, int r) {
  if (r >= 0)
    s->w[r >> 6] |= 1ULL << (r & 63);
}
static inline bool sch_has(const SchSet *s, int r) {
  return (s->w[r >> 6] >> (r & 63)) & 1;
}
// いちばん下の 1 のビットの位置（m は 0 でない。slimcc には __builtin_ctzll がない）
static inline int sch_ctz(uint64_t m) {
  int n = 0;
  while (!(m & 0xff))
    m >>= 8, n += 8;
  while (!(m & 1))
    m >>= 1, n++;
  return n;
}

static inline bool sch_meets(const SchSet *a, const SchSet *b) {
  return (a->w[0] & b->w[0]) || (a->w[1] & b->w[1]);
}

enum {
  SP_ALU, SP_LS, SP_MUL, SP_DIV, SP_FP, SP_FMA, SP_FDIV,
};

typedef struct {
  const char *text;
  int len;
  const IrRec *rec;         // この行の記録（ir.inc。かけらを作るとき sch_put_rec が集める）
  SchSet defs, uses;
  SchSet early, late, acc;  // 受け取る側の読む時期（+1、-1、-4）
  int8_t lat;               // 結果ができるまで
  int8_t pipe;
  int8_t occ;               // そのユニットを使うサイクル数
  int8_t mem;               // 0、1 ロード、2 ストア
  bool fixed;               // 区切り（動かさない）
  bool known;               // 番地が「土台 + 即値」でわかる
  int8_t base;
  int16_t basever;          // 土台を最後に書いた命令（-1: この並びの前）
  int64_t off;
  int size;
} SchIns;

// 命令の種類
enum {
  SC_ALU,     // 書く先 + 読む
  SC_ALUSH,   // ずらす・取り出す（即値の形なら入力が早く要る）
  SC_MOVK,    // 書く先も読む
  SC_BFI,     // 書く先も読む・入力が早く要る
  SC_CMP,     // 書く先なし、フラグを書く
  SC_MUL,
  SC_MULH,
  SC_DIV,
  SC_LOAD,
  SC_STORE,
  SC_FP,
  SC_FMOV,
  SC_FMA,
  SC_FDIV,
  SC_SCVTF,
  SC_FCVTZ,
  SC_TERM,    // 分岐・呼び出し（区切り）
};

enum {
  SF_DEFF = 1,  // フラグを書く
  SF_USEF = 2,  // フラグを読む
  SF_PAIR = 4,  // 2 つのレジスタ（ldp・stp）
};

typedef struct {
  const char *name;
  int8_t cls;
  int8_t size;   // ロード・ストアの大きさ（0: レジスタで決まる）
  int8_t flags;
  int8_t lat;
} SchOp;

// 名前の順（strcmp）。sch_lookup が二分探索する
static const SchOp sch_ops[] = {
  {"adc", SC_ALU, 0, SF_USEF, 1},
  {"adcs", SC_ALU, 0, SF_USEF | SF_DEFF, 1},
  {"add", SC_ALU, 0, 0, 1},
  {"adds", SC_ALU, 0, SF_DEFF, 1},
  {"adr", SC_ALU, 0, 0, 1},
  {"adrp", SC_ALU, 0, 0, 1},
  {"and", SC_ALU, 0, 0, 1},
  {"ands", SC_ALU, 0, SF_DEFF, 1},
  {"asr", SC_ALUSH, 0, 0, 1},
  {"b", SC_TERM, 0, 0, 0},
  {"bfi", SC_BFI, 0, 0, 1},
  {"bfxil", SC_BFI, 0, 0, 1},
  {"bic", SC_ALU, 0, 0, 1},
  {"bics", SC_ALU, 0, SF_DEFF, 1},
  {"bl", SC_TERM, 0, 0, 0},
  {"blr", SC_TERM, 0, 0, 0},
  {"br", SC_TERM, 0, 0, 0},
  {"cbnz", SC_TERM, 0, 0, 0},
  {"cbz", SC_TERM, 0, 0, 0},
  {"ccmn", SC_CMP, 0, SF_USEF | SF_DEFF, 1},
  {"ccmp", SC_CMP, 0, SF_USEF | SF_DEFF, 1},
  {"cinc", SC_ALU, 0, SF_USEF, 1},
  {"cinv", SC_ALU, 0, SF_USEF, 1},
  {"clz", SC_ALU, 0, 0, 1},
  {"cmn", SC_CMP, 0, SF_DEFF, 1},
  {"cmp", SC_CMP, 0, SF_DEFF, 1},
  {"cneg", SC_ALU, 0, SF_USEF, 1},
  {"csel", SC_ALU, 0, SF_USEF, 1},
  {"cset", SC_ALU, 0, SF_USEF, 1},
  {"csetm", SC_ALU, 0, SF_USEF, 1},
  {"csinc", SC_ALU, 0, SF_USEF, 1},
  {"csinv", SC_ALU, 0, SF_USEF, 1},
  {"csneg", SC_ALU, 0, SF_USEF, 1},
  {"eon", SC_ALU, 0, 0, 1},
  {"eor", SC_ALU, 0, 0, 1},
  {"extr", SC_BFI, 0, 0, 1},
  {"fabs", SC_FP, 0, 0, 4},
  {"fadd", SC_FP, 0, 0, 4},
  {"fcmp", SC_CMP, 0, SF_DEFF, 1},
  {"fcmpe", SC_CMP, 0, SF_DEFF, 1},
  {"fcvt", SC_FP, 0, 0, 4},
  {"fcvtzs", SC_FCVTZ, 0, 0, 2},
  {"fcvtzu", SC_FCVTZ, 0, 0, 2},
  {"fdiv", SC_FDIV, 0, 0, 22},
  {"fmadd", SC_FMA, 0, 0, 8},
  {"fmax", SC_FP, 0, 0, 4},
  {"fmaxnm", SC_FP, 0, 0, 4},
  {"fmin", SC_FP, 0, 0, 4},
  {"fminnm", SC_FP, 0, 0, 4},
  {"fmov", SC_FMOV, 0, 0, 1},
  {"fmsub", SC_FMA, 0, 0, 8},
  {"fmul", SC_FP, 0, 0, 4},
  {"fneg", SC_FP, 0, 0, 4},
  {"fnmadd", SC_FMA, 0, 0, 8},
  {"fnmsub", SC_FMA, 0, 0, 8},
  {"fnmul", SC_FP, 0, 0, 4},
  {"fsqrt", SC_FDIV, 0, 0, 22},
  {"fsub", SC_FP, 0, 0, 4},
  {"ldp", SC_LOAD, 0, SF_PAIR, 3},
  {"ldpsw", SC_LOAD, 4, SF_PAIR, 3},
  {"ldr", SC_LOAD, 0, 0, 2},
  {"ldrb", SC_LOAD, 1, 0, 2},
  {"ldrh", SC_LOAD, 2, 0, 2},
  {"ldrsb", SC_LOAD, 1, 0, 2},
  {"ldrsh", SC_LOAD, 2, 0, 2},
  {"ldrsw", SC_LOAD, 4, 0, 2},
  {"ldur", SC_LOAD, 0, 0, 2},
  {"ldurb", SC_LOAD, 1, 0, 2},
  {"ldurh", SC_LOAD, 2, 0, 2},
  {"ldursb", SC_LOAD, 1, 0, 2},
  {"ldursh", SC_LOAD, 2, 0, 2},
  {"ldursw", SC_LOAD, 4, 0, 2},
  {"lsl", SC_ALUSH, 0, 0, 1},
  {"lsr", SC_ALUSH, 0, 0, 1},
  {"madd", SC_MUL, 0, 0, 2},
  {"mneg", SC_MUL, 0, 0, 2},
  {"mov", SC_ALU, 0, 0, 1},
  {"movk", SC_MOVK, 0, 0, 1},
  {"movn", SC_ALU, 0, 0, 1},
  {"movz", SC_ALU, 0, 0, 1},
  {"msub", SC_MUL, 0, 0, 2},
  {"mul", SC_MUL, 0, 0, 2},
  {"mvn", SC_ALU, 0, 0, 1},
  {"neg", SC_ALU, 0, 0, 1},
  {"negs", SC_ALU, 0, SF_DEFF, 1},
  {"orn", SC_ALU, 0, 0, 1},
  {"orr", SC_ALU, 0, 0, 1},
  {"rbit", SC_ALU, 0, 0, 1},
  {"ret", SC_TERM, 0, 0, 0},
  {"rev", SC_ALU, 0, 0, 1},
  {"rev16", SC_ALU, 0, 0, 1},
  {"rev32", SC_ALU, 0, 0, 1},
  {"ror", SC_ALUSH, 0, 0, 1},
  {"sbc", SC_ALU, 0, SF_USEF, 1},
  {"sbcs", SC_ALU, 0, SF_USEF | SF_DEFF, 1},
  {"sbfiz", SC_ALUSH, 0, 0, 1},
  {"sbfx", SC_ALUSH, 0, 0, 1},
  {"scvtf", SC_SCVTF, 0, 0, 5},
  {"sdiv", SC_DIV, 0, 0, 11},
  {"smaddl", SC_MUL, 0, 0, 2},
  {"smsubl", SC_MUL, 0, 0, 2},
  {"smulh", SC_MULH, 0, 0, 5},
  {"smull", SC_MUL, 0, 0, 2},
  {"stp", SC_STORE, 0, SF_PAIR, 0},
  {"str", SC_STORE, 0, 0, 0},
  {"strb", SC_STORE, 1, 0, 0},
  {"strh", SC_STORE, 2, 0, 0},
  {"stur", SC_STORE, 0, 0, 0},
  {"sturb", SC_STORE, 1, 0, 0},
  {"sturh", SC_STORE, 2, 0, 0},
  {"sub", SC_ALU, 0, 0, 1},
  {"subs", SC_ALU, 0, SF_DEFF, 1},
  {"sxtb", SC_ALUSH, 0, 0, 1},
  {"sxth", SC_ALUSH, 0, 0, 1},
  {"sxtw", SC_ALUSH, 0, 0, 1},
  {"tbnz", SC_TERM, 0, 0, 0},
  {"tbz", SC_TERM, 0, 0, 0},
  {"tst", SC_CMP, 0, SF_DEFF, 1},
  {"ubfiz", SC_ALUSH, 0, 0, 1},
  {"ubfx", SC_ALUSH, 0, 0, 1},
  {"ucvtf", SC_SCVTF, 0, 0, 5},
  {"udiv", SC_DIV, 0, 0, 11},
  {"umaddl", SC_MUL, 0, 0, 2},
  {"umsubl", SC_MUL, 0, 0, 2},
  {"umulh", SC_MULH, 0, 0, 5},
  {"umull", SC_MUL, 0, 0, 2},
  {"uxtb", SC_ALUSH, 0, 0, 1},
  {"uxth", SC_ALUSH, 0, 0, 1},
};

static const SchOp *sch_lookup(const char *s, int n) {
  int lo = 0, hi = (int)(sizeof(sch_ops) / sizeof(sch_ops[0])) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const char *m = sch_ops[mid].name;
    int c = strncmp(m, s, n);
    if (c == 0)
      c = m[n] ? 1 : 0;
    if (c == 0)
      return &sch_ops[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

// オペランド
enum { SO_REG, SO_IMM, SO_MEM, SO_EXT, SO_OTHER };

typedef struct {
  int8_t kind;
  int8_t reg;       // SO_REG: 番号（SCH_ZERO もある）。SO_MEM: 土台
  char rc;          // SO_REG: x w sp v（b h s d q）
  int8_t index;     // SO_MEM: 添字のレジスタ（-1 なし）
  bool wb;          // SO_MEM: 前置の書き戻し（!）
  bool known;       // SO_MEM: オフセットが即値
  bool shifted;     // SO_REG: 後ろにずらす・広げる指定が続く
  int64_t off;
} SchOpnd;

// レジスタの名前なら番号（xzr・wzr は SCH_ZERO）、ほかは -1
static int sch_reg(const char *s, int n, char *rc) {
  if (n < 2 || n > 3)
    return -1;
  char c = s[0];
  char d1 = s[1];
  if (d1 >= '0' && d1 <= '9') {
    int num = d1 - '0';
    if (n == 3) {
      if (s[2] < '0' || s[2] > '9')
        return -1;
      num = num * 10 + (s[2] - '0');
    }
    switch (c) {
    case 'x': case 'w':
      if (num > 30)
        return -1;
      *rc = c;
      return num;
    case 'b': case 'h': case 's': case 'd': case 'q':
      if (num > 31)
        return -1;
      *rc = c;
      return 32 + num;
    }
    return -1;
  }
  if (n == 2 && c == 's' && d1 == 'p') {
    *rc = 'x';
    return SCH_SP;
  }
  if (n == 3 && c == 'w' && d1 == 's' && s[2] == 'p') {
    *rc = 'w';
    return SCH_SP;
  }
  if (n == 3 && (c == 'x' || c == 'w') && d1 == 'z' && s[2] == 'r') {
    *rc = c;
    return SCH_ZERO;
  }
  return -1;
}

static void sch_trim(const char **s, int *n) {
  while (*n > 0 && (**s == ' ' || **s == '\t'))
    (*s)++, (*n)--;
  while (*n > 0 && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t'))
    (*n)--;
}

// 即値（#…）の値。数でなければ false
static bool sch_imm(const char *s, int n, int64_t *v) {
  if (n < 2 || s[0] != '#')
    return false;
  char buf[32];
  if (n - 1 >= (int)sizeof(buf))
    return false;
  memcpy(buf, s + 1, n - 1);
  buf[n - 1] = '\0';
  char *end;
  *v = strtoll(buf, &end, 0);
  return *end == '\0';
}

static bool sch_mem(const char *s, int n, SchOpnd *o) {
  // [base] [base, #imm] [base, xi] [base, xi, lsl #k] [base, wi, sxtw #k] [base, :lo12:sym]、後ろに ! も
  o->kind = SO_MEM;
  o->index = -1;
  o->wb = false;
  o->known = true;
  o->off = 0;
  if (n > 0 && s[n - 1] == '!') {
    o->wb = true;
    n--;
  }
  if (n < 2 || s[0] != '[' || s[n - 1] != ']')
    return false;
  s++, n -= 2;
  int k = 0;
  while (n > 0) {
    int m = 0;
    while (m < n && s[m] != ',')
      m++;
    const char *p = s;
    int pn = m;
    sch_trim(&p, &pn);
    char rc;
    if (k == 0) {
      int r = sch_reg(p, pn, &rc);
      if (r < 0 || rc == 'w')
        return false;
      o->reg = (int8_t)r;
    } else if (k == 1) {
      int64_t v;
      if (sch_imm(p, pn, &v)) {
        o->off = v;
      } else {
        int r = sch_reg(p, pn, &rc);
        o->known = false;
        if (r >= 0 && r < 32)
          o->index = (int8_t)r;
        else if (r == SCH_ZERO)
          ;
        else if (pn > 0 && p[0] == ':')
          ;  // :lo12:sym など
        else
          return false;
      }
    }
    // k >= 2 は添字のずらし方（lsl #3・sxtw など）
    k++;
    s += m;
    n -= m;
    if (n > 0)
      s++, n--;
  }
  return k >= 1;
}

static bool sch_is_ext(const char *p, int pn) {
  static const char *names[] = {"lsl", "lsr", "asr", "ror", "sxtw", "uxtw", "sxtx", "uxtx",
                                "sxtb", "uxtb", "sxth", "uxth", "msl"};
  for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
    int l = (int)strlen(names[i]);
    if (pn >= l && !strncmp(p, names[i], l) && (pn == l || p[l] == ' '))
      return true;
  }
  return false;
}

// 1 行を読む。並べ替えの模型にのらないものは fixed
static void sch_parse(SchIns *in, const char *line, int len) {
  memset(in, 0, sizeof(*in));
  in->text = line;
  in->len = len;
  in->fixed = true;
  in->base = -1;
  in->basever = -1;

  const char *s = line;
  int n = len;
  sch_trim(&s, &n);
  if (n == 0 || s[0] == '.' || s[n - 1] == ':' || (unsigned char)s[0] < ' ')
    return;
  int mn = 0;
  while (mn < n && s[mn] != ' ' && s[mn] != '\t')
    mn++;
  static const SchOp bcond = {"b.cond", SC_TERM, 0, SF_USEF, 0};
  const SchOp *op;
  if (mn >= 2 && s[0] == 'b' && s[1] == '.')
    op = &bcond;
  else
    op = sch_lookup(s, mn);
  if (!op)
    return;  // 表にない命令
  if (op->cls == SC_TERM) {
    // 区切りのまま。読むレジスタだけは調べる（直前の命令の結果を待つ分を、並べ替えで先に済ませるため）
    in->lat = 0;
    if (op->flags & SF_USEF)
      sch_add(&in->uses, SCH_FLAGS);
    if (!strncmp(s, "cb", 2) || !strncmp(s, "tb", 2)) {
      const char *p = s + mn;
      int pn = 0;
      while (p[pn] && p[pn] != ',' && p + pn < s + n)
        pn++;
      sch_trim(&p, &pn);
      char rc;
      sch_add(&in->uses, sch_reg(p, pn, &rc));
    } else if (op->flags & SF_USEF) {
      ;
    } else {
      in->uses.w[0] = ~0ULL;  // 呼び出し・ret・間接分岐: すべてを読む
      in->uses.w[1] = 1;
    }
    return;
  }

  // オペランドを分ける（[ ] の中の , では分けない）
  SchOpnd ops[8];
  int nops = 0;
  const char *p = s + mn;
  int pn = n - mn;
  while (pn > 0) {
    int depth = 0, m = 0;
    while (m < pn && (depth || p[m] != ',')) {
      if (p[m] == '[')
        depth++;
      else if (p[m] == ']')
        depth--;
      m++;
    }
    const char *q = p;
    int qn = m;
    sch_trim(&q, &qn);
    if (nops == 8 || qn == 0)
      return;
    SchOpnd *o = &ops[nops++];
    memset(o, 0, sizeof(*o));
    char rc = 0;
    int r;
    if (q[0] == '[') {
      if (!sch_mem(q, qn, o))
        return;
    } else if (q[0] == '#') {
      o->kind = SO_IMM;
      if (!sch_imm(q, qn, &o->off))
        o->kind = SO_OTHER;
    } else if ((r = sch_reg(q, qn, &rc)) != -1) {
      o->kind = SO_REG;
      o->reg = (int8_t)r;
      o->rc = rc;
    } else if (sch_is_ext(q, qn)) {
      o->kind = SO_EXT;
      if (nops >= 2 && ops[nops - 2].kind == SO_REG)
        ops[nops - 2].shifted = true;
    } else if (memchr(q, '.', qn) && (q[0] == 'v')) {
      return;  // ベクトルの命令
    } else {
      o->kind = SO_OTHER;  // ラベル・条件・:lo12:sym など
    }
    p += m;
    pn -= m;
    if (pn > 0)
      p++, pn--;
  }

  in->lat = op->lat;
  in->pipe = SP_ALU;
  in->occ = 1;
  if (op->flags & SF_DEFF)
    sch_add(&in->defs, SCH_FLAGS);
  if (op->flags & SF_USEF)
    sch_add(&in->uses, SCH_FLAGS);

  int first_src = 1;
  switch (op->cls) {
  case SC_LOAD:
  case SC_STORE: {
    int nreg = (op->flags & SF_PAIR) ? 2 : 1;
    if (nops < nreg + 1)
      return;
    for (int i = 0; i < nreg; i++)
      if (ops[i].kind != SO_REG)
        return;
    SchOpnd *m = &ops[nreg];
    if (m->kind != SO_MEM)
      return;
    bool post = false;
    if (nops == nreg + 2) {
      if (ops[nreg + 1].kind != SO_IMM)
        return;
      post = true;
    } else if (nops != nreg + 1) {
      return;
    }
    char rc = ops[0].rc;
    int rsize = rc == 'x' ? 8 : rc == 'w' ? 4 : rc == 'b' ? 1 : rc == 'h' ? 2 : rc == 's' ? 4 :
                rc == 'd' ? 8 : 16;
    in->size = (op->size ? op->size : rsize) * nreg;
    bool fpreg = ops[0].reg >= 32;
    for (int i = 0; i < nreg; i++) {
      if (op->cls == SC_LOAD)
        sch_add(&in->defs, ops[i].reg);
      else {
        sch_add(&in->uses, ops[i].reg);
        sch_add(&in->late, ops[i].reg);
      }
    }
    sch_add(&in->uses, m->reg);
    sch_add(&in->early, m->reg);
    if (m->index >= 0) {
      sch_add(&in->uses, m->index);
      sch_add(&in->early, m->index);
    }
    if (m->wb || post)
      sch_add(&in->defs, m->reg);
    in->mem = op->cls == SC_LOAD ? 1 : 2;
    in->known = m->known && !m->wb && !post;
    in->base = m->reg;
    in->off = m->off;
    in->pipe = SP_LS;
    in->occ = (nreg == 2 || rc == 'q') ? 2 : 1;
    if (op->cls == SC_LOAD && fpreg && nreg == 1)
      in->lat = 3;
    break;
  }
  case SC_CMP:
    first_src = 0;
    goto alu;
  case SC_MOVK:
  case SC_BFI:
    if (nops < 1 || ops[0].kind != SO_REG)
      return;
    sch_add(&in->uses, ops[0].reg);
    goto alu;
  case SC_MUL:
  case SC_MULH:
  case SC_DIV: {
    if (nops < 1 || ops[0].kind != SO_REG)
      return;
    bool x = ops[0].rc == 'x';
    if (op->cls == SC_MUL) {
      in->pipe = SP_MUL;
      in->lat = x && strncmp(s, "smull", 5) && strncmp(s, "umull", 5) ? 3 : 2;
      in->occ = x ? 2 : 1;
    } else if (op->cls == SC_MULH) {
      in->pipe = SP_MUL;
      in->occ = 2;
    } else {
      in->pipe = SP_DIV;
      in->lat = x ? 19 : 11;
      in->occ = in->lat;
    }
    goto alu;
  }
  case SC_FP:
  case SC_FMOV:
  case SC_FMA:
  case SC_FDIV:
  case SC_SCVTF:
  case SC_FCVTZ:
    if (nops < 1 || ops[0].kind != SO_REG)
      return;
    in->pipe = op->cls == SC_FMA ? SP_FMA : op->cls == SC_FDIV ? SP_FDIV : SP_FP;
    if (op->cls == SC_FDIV) {
      in->lat = ops[0].rc == 'd' ? 22 : 13;
      in->occ = in->lat;
    }
    if (op->cls == SC_FMOV && ops[0].reg < 32)
      ;  // 浮動小数点から整数へ（読む側の -1 は下で）
    goto alu;
  default:
  alu:
    for (int i = 0; i < nops; i++) {
      SchOpnd *o = &ops[i];
      if (o->kind == SO_MEM)
        return;  // ロード・ストアでない命令に番地はない
      if (o->kind != SO_REG || o->reg == SCH_ZERO)
        continue;
      if (o->reg >= 32 && o->rc != 'b' && o->rc != 'h' && o->rc != 's' && o->rc != 'd' && o->rc != 'q')
        return;
      if (i < first_src) {
        sch_add(&in->defs, o->reg);
        continue;
      }
      sch_add(&in->uses, o->reg);
      bool early = false;
      if (op->cls == SC_MUL || op->cls == SC_MULH || op->cls == SC_DIV)
        early = !(op->cls == SC_MUL && i == 3);  // madd・msub の足す側はふつう
      else if (op->cls == SC_BFI)
        early = true;
      else if (op->cls == SC_ALUSH)
        early = nops < 3 || ops[nops - 1].kind != SO_REG;  // lsl x, y, z（レジスタの量）はふつう
      else if (o->shifted)
        early = true;
      if (early)
        sch_add(&in->early, o->reg);
      if (op->cls == SC_FMA && i == 3)
        sch_add(&in->acc, o->reg);
      if (op->cls == SC_FMOV && ops[0].reg < 32)
        sch_add(&in->late, o->reg);
    }
    break;
  }

  // sp・x29・x30 を書く命令は区切り
  if (sch_has(&in->defs, SCH_SP) || sch_has(&in->defs, 29) || sch_has(&in->defs, 30))
    return;
  in->fixed = false;
}

// i の結果を j が待つサイクル数（依存がなければ -1）
static int sch_dep(const SchIns *a, const SchIns *b) {
  int l = -1;
  if (sch_meets(&a->defs, &b->uses)) {
    for (int k = 0; k < 2; k++) {
      uint64_t m = a->defs.w[k] & b->uses.w[k];
      while (m) {
        int bit = sch_ctz(m);
        m &= m - 1;
        int r = k * 64 + bit;
        int lr = a->lat;
        if (sch_has(&b->early, r))
          lr++;
        if (sch_has(&b->late, r))
          lr--;
        if (sch_has(&b->acc, r))
          lr -= 4;
        if (lr < 1)
          lr = 1;
        if (lr > l)
          l = lr;
      }
    }
  }
  if (l < 1 && sch_meets(&a->defs, &b->defs))
    l = 1;
  if (l < 0 && sch_meets(&a->uses, &b->defs))
    l = 0;
  if (a->mem && b->mem && (a->mem == 2 || b->mem == 2)) {
    bool alias = true;
    if (a->known && b->known && a->base == b->base && a->basever == b->basever &&
        (a->off + a->size <= b->off || b->off + b->size <= a->off))
      alias = false;
    if (alias) {
      int lm = a->mem == 2 && b->mem == 1 ? 1 : 0;
      if (lm > l)
        l = lm;
    }
  }
  return l;
}

typedef struct {
  int cyc;
  int slots;
  int fp;
  int ls_free, mul_free, div_free, fdiv_free;
} SchState;

static bool sch_fits(SchState *st, const SchIns *in) {
  if (st->slots >= 2)
    return false;
  switch (in->pipe) {
  case SP_LS:
    return st->ls_free <= st->cyc;
  case SP_MUL:
    return st->mul_free <= st->cyc;
  case SP_DIV:
    return st->div_free <= st->cyc;
  case SP_FP:
    return st->fp < 2;
  case SP_FMA:
    return st->fp == 0;
  case SP_FDIV:
    return st->fp < 2 && st->fdiv_free <= st->cyc;
  }
  return true;
}

static void sch_take(SchState *st, const SchIns *in) {
  st->slots++;
  switch (in->pipe) {
  case SP_LS: st->ls_free = st->cyc + in->occ; break;
  case SP_MUL: st->mul_free = st->cyc + in->occ; break;
  case SP_DIV: st->div_free = st->cyc + in->occ; break;
  case SP_FP: st->fp++; break;
  case SP_FMA: st->fp = 2; break;
  case SP_FDIV: st->fp++; st->fdiv_free = st->cyc + in->occ; break;
  }
}

static void sch_next_cycle(SchState *st) {
  st->cyc++;
  st->slots = 0;
  st->fp = 0;
}

TLS static int8_t sch_lat[SCH_MAX + 1][SCH_MAX + 1];

// 命令 i を cyc に出したとき、終わり（区切りの命令が出せる・結果ができる）
static int sch_done(const SchIns *ins, int i, int cyc, const SchIns *term) {
  if (term) {
    int l = sch_lat[i][SCH_MAX];
    return l >= 0 ? cyc + l : cyc + 1;
  }
  return cyc + (ins[i].lat > 0 ? ins[i].lat : 1);
}

// 元の順に出したときの、終わりまでのサイクル数
static int sch_simulate(SchIns *ins, int n, SchIns *term) {
  SchState st = {0};
  int ready[SCH_MAX];
  for (int i = 0; i < n; i++)
    ready[i] = 0;
  int end = 0;
  for (int i = 0; i < n; i++) {
    while (st.cyc < ready[i])
      sch_next_cycle(&st);
    while (!sch_fits(&st, &ins[i]))
      sch_next_cycle(&st);
    sch_take(&st, &ins[i]);
    for (int j = i + 1; j < n; j++)
      if (sch_lat[i][j] >= 0 && st.cyc + sch_lat[i][j] > ready[j])
        ready[j] = st.cyc + sch_lat[i][j];
    int d = sch_done(ins, i, st.cyc, term);
    if (d > end)
      end = d;
  }
  return end;
}

// ins[0..n-1]（区切りなし）を並べ替えて out に書く。term はその後の区切りの命令（なければ NULL）
// 並べ替えた行を書く。sch_rec が非 NULL なら（かけらを作るとき）記録を集める
TLS static void (*sch_rec)(const IrRec *r);


static void sch_put_rec(const IrRec *r, const char *text, int len, FILE *out) {
  if (sch_rec) {
    sch_rec(r);
    return;
  }
  fwrite(text, 1, len, out);
  fputc('\n', out);
}

static void sch_region(SchIns *ins, int n, SchIns *term, FILE *out) {
  if (n <= 1) {
    for (int i = 0; i < n; i++)
      sch_put_rec(ins[i].rec, ins[i].text, ins[i].len, out);
    return;
  }
  // 土台のレジスタを最後に書いた命令（オフセットで比べてよいか）
  int16_t lastdef[65];
  for (int r = 0; r < 65; r++)
    lastdef[r] = -1;
  for (int i = 0; i < n; i++) {
    if (ins[i].base >= 0)
      ins[i].basever = lastdef[(int)ins[i].base];
    for (int k = 0; k < 2; k++)
      for (uint64_t m = ins[i].defs.w[k]; m; m &= m - 1)
        lastdef[k * 64 + sch_ctz(m)] = (int16_t)i;
  }
  uint64_t preds[SCH_MAX] = {0};
  for (int j = 0; j < n; j++) {
    for (int i = 0; i < j; i++) {
      int l = sch_dep(&ins[i], &ins[j]);
      sch_lat[i][j] = (int8_t)l;
      if (l >= 0)
        preds[j] |= 1ULL << i;
    }
    for (int i = j; i < n; i++)
      sch_lat[i][j] = -1;
    sch_lat[j][SCH_MAX] = term ? (int8_t)sch_dep(&ins[j], term) : -1;
  }

  // 終わりまでの長さ（遅延の和の最大）
  int height[SCH_MAX];
  for (int i = n - 1; i >= 0; i--) {
    int h = ins[i].lat;
    if (term && sch_lat[i][SCH_MAX] > h)
      h = sch_lat[i][SCH_MAX];
    for (int j = i + 1; j < n; j++)
      if (sch_lat[i][j] >= 0 && sch_lat[i][j] + height[j] > h)
        h = sch_lat[i][j] + height[j];
    height[i] = h;
  }

  // 元の順がもう、いちばん長い道（と、出す数の下限）どおりなら、そのまま
  int t_orig = sch_simulate(ins, n, term);
  int bound = (n + 1) / 2;
  for (int i = 0; i < n; i++)
    if (height[i] > bound)
      bound = height[i];
  if (t_orig <= bound) {
    for (int i = 0; i < n; i++)
      sch_put_rec(ins[i].rec, ins[i].text, ins[i].len, out);
    return;
  }

  // 一番長い道の上にあるものから、出せるときに出す
  int order[SCH_MAX], ready[SCH_MAX];
  for (int i = 0; i < n; i++)
    ready[i] = 0;
  uint64_t done = 0;
  SchState st = {0};
  int t_new = 0;
  for (int k = 0; k < n; k++) {
    for (;;) {
      int best = -1;
      for (int i = 0; i < n; i++) {
        if ((done >> i) & 1 || (preds[i] & ~done) || ready[i] > st.cyc || !sch_fits(&st, &ins[i]))
          continue;
        if (best < 0 || height[i] > height[best])
          best = i;
      }
      if (best >= 0) {
        order[k] = best;
        done |= 1ULL << best;
        sch_take(&st, &ins[best]);
        for (int j = best + 1; j < n; j++)
          if (sch_lat[best][j] >= 0 && st.cyc + sch_lat[best][j] > ready[j])
            ready[j] = st.cyc + sch_lat[best][j];
        int d = sch_done(ins, best, st.cyc, term);
        if (d > t_new)
          t_new = d;
        break;
      }
      sch_next_cycle(&st);
    }
  }

  // 並べ替えて速くならなければ、元の順のまま
  for (int k = 0; k < n; k++) {
    int i = t_new < t_orig ? order[k] : k;
    sch_put_rec(ins[i].rec, ins[i].text, ins[i].len, out);
  }
}

// 軽く見て、区切り（空・ラベル・ディレクティブ・印・分岐・呼び出し・ret）か。q は前後の空白を除いた行
static bool sch_cheap_boundary(const char *q, int ql) {
  if (ql == 0)
    return true;
  char c = q[0];
  if (c == '.' || (unsigned char)c < ' ' || q[ql - 1] == ':')
    return true;
  int mn = 0;
  while (mn < ql && q[mn] != ' ' && q[mn] != '\t')
    mn++;
  if (c == 'b')   // b、b.cond、bl、blr、br
    return mn == 1 || q[1] == '.' || (mn == 2 && (q[1] == 'l' || q[1] == 'r')) ||
           (mn == 3 && q[1] == 'l' && q[2] == 'r');
  if (c == 'r')
    return mn == 3 && q[1] == 'e' && q[2] == 't';
  if (c == 'c' || c == 't')   // cbz、cbnz、tbz、tbnz
    return mn >= 3 && q[1] == 'b' && (q[2] == 'z' || q[2] == 'n');
  return false;
}

// 区切りの間の n 行を並べ替えて書く。term は後ろの区切りの行（命令でなければ NULL）。
// 2 行までは並べ替えても速くならないので、読み解かずにそのまま書く
static void sch_block(const char **lines, const int *lens, const IrRec **rps, int n, const char *term, int tlen,
                      FILE *out) {
  if (n <= 2) {
    for (int i = 0; i < n; i++)
      sch_put_rec(rps[i], lines[i], lens[i], out);
    return;
  }
  SchIns ins[SCH_MAX + 1];
  for (int i = 0; i < n; i++) {
    sch_parse(&ins[i], lines[i], lens[i]);
    ins[i].rec = rps[i];
  }
  SchIns tin, *tp = NULL;
  if (term) {
    sch_parse(&tin, term, tlen);
    tp = &tin;
  }
  // 読み解いて区切りとわかった命令（表にない・sp を書くなど）でも分ける
  int start = 0;
  for (int i = 0; i < n; i++) {
    if (!ins[i].fixed)
      continue;
    sch_region(ins + start, i - start, &ins[i], out);
    sch_put_rec(rps[i], lines[i], lens[i], out);
    start = i + 1;
  }
  sch_region(ins + start, n - start, tp, out);
}

// 行の並び（ir.inc の記録 IrRec。s と len）を並べ替えて out に書く
static void sch_lines(const IrRec *recs, int nrec, FILE *out) {
  const char *lines[SCH_MAX];
  int lens[SCH_MAX];
  const IrRec *rps[SCH_MAX];
  int n = 0;
  bool fixed = false, free_label = false;
  for (int ri = 0; ri < nrec; ri++) {
    int l = recs[ri].len;
    const char *line = recs[ri].s;
    const char *q = line;
    int ql = l;
    sch_trim(&q, &ql);
    if (ql == 1 && (q[0] == SCH_FIXED_BEGIN[0] || q[0] == SCH_FIXED_END[0])) {
      sch_block(lines, lens, rps, n, NULL, 0, out);
      n = 0;
      fixed = q[0] == SCH_FIXED_BEGIN[0];
      continue;
    }
    if (ql == 1 && q[0] == SCH_FREE_LABEL[0]) {
      free_label = true;
      continue;
    }
    if (fixed) {
      sch_put_rec(&recs[ri], line, l, out);
      continue;
    }
    if (free_label && ql > 0 && q[ql - 1] == ':') {
      // どこからも飛んでこないラベル: 区切りにせず、すぐ書く（置き場所は意味を持たない）
      free_label = false;
      sch_put_rec(&recs[ri], line, l, out);
      continue;
    }
    free_label = false;
    if (sch_cheap_boundary(q, ql)) {
      bool insn = ql > 0 && q[0] != '.' && q[ql - 1] != ':';
      sch_block(lines, lens, rps, n, insn ? line : NULL, l, out);
      sch_put_rec(&recs[ri], line, l, out);
      n = 0;
      continue;
    }
    lines[n] = line;
    lens[n] = l;
    rps[n] = &recs[ri];
    if (++n == SCH_MAX) {
      sch_block(lines, lens, rps, n, NULL, 0, out);
      n = 0;
    }
  }
  sch_block(lines, lens, rps, n, NULL, 0, out);
}
