// slimcc-a64: 中間表現（SSA）を使うコード生成（codegen.c の末尾で #include する）
//
// 関数ごとに AST を基本ブロックの並び（制御フローグラフ）と SSA 形式の命令にし、
// 線形走査でレジスタを割り当てて AArch64 の命令を出す（ir_a64_emit.inc）。
// 扱えない機能（long double、可変長引数の関数、スタックで渡す引数、計算型の goto、asm goto など）を
// 含む関数は、ir_gen_fn が false を返し、今までのコード生成（gen_fn_code）が出力する。
// 既定で使う（-fno-slimcc-ir で使わない）。扱えない理由は、環境変数 SLIMCC_IR_WHY を設定すると表示する。
//
// 出典: Braun et al., "Simple and Efficient Construction of Static Single Assignment Form"（CC 2013）、
//       Poletto and Sarkar, "Linear Scan Register Allocation"（TOPLAS 1999）、AAPCS64
//
// 値の表し方: 整数とポインタの値は、すべて 64 ビットのレジスタに、C の型に合わせて符号拡張・
// ゼロ拡張した形で持つ（_Bool は 0 か 1）。演算の結果は、型の幅に合わせて拡張し直す。
// こうすると、割り算・シフト・比較を 64 ビットの命令で正しく行える。
//
// ターゲット: AArch64 のことは ir_a64.inc（レジスタの組・呼び出し規約・即値）と ir_a64_emit.inc（命令を出す）、
// 命令の並べ替えは sched.inc（Cortex-A53）。このファイルに残っている AArch64 の前提（w レジスタの印 low32・番地の形・
// csel・ccmp・tbz を前提にした変形など）は docs/slimcc-codegen.md の「ターゲットに依存するところ」。
//
// フレーム（x29 から下）: [x29 - 8k] 退避したレジスタ、その下にメモリに置く局所変数、逃がした値

#include <setjmp.h>

TLS bool opt_strict_aliasing = true; // 型による別名の判定（-fstrict-aliasing・-fno-strict-aliasing。GCC の -O2 と同じ）
TLS bool opt_unroll = true; // ループの展開（-funroll-loops・-fno-unroll-loops）
TLS bool opt_ir = true; // 既定で使う。-fno-slimcc-ir で今までのコード生成だけにする

// 命令の並べ替え（sched.inc。codegen.c の終わりで取り込む）
#define SCH_FIXED_BEGIN "\x01"
#define SCH_FIXED_END "\x02"
#define SCH_FREE_LABEL "\x03"   // 次の行のラベルはどこからも飛んでこない（区切りにしない）
// 中間表現の関数の出力の記録（2026-11-16。段階 1a: 書式済みの 1 行）。ir_emit_fn は記録をためて、関数の
// 終わりにループの中の範囲を並べ替えながら書く
// AOp・IrRec の型は slimcc.h（内蔵アセンブラも使う）
static void sch_lines(const IrRec *recs, int nrec, FILE *out);
TLS static void (*sch_rec)(const IrRec *r);   // sched.inc
static int ao_print(char *b, const AOp *o);
static int ao_putd(char *b, int64_t v);

typedef struct IrInst IrInst;
typedef struct IrBlock IrBlock;

typedef enum {
  IR_CONST,   // imm
  IR_UNDEF,   // 未定義（初期化していない変数）。0 として扱う
  IR_PARAM,   // 引数（imm 番目のレジスタ）
  IR_PHI,     // args[i] は preds[i] から来たときの値
  IR_COPY,    // a（SSA を抜けるときのコピー）
  IR_ADD, IR_SUB, IR_MUL, IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
  IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR, IR_SAR,
  IR_NEG, IR_NOT,       // -a、~a
  IR_EXT,               // a を size バイトに切り詰め、sign なら符号拡張（size 8 は何もしない）
  IR_BOOL,              // a != 0
  IR_CMP,               // cond（CC_*）で a と b を比べた 0/1
  IR_LADDR,             // メモリに置いた局所変数のアドレス（slot）
  IR_GADDR,             // 大域変数・関数のアドレス（sym）
  IR_TP,                // スレッドポインタ（mrs tpidr_el0。関数の中では変わらない）
  IR_TLSADDR,           // TLS の変数のアドレス（sym）: a（IR_TP）+ 変数のオフセット（local-exec・initial-exec）
  IR_LOAD,              // [a] から size バイト（sign なら符号拡張）
  IR_STORE,             // [a] に b を size バイト
  IR_CALL,              // sym（直接）または a（関数ポインタ）を args で呼ぶ。値は戻り値（size・sign で拡張）
  IR_RET,               // a を戻り値にして返る（a がなければ値なし）
  IR_BR,                // succ[0] へ
  IR_CBR,               // a が 0 でなければ succ[0]、0 なら succ[1]
  IR_UNREACH,           // ここには来ない（noreturn の関数の呼び出しの後、unreachable()）
  // 浮動小数点（fp が立った値。fsz は 4（float）か 8（double））
  IR_FCONST,            // imm（ビット列）
  IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,
  IR_FMADD, IR_FMSUB, IR_FNMSUB, // 積和演算: c + a*b、c - a*b、a*b - c（丸めは 1 回。-ffp-contract）
  IR_FNEG, IR_FABS,
  IR_FSQRT,             // sym の sqrt・sqrtf の代わり。結果が NaN なら sym を呼ぶ（errno のため）ので、呼び出しと同じに扱う
  IR_FCMP,              // cond（CC_EQ、CC_NE、CC_FLT〜CC_FGE）で比べた 0/1（整数）
  IR_I2F,               // 整数 a を浮動小数点に（sign なら符号つき）
  IR_F2I,               // 浮動小数点 a を 64 ビットの整数に（sign なら符号つき。0 の方へ切り捨て）
  IR_F2F,               // 浮動小数点の幅を変える
  IR_MEMZERO,           // [a] から imm バイトを 0 にする（初期化子）
  IR_MEMCPY,            // [a] に [b] から imm バイトをコピーする（構造体の代入など）
  IR_PARAM_STRUCT,      // レジスタで渡された構造体の引数（ty。imm 番目から）を [x29 + rtn_slot] に書く
  IR_ASM,               // インラインアセンブリ（asm_node）。args を argregs（x0〜x7、32 + v0〜v7）に置いてから本文
  IR_ASMOUT,            // 直前の IR_ASM の出力（imm 番のレジスタ）を受け取る
  // アトミック操作（__atomic_compare_exchange・__atomic_exchange など。ldaxr・stlxr のループ。x0〜x5 を使う）
  IR_CAS,               // [a] が [b] と等しければ c を書いて 1、違えば [a] を [b] に書いて 0（size バイト）
  IR_XCHG,              // [a] に b を書き、前の値を返す（size・sign で拡張）
  IR_ALLOCA,            // __builtin_alloca(a): sp を a バイト（16 にそろえる）下げ、その番地。関数から戻るまで残る
  IR_X1,                // 直前の IR_CALL の後の x1（16 バイトの構造体の戻り値の 2 語目。2026-11-21）
  // 計算型の goto（GNU の &&label・goto *p。2026-11-21）。goto *p はすべて 1 つの振り分けのブロック（ir_dispatch）に
  // 集め、そこで IR_INDBR する。後続は、番地を取れるラベルごとの小さなブロック（jmp_id の .L.jmp.N を出し、
  // ラベルのブロックへ飛ぶだけ）。後続の多いブロックから合流のあるブロックへの辺ができないので、SSA を抜けるとき
  // の辺の分け方などは今までのまま（GCC も goto *p を 1 か所に集める）
  IR_INDBR,             // a の番地へ飛ぶ（br。後続は bb->succ のすべて）
  // 表引きの switch（2026-11-27）: a（0〜nswtab - 1。範囲は前の比べで確かめてある）番目の行き先
  // bb->succ[swtab[a]] へ飛ぶ。後続は行き先ごとの入口のブロック（前任が 1 つ。IR_INDBR と同じく、SSA を抜けるとき
  // に辺を分けなくてよい）
  IR_SWTAB,
  IR_LABELADDR,         // ラベルの番地（adr。imm は .L.jmp の番号）
  IR_SELECT,            // a が 0 でなければ b、0 なら c（整数。csel。分岐をなくす if 変換 ir_if_convert。2026-11-26）
} IrOp;

// 浮動小数点の比較（fcmp）: NaN（順序なし）では <・<=・>・>=・== が偽、!= が真。
// < は mi（否定は pl）、<= は ls（否定は hi）、> は gt（否定は le）、>= は ge（否定は lt）で、否定は NaN で真になる
enum { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE, CC_LO, CC_LS, CC_HI, CC_HS,
       CC_FLT, CC_FLE, CC_FGT, CC_FGE, CC_PL };
static const char *const cc_name[] = {"eq", "ne", "lt", "le", "gt", "ge", "lo", "ls", "hi", "hs",
                                      "mi", "ls", "gt", "ge", "pl"};
// 条件の否定（分岐を逆にするとき）
static const int cc_inverse[] = {CC_NE, CC_EQ, CC_GE, CC_GT, CC_LE, CC_LT, CC_HS, CC_HI, CC_LS, CC_LO,
                                 CC_PL, CC_HI, CC_LE, CC_LT, CC_FLT};

struct IrInst {
  IrOp op;
  int id;           // 値の番号（仮想レジスタ）
  IrInst *a, *b;
  IrInst **args;
  int nargs, capargs;
  int64_t imm;
  int size;         // EXT・LOAD・STORE・CALL の幅（バイト）
  bool sign;
  bool vol;         // volatile なメモリの読み書き
  bool acq;         // _Atomic の変数の読み出し（ldar。vol も立てる。アドレッシングモードに畳み込まない）
  int cond;
  Obj *sym;
  IrBlock *bb;
  IrInst *next, *prev;
  IrInst *repl;     // 置き換えた先（自明な phi、最適化で同じ値になったもの）
  int uses;
  int visit;        // 走査の印
  int fits_key;     // ir_fits で調べている途中の幅と符号
  // LOAD・STORE のアドレス: [a + c << shift + imm]（c がなければ [a + imm]）。fp_base なら a の代わりに x29
  IrInst *c;
  int shift;
  bool fp_base;
  // LOAD・STORE が構造体のメンバーの読み書きなら、その構造体の型とメンバーの位置（ir_may_alias で、
  // 同じ構造体の重ならない別のメンバーは重ならないとみなす。C の型の規則による。共用体には付けない）
  Type *mem_struct;
  int mem_off;
  // FSQRT の引数が負にならない（二乗の和など。ir_mark_sqrt）: errno のために sqrt を呼ぶことがないので、
  // 確かめずに fsqrt だけにし、呼び出しとしても扱わない
  bool sqrt_nonneg;
  // CALL の戻り値が _Bool（0 か 1。AAPCS64 で下位 8 ビットに入る）。すぐ後の分岐だけに使うなら tbz w0, #0（2026-11-27）
  bool rbool;
  // CMP の c があれば ccmp（ir_ccmp_chain）: c（すぐ前の比較）の後、条件 ccond が成り立つときだけ a と b を比べ、
  // 成り立たなければ自分の条件 cond が偽になるフラグにする
  int ccond;
  // LOAD・STORE の型の組（ir_tbaa。0 は何とでも重なりうる）。組の違う読み書きは重ならない（C の型の規則。
  // -fno-strict-aliasing ではいつも 0）
  uint8_t tbaa;
  // 浮動小数点の値（fsz は 4 か 8）。LOAD・STORE・CALL・PARAM・PHI・COPY・UNDEF にも付く
  bool fp;
  int fsz;
  // 32 ビットの整数演算: wform なら w レジスタの命令で計算する（結果はゼロ拡張）。
  // nsw なら符号つきのあふれがない（C では未定義動作）ので、結果は符号拡張した 32 ビットの値のまま
  bool wform;
  bool nsw;
  // 下位 32 ビットだけが使われる（ir_mark_low32）。low32 の nsw の演算は w レジスタの命令で計算し、
  // 符号拡張しない（上位 32 ビットは 0 になりうる。使う側は下位 32 ビットだけを読む）
  bool low32;
  int *swtab;       // IR_SWTAB: 値 → 後続の番号
  int nswtab;
  int *argregs;     // CALL の引数を置くレジスタの番号（x か v）。負ならスタック: -1 - そのオフセット（sp から）
  int stkargs;      // CALL: スタックで渡す引数の量（16 の倍数。呼ぶ直前に sp を下げ、戻ったら上げる）
  Type **argtys;    // CALL の引数の型（構造体はアドレスを値にして、レジスタに読み込んで渡す）
  Type *ty;         // 構造体の戻り値（CALL・RET）、構造体の引数（PARAM_STRUCT）
  int rtn_slot;     // CALL: 構造体の戻り値の一時領域（x29 からのオフセット。0 はなし）
  Node *asm_node;   // IR_ASM の ND_ASM
  // レジスタ割り当て
  int pos;          // 線形化した命令の位置
  int start, end;   // 生存区間
  int reg;          // 割り当てたレジスタ（0 はなし）
  int spill;        // 逃がした先（x29 からの負のオフセット。0 はなし）
  bool cross_call;  // 生存区間が関数呼び出しをまたぐ
  int *rng;         // 生存区間の並び（半開区間 [rng[2i], rng[2i+1])、昇順）
  int nrng, caprng;
  int rcur;         // ir_linear_scan: 終わった区間の数（調べる位置は進む一方なので、前の区間は飛ばす）
  IrInst *hint;     // 同じレジスタにしたい値（コピーの相手）
};

typedef struct {
  Obj *var;
  IrInst *val;
} IrDef;

struct IrBlock {
  int id;
  IrInst *first, *last;
  IrBlock **preds;
  int npreds, cappreds;
  IrBlock **succ;   // ふつうは succ_buf（IR_INDBR だけ多い）
  IrBlock *succ_buf[2];
  int nsucc, capsucc;
  int64_t jmp_id;   // .L.jmp.N（番地を取れるラベルの入口のブロック。0 はなし）
  bool sealed;
  bool reachable;
  IrDef *defs;      // このブロックでの変数の最新の値
  int ndefs, capdefs;
  IrDef *incomplete; // 封じる前に作った phi（var の値を待っている）
  int nincomplete, capincomplete;
  int order;        // 線形化の順
  IrBlock *idom;    // 直接の支配ブロック
  int rpo;          // 逆後順の番号
  int start, end;   // 線形化した命令の位置の範囲
  uint64_t *live_in, *live_out;
};

typedef struct {
  Node *node;       // ループ・switch の節点（break・continue の parent_loop）
  IrBlock *brk, *cont;
} IrLoop;

typedef struct {
  Obj *var;
  int ofs;          // x29 からのオフセット（負）
} IrSlot;

// ポインタの集合（開番地法）
typedef struct {
  void **keys;
  int cap, n;
} IrSet;

static void *ir_alloc(size_t sz);

static uint64_t ir_ptr_hash(void *p) {
  uint64_t x = (uint64_t)(uintptr_t)p;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return x;
}

static bool ir_set_has(IrSet *s, void *p) {
  if (!s->cap)
    return false;
  for (uint64_t i = ir_ptr_hash(p) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
    if (!s->keys[i])
      return false;
    if (s->keys[i] == p)
      return true;
  }
}

static void ir_set_add(IrSet *s, void *p) {
  if (ir_set_has(s, p))
    return;
  if ((s->n + 1) * 2 > s->cap) {
    IrSet t = {.cap = s->cap ? s->cap * 2 : 64};
    t.keys = ir_alloc(t.cap * sizeof(void *));
    for (int i = 0; i < s->cap; i++)
      if (s->keys[i])
        ir_set_add(&t, s->keys[i]);
    *s = t;
  }
  uint64_t i = ir_ptr_hash(p) & (s->cap - 1);
  while (s->keys[i])
    i = (i + 1) & (s->cap - 1);
  s->keys[i] = p;
  s->n++;
}

// 関数ごとの状態
TLS static jmp_buf ir_bail;
TLS static IrBlock **ir_blocks;
TLS static int ir_nblocks, ir_capblocks;
TLS static IrBlock *ir_cur;
TLS static int ir_nvals;
TLS static IrLoop *ir_loops;
TLS static int ir_nloops, ir_caploops;
typedef struct {
  Node *label;      // ND_LABEL、ND_CASE、ND_DEFAULT の節点
  IrBlock *bb;
  bool defined;     // ND_LABEL の文を通った
} IrLabel;
// 計算型の goto（IR_INDBR）
TLS static bool ir_has_labelval;    // &&label か goto *p がある
TLS static IrBlock *ir_dispatch;    // goto *p を集める振り分けのブロック
TLS static Obj *ir_dispatch_var;    // 振り分けの番地（goto *p の値を合流させる変数）
TLS static Node **ir_lbl_lists;     // ブロックの local_labels（番号を付ける）
TLS static int ir_nlbl_lists, ir_caplbl_lists;
TLS static int64_t ir_lbl_lo, ir_lbl_hi;  // この関数で付けた .L.jmp の番号の範囲

TLS static IrLabel *ir_labels;
TLS static int ir_nlabels, ir_caplabels;
TLS static IrSlot *ir_slots;
TLS static int ir_nslots, ir_capslots;
TLS static int ir_frame;           // 局所変数と逃がした値に使った量（x29 から下）
TLS static bool ir_has_alloca;     // __builtin_alloca を使う（インライン展開しない）
TLS static bool ir_stk_params;     // スタックで渡された引数がある（フレームを省かない。[x29 + 16] から読む）
TLS static IrSet ir_ssa_vars;      // SSA にする変数
TLS static Obj *ir_fn;
TLS static IrInst *ir_null_val;    // ND_NULL_EXPR の値（複合代入の左辺の今の値）
TLS static IrInst *ir_rtn_ptr;     // 大きな構造体を返す関数: 呼び出し元が x8 で渡した領域
// インラインアセンブリを含む関数: 今までのコード生成と同じく x19・x20 を [x29 - 16] に退避する
// （フレームの形を前提にしたアセンブリがある。test/asm.c）
TLS static bool ir_has_asm;

// インライン展開の文脈（return の行き先と結果の変数、ラベルの範囲）
typedef struct {
  Obj *fn;
  IrBlock *exit;
  Obj *result;
  int label_base;
  int rtn_ofs;      // 構造体の戻り値の場所（呼び出しの rtn_buf の slot。2026-11-21）
} IrInline;

#define IR_INLINE_DEPTH 4
#define IR_INLINE_SIZE 12         // 展開する関数の大きさ（最適化した後の命令の数。30 では .text が 2 割増えて速さは変わらなかった）
#define IR_INLINE_SIZE_ALWAYS 150 // always_inline の関数
TLS static IrInline ir_inlines[IR_INLINE_DEPTH];
TLS static int ir_ninlines;
TLS static Obj **ir_inlined;      // この関数に展開した関数（出力から参照する）
TLS static int ir_ninlined, ir_capinlined;
TLS static void **ir_allocs;
TLS static int ir_nallocs, ir_capallocs;

TLS static int ir_bail_line;
// ループの反転・展開で 2 か所目以降に出した分の値の数（インライン展開の大きさの判断から引く。反転の前と
// 同じ関数を展開するように。展開した本体は呼び出し元でも同じく反転・展開される）
TLS static int *ir_dup_rng;      // [始め, 終わり) の値の番号の組
TLS static int ir_ndup, ir_capdup;


// 扱えない（関数全体を今までのコード生成に任せる）。SLIMCC_IR_WHY を設定すると、理由の行を表示する
#define ir_unsupported() (ir_bail_line = __LINE__, longjmp(ir_bail, 1))

// 関数 1 つ分の確保は塊から切り出し、関数の終わりにまとめて捨てる（ir_free_all）。
// 塊は 8KB から倍々に大きくする（小さな関数で大きな塊をゼロにしないため）
TLS static char *ir_bump, *ir_bump_end;
TLS static size_t ir_chunk_size;

static void *ir_alloc_raw(size_t sz) {
  void *p = calloc(1, sz);
  if (ir_nallocs == ir_capallocs) {
    ir_capallocs = ir_capallocs ? ir_capallocs * 2 : 256;
    ir_allocs = realloc(ir_allocs, ir_capallocs * sizeof(void *));
  }
  ir_allocs[ir_nallocs++] = p;
  return p;
}

// 塊は関数をまたいで使い回し（ir_pool。コンパイルの終わりに忘れる: ir_pool_forget）、切り出すたびに
// その分だけ 0 にする（2026-11-15。前は関数ごとに新しい塊を calloc していた。Monocs の中ではゾーンの free が
// 何もしないので、コンパイルの終わりまで塊が増え続け、キャッシュに載っていないメモリを毎回 0 にしていた）
TLS static char **ir_pool;
TLS static size_t *ir_pool_size;
TLS static int ir_npool, ir_cappool, ir_pool_next;

TLS static IrRec *ir_recs_keep;
TLS static int ir_capkeep;
TLS static const IrRec **frag_order_keep;
TLS static int capfrag_keep;

static void ir_pool_forget(void) {
  ir_recs_keep = NULL;
  ir_capkeep = 0;
  frag_order_keep = NULL;
  capfrag_keep = 0;
  ir_pool = NULL;
  ir_pool_size = NULL;
  ir_npool = ir_cappool = ir_pool_next = 0;
}

// 関数ごとに中間表現で確保した量（ir_alloc_nz の合計。関数の終わり ir_free_all で、いちばん多い関数を覚える。
// slimcc_set_mem_hook が Monocs の dmesg に書く。2026-11-20）
TLS static size_t ir_fn_bytes;
TLS static const char *ir_cur_fn;
TLS static size_t ir_big_bytes;
TLS static char ir_big_fn[128];

void ir_mem_reset(void) {
  ir_fn_bytes = ir_big_bytes = 0;
  ir_big_fn[0] = '\0';
}

void ir_mem_biggest(const char **fn, size_t *bytes) {
  *fn = ir_big_fn;
  *bytes = ir_big_bytes;
}

// 0 にしない確保（すぐに全部を書くもの: ir_grow の写す部分）
static void *ir_alloc_nz(size_t sz) {
  sz = (sz + 15) & ~(size_t)15;
  ir_fn_bytes += sz;
  if (sz > (size_t)(ir_bump_end - ir_bump)) {
    if (sz >= 4096)
      return ir_alloc_raw(sz);
    if (ir_pool_next == ir_npool) {
      ir_chunk_size = ir_chunk_size ? ir_chunk_size * 2 : 8192;
      if (ir_chunk_size > (1 << 20))
        ir_chunk_size = 1 << 20;
      if (ir_npool == ir_cappool) {
        ir_cappool = ir_cappool ? ir_cappool * 2 : 16;
        ir_pool = realloc(ir_pool, sizeof(char *) * ir_cappool);
        ir_pool_size = realloc(ir_pool_size, sizeof(size_t) * ir_cappool);
      }
      ir_pool[ir_npool] = malloc(ir_chunk_size);
      ir_pool_size[ir_npool++] = ir_chunk_size;
    }
    ir_bump = ir_pool[ir_pool_next];
    ir_bump_end = ir_bump + ir_pool_size[ir_pool_next++];
  }
  void *p = ir_bump;
  ir_bump += sz;
  return p;
}

static void *ir_alloc(size_t sz) {
  void *p = ir_alloc_nz(sz);
  memset(p, 0, sz);
  return p;
}

// 広げた配列の、前の中身を写す部分は 0 にしない（新しい部分だけ 0 に。2026-11-16）
static void *ir_grow(void *p, int *cap, int need, size_t elem) {
  if (need <= *cap)
    return p;
  int n = *cap ? *cap * 2 : 4;
  while (n < need)
    n *= 2;
  char *q = ir_alloc_nz(elem * n);
  size_t old = p ? elem * *cap : 0;
  if (old)
    memcpy(q, p, old);
  memset(q + old, 0, elem * n - old);
  *cap = n;
  return q;
}

static void ir_dup_range(int from, int to) {
  if (from >= to)
    return;
  ir_dup_rng = ir_grow(ir_dup_rng, &ir_capdup, ir_ndup + 2, sizeof(int));
  ir_dup_rng[ir_ndup++] = from;
  ir_dup_rng[ir_ndup++] = to;
}

static bool ir_is_dup(int id) {
  for (int i = 0; i < ir_ndup; i += 2)
    if (id >= ir_dup_rng[i] && id < ir_dup_rng[i + 1])
      return true;
  return false;
}

static void ir_free_all(void) {
  if (ir_fn_bytes > ir_big_bytes) {
    ir_big_bytes = ir_fn_bytes;
    snprintf(ir_big_fn, sizeof ir_big_fn, "%s", ir_cur_fn ? ir_cur_fn : "?");
  }
  ir_fn_bytes = 0;
  for (int i = 0; i < ir_nallocs; i++)
    free(ir_allocs[i]);
  free(ir_allocs);
  ir_allocs = NULL;
  ir_nallocs = ir_capallocs = 0;
  ir_bump = ir_bump_end = NULL;
  ir_pool_next = 0;           // 塊は次の関数で使い回す
  ir_ssa_vars = (IrSet){0};
}

//
// ブロックと命令
//

static IrBlock *ir_new_block(void) {
  IrBlock *bb = ir_alloc(sizeof(IrBlock));
  bb->succ = bb->succ_buf;
  bb->id = ir_nblocks;
  ir_blocks = ir_grow(ir_blocks, &ir_capblocks, ir_nblocks + 1, sizeof(IrBlock *));
  ir_blocks[ir_nblocks++] = bb;
  return bb;
}

static IrInst *ir_resolve(IrInst *v) {
  while (v && v->repl)
    v = v->repl;
  return v;
}

static IrInst *ir_new(IrOp op) {
  IrInst *in = ir_alloc(sizeof(IrInst));
  in->op = op;
  in->id = ir_nvals++;
  return in;
}

static bool ir_is_term(IrInst *in) {
  return in && (in->op == IR_BR || in->op == IR_CBR || in->op == IR_RET || in->op == IR_UNREACH ||
                in->op == IR_INDBR || in->op == IR_SWTAB);
}

static void ir_append(IrBlock *bb, IrInst *in) {
  in->bb = bb;
  in->prev = bb->last;
  if (bb->last)
    bb->last->next = in;
  else
    bb->first = in;
  bb->last = in;
}

// phi はブロックの先頭に置く
static void ir_prepend(IrBlock *bb, IrInst *in) {
  in->bb = bb;
  in->next = bb->first;
  if (bb->first)
    bb->first->prev = in;
  else
    bb->last = in;
  bb->first = in;
}

static void ir_insert_before(IrInst *pos, IrInst *in) {
  IrBlock *bb = pos->bb;
  in->bb = bb;
  in->next = pos;
  in->prev = pos->prev;
  if (pos->prev)
    pos->prev->next = in;
  else
    bb->first = in;
  pos->prev = in;
}

// 今のブロックに命令を足す。終わったブロック（return などの後）なら、到達しない新しいブロックに置く
static IrInst *ir_emit(IrInst *in) {
  if (ir_is_term(ir_cur->last)) {
    ir_cur = ir_new_block();
    ir_cur->sealed = true;
  }
  ir_append(ir_cur, in);
  return in;
}

static void ir_add_pred(IrBlock *bb, IrBlock *pred) {
  bb->preds = ir_grow(bb->preds, &bb->cappreds, bb->npreds + 1, sizeof(IrBlock *));
  bb->preds[bb->npreds++] = pred;
}

static void ir_jump(IrBlock *to) {
  if (ir_is_term(ir_cur->last))
    return; // 到達しない
  IrInst *in = ir_new(IR_BR);
  ir_append(ir_cur, in);
  ir_cur->succ[0] = to;
  ir_cur->nsucc = 1;
  ir_add_pred(to, ir_cur);
}

static void ir_branch(IrInst *cond, IrBlock *t, IrBlock *f) {
  if (ir_is_term(ir_cur->last))
    return;
  IrInst *in = ir_new(IR_CBR);
  in->a = cond;
  ir_append(ir_cur, in);
  ir_cur->succ[0] = t;
  ir_cur->succ[1] = f;
  ir_cur->nsucc = 2;
  ir_add_pred(t, ir_cur);
  ir_add_pred(f, ir_cur);
}

static IrInst *ir_const(int64_t v) {
  IrInst *in = ir_new(IR_CONST);
  in->imm = v;
  return ir_emit(in);
}

static IrInst *ir_bin(IrOp op, IrInst *a, IrInst *b) {
  IrInst *in = ir_new(op);
  in->a = a;
  in->b = b;
  return ir_emit(in);
}

static IrInst *ir_un(IrOp op, IrInst *a) {
  IrInst *in = ir_new(op);
  in->a = a;
  return ir_emit(in);
}

static IrInst *ir_ext(IrInst *a, int size, bool sign) {
  if (size >= 8)
    return a;
  IrInst *in = ir_un(IR_EXT, a);
  in->size = size;
  in->sign = sign;
  return in;
}

static void ir_add_arg(IrInst *in, IrInst *v) {
  in->args = ir_grow(in->args, &in->capargs, in->nargs + 1, sizeof(IrInst *));
  in->args[in->nargs++] = v;
}

//
// SSA の構築（Braun et al.）
//

static void ir_set_class(IrInst *in, Type *ty);

static void ir_write_var(Obj *var, IrBlock *bb, IrInst *val) {
  for (int i = 0; i < bb->ndefs; i++)
    if (bb->defs[i].var == var) {
      bb->defs[i].val = val;
      return;
    }
  bb->defs = ir_grow(bb->defs, &bb->capdefs, bb->ndefs + 1, sizeof(IrDef));
  bb->defs[bb->ndefs++] = (IrDef){var, val};
}

static IrInst *ir_read_var(Obj *var, IrBlock *bb);

static IrInst *ir_try_remove_trivial_phi(IrInst *phi) {
  IrInst *same = NULL;
  for (int i = 0; i < phi->nargs; i++) {
    IrInst *op = ir_resolve(phi->args[i]);
    if (op == same || op == phi)
      continue;
    if (same)
      return phi;
    same = op;
  }
  if (!same) {
    same = ir_new(IR_UNDEF);
    same->fp = phi->fp;
    same->fsz = phi->fsz;
    ir_prepend(phi->bb, same);
  }
  phi->repl = same;
  return same;
}

static IrInst *ir_add_phi_operands(Obj *var, IrInst *phi) {
  for (int i = 0; i < phi->bb->npreds; i++)
    ir_add_arg(phi, ir_read_var(var, phi->bb->preds[i]));
  return ir_try_remove_trivial_phi(phi);
}

static IrInst *ir_new_phi(IrBlock *bb) {
  IrInst *phi = ir_new(IR_PHI);
  ir_prepend(bb, phi);
  return phi;
}

static IrInst *ir_read_var(Obj *var, IrBlock *bb) {
  for (int i = 0; i < bb->ndefs; i++)
    if (bb->defs[i].var == var)
      return ir_resolve(bb->defs[i].val);
  IrInst *val;
  if (!bb->sealed) {
    val = ir_new_phi(bb);
    ir_set_class(val, var->ty);
    bb->incomplete = ir_grow(bb->incomplete, &bb->capincomplete, bb->nincomplete + 1, sizeof(IrDef));
    bb->incomplete[bb->nincomplete++] = (IrDef){var, val};
  } else if (bb->npreds == 0) {
    val = ir_new(IR_UNDEF);
    ir_set_class(val, var->ty);
    ir_prepend(bb, val);
  } else if (bb->npreds == 1) {
    val = ir_read_var(var, bb->preds[0]);
  } else {
    IrInst *phi = ir_new_phi(bb);
    ir_set_class(phi, var->ty);
    ir_write_var(var, bb, phi);
    val = ir_add_phi_operands(var, phi);
  }
  ir_write_var(var, bb, val);
  return val;
}

static void ir_seal(IrBlock *bb) {
  if (bb->sealed)
    return;
  bb->sealed = true;
  for (int i = 0; i < bb->nincomplete; i++)
    ir_add_phi_operands(bb->incomplete[i].var, bb->incomplete[i].val);
  bb->nincomplete = 0;
}

//
// 型
//

// _Atomic の整数・ポインタも値として扱う（読み出しは ldar。書き込みは構文解析で ND_EXCH・ND_CAS になる。
// 変数は SSA にしない: ir_collect_vars）
static bool ir_scalar_ty(Type *ty) {
  if (ty->kind == TY_PTR || ty->kind == TY_NULLPTR)    // nullptr_t（C23）は値が 0 のポインタ
    return true;
  return is_integer(ty) && ty->kind != TY_BITINT && ty->size <= 8;
}

static bool ir_fp_ty(Type *ty) {
  return ty && (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE) && !(ty->qual & Q_ATOMIC);
}

// アトミック操作（IR_CAS・IR_XCHG）で扱える型: 整数・ポインタ（_Atomic でもよい。float・double は今までのコード生成）
static bool ir_atomic_ty(Type *ty) {
  if (ty->kind == TY_PTR)
    return true;
  return is_integer(ty) && ty->kind != TY_BITINT && ty->size <= 8;
}

// 中間表現の値として扱える型（整数・ポインタ、float・double）
static bool ir_value_ty(Type *ty) {
  return ir_scalar_ty(ty) || ir_fp_ty(ty);
}

static void ir_set_class(IrInst *in, Type *ty) {
  if (ir_fp_ty(ty)) {
    in->fp = true;
    in->fsz = ty->size;
  }
}

static bool ir_aggregate_ty(Type *ty) {
  return ty->kind == TY_ARRAY || ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_FUNC;
}

static bool ir_is_signed(Type *ty) {
  return ty->kind != TY_PTR && ty->kind != TY_BOOL && !ty->is_unsigned;
}

// 値を型に合わせた形にする
static IrInst *ir_normalize(IrInst *v, Type *ty) {
  if (ir_fp_ty(ty))
    return v;
  if (ty->kind == TY_BOOL)
    return ir_un(IR_BOOL, v);
  if (ty->kind == TY_PTR || ty->size >= 8)
    return v;
  return ir_ext(v, ty->size, ir_is_signed(ty));
}

//
// 局所変数
//

static bool ir_is_ssa_var(Obj *var) {
  return ir_set_has(&ir_ssa_vars, var);
}

// 変数の場所を決まったオフセットにする（スタックで渡された引数: x29 + 16 からの正のオフセット。2026-11-21）
static void ir_slot_at(Obj *var, int ofs) {
  ir_slots = ir_grow(ir_slots, &ir_capslots, ir_nslots + 1, sizeof(IrSlot));
  ir_slots[ir_nslots++] = (IrSlot){var, ofs};
}

static int ir_slot(Obj *var) {
  for (int i = 0; i < ir_nslots; i++)
    if (ir_slots[i].var == var)
      return ir_slots[i].ofs;
  if (var->ty->kind == TY_VLA || var->ty->size < 0)
    ir_unsupported();
  int align = MAX(var->ty->align, var->alt_align);
  if (align > 16)
    ir_unsupported();
  ir_frame = align_to(ir_frame + MAX(var->ty->size, 1), MAX(align, 1));
  ir_slots = ir_grow(ir_slots, &ir_capslots, ir_nslots + 1, sizeof(IrSlot));
  ir_slots[ir_nslots++] = (IrSlot){var, -ir_frame};
  return -ir_frame;
}

// 関数の中で、SSA にできない（アドレスが要る）局所変数を探す。
// 新しいコード生成が扱う種類の節点だけを辿り、それ以外があれば関数全体を今までのコード生成に任せる
static void ir_scan_addr(Node *node, IrSet *pinned);
static bool ir_ckd_ok(Node *node);

static void ir_scan_list(Node *n, IrSet *pinned) {
  for (; n; n = n->next)
    ir_scan_addr(n, pinned);
}

static void ir_pin(Node *lv, IrSet *pinned) {
  // &x、x.member など、変数そのものの場所が要る
  while (lv && (lv->kind == ND_MEMBER || lv->kind == ND_COMMA || lv->kind == ND_CHAIN))
    lv = lv->kind == ND_MEMBER ? lv->m.lhs : lv->m.rhs;
  if (lv && lv->kind == ND_VAR && lv->m.var->kind == OBJ_LOCAL)
    ir_set_add(pinned, lv->m.var);
}

static void ir_scan_addr(Node *node, IrSet *pinned) {
  if (!node)
    return;
  if (node->dfr_from || node->dfr_dest)
    ir_unsupported();
  switch (node->kind) {
  case ND_NUM:
  case ND_VAR:
  case ND_BREAK:
  case ND_CONT:
  case ND_NULL_EXPR:
  case ND_NULL_STMT:
  case ND_UNREACHABLE:
  case ND_CASE:
  case ND_DEFAULT:
  case ND_LABEL:
  case ND_GOTO:
    return;
  case ND_SWITCH:
    ir_scan_addr(node->ctrl.cond, pinned);
    ir_scan_addr(node->ctrl.then, pinned);
    return;
  case ND_ASM:
    ir_has_asm = true;
    for (int k = 0; k < 2; k++)
      for (AsmParam *ap = k ? node->gasm.inputs : node->gasm.outputs; ap; ap = ap->next) {
        if (ap->kind == ASMOP_MEM)
          ir_pin(ap->arg, pinned);
        ir_scan_addr(ap->arg, pinned);
      }
    return;
  case ND_ADDR:
  case ND_MEMBER:
    ir_pin(node->m.lhs, pinned);
    ir_scan_addr(node->m.lhs, pinned);
    return;
  case ND_INIT_SEQ:
    if (node->m.var->kind != OBJ_LOCAL)
      ir_unsupported();
    ir_set_add(pinned, node->m.var);
    ir_scan_list(node->m.lhs, pinned);
    return;
  case ND_FUNCALL:
    if (node->call.alloca_args)
      ir_unsupported();
    if (node->call.rtn_buf)
      ir_set_add(pinned, node->call.rtn_buf);
    ir_scan_addr(node->call.expr, pinned);
    for (Obj *a = node->call.args; a; a = a->param_next)
      ir_scan_addr(a->arg_expr, pinned);
    return;
  case ND_BLOCK:
  case ND_STMT_EXPR:
    // blk.local_labels は goto の飛び先のラベル（関数本体のものと、GNU の __label__ のもの）。
    // ラベルは節で引くので、どちらもそのまま扱える（defer を越える goto は、別の所で断る）
    if (node->blk.local_labels) {
      ir_lbl_lists = ir_grow(ir_lbl_lists, &ir_caplbl_lists, ir_nlbl_lists + 1, sizeof(Node *));
      ir_lbl_lists[ir_nlbl_lists++] = node->blk.local_labels;
    }
    ir_scan_list(node->blk.body, pinned);
    return;
  case ND_LABEL_VAL:
    ir_has_labelval = true;
    return;
  case ND_GOTO_EXPR:
    ir_has_labelval = true;
    ir_scan_addr(node->m.lhs, pinned);
    return;
  case ND_IF:
  case ND_COND:
    ir_scan_addr(node->ctrl.cond, pinned);
    ir_scan_addr(node->ctrl.then, pinned);
    ir_scan_addr(node->ctrl.els, pinned);
    return;
  case ND_FOR:
    ir_scan_addr(node->ctrl.for_init, pinned);
    ir_scan_addr(node->ctrl.cond, pinned);
    ir_scan_addr(node->ctrl.for_inc, pinned);
    ir_scan_addr(node->ctrl.then, pinned);
    return;
  case ND_DO:
    ir_scan_addr(node->ctrl.cond, pinned);
    ir_scan_addr(node->ctrl.then, pinned);
    return;
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_REM:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR: case ND_SAR:
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
  case ND_ASSIGN: case ND_ARITH_ASSIGN: case ND_POST_INCDEC:
  case ND_COMMA: case ND_CHAIN: case ND_LOGAND: case ND_LOGOR:
    ir_scan_addr(node->m.lhs, pinned);
    ir_scan_addr(node->m.rhs, pinned);
    return;
  case ND_POS: case ND_NEG: case ND_BITNOT: case ND_NOT: case ND_CAST: case ND_DEREF:
  case ND_EXPR_STMT: case ND_RETURN:
    ir_scan_addr(node->m.lhs, pinned);
    return;
  case ND_CKD_ARITH:
    if (!ir_ckd_ok(node))
      ir_unsupported();
    ir_scan_addr(node->m.lhs, pinned);
    ir_scan_addr(node->m.rhs, pinned);
    ir_scan_addr(node->m.target, pinned);
    return;
  case ND_VA_ARG:
    if (va_arg_need_copy(node->ty->base))
      ir_unsupported();  // HFA は局所変数に組み立て直す（今までのコード生成で）
    ir_scan_addr(node->m.lhs, pinned);
    return;
  case ND_ALLOCA:
    // __builtin_alloca だけ（2026-11-21。VLA は解放の仕組みがあるので今までのコード生成）
    if (node->m.var)
      ir_unsupported();
    ir_has_alloca = true;
    ir_scan_addr(node->m.lhs, pinned);
    return;
  case ND_VA_COPY:
    ir_scan_addr(node->m.lhs, pinned);
    ir_scan_addr(node->m.rhs, pinned);
    return;
  case ND_CAS:
    if (!ir_atomic_ty(node->cas.addr->ty->base))
      ir_unsupported();
    ir_scan_addr(node->cas.addr, pinned);
    ir_scan_addr(node->cas.old_val, pinned);
    ir_scan_addr(node->cas.new_val, pinned);
    return;
  case ND_EXCH:
    if (!ir_atomic_ty(node->ty))
      ir_unsupported();
    ir_scan_addr(node->m.lhs, pinned);
    ir_scan_addr(node->m.rhs, pinned);
    return;
  default:
    ir_bail_line = 100000 + node->kind;
    longjmp(ir_bail, 1);
  }
}

static void ir_collect_vars(Scope *sc, IrSet *pinned) {
  for (Obj *var = sc->locals; var; var = var->next) {
    if (var->kind != OBJ_LOCAL)
      continue;
    if (!ir_value_ty(var->ty) || (var->ty->qual & (Q_VOLATILE | Q_ATOMIC)) || ir_set_has(pinned, var))
      continue;
    ir_set_add(&ir_ssa_vars, var);
  }
  for (Scope *sub = sc->children; sub; sub = sub->sibling_next)
    ir_collect_vars(sub, pinned);
}

//
// AST から命令へ
//

static IrInst *ir_expr(Node *node);
static void ir_stmt(Node *node);
static void ir_cond(Node *node, IrBlock *t, IrBlock *f);

static IrInst *ir_sym_addr(Obj *var) {
  if (var->kind == OBJ_TLS) {
    // local-exec・initial-exec だけ（TLS ディスクリプタと emulated TLS は関数を呼ぶ）
    if (opt_fpic || opt_femulated_tls)
      ir_unsupported();
    // スレッドポインタを別の命令にして、関数の中で 1 回にまとめ、ループの外にも出せるようにする（2026-11-16。
    // 前は変数を使うたびに mrs・add・add・mov の 4 命令を、ループの中でもくり返していた）
    IrInst *in = ir_new(IR_TLSADDR);
    in->a = ir_emit(ir_new(IR_TP));
    in->sym = var;
    return ir_emit(in);
  }
  IrInst *in = ir_new(IR_GADDR);
  in->sym = var;
  return ir_emit(in);
}

// 左辺値のアドレス
static IrInst *ir_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->m.var;
    if (var->kind == OBJ_LOCAL) {
      if (ir_is_ssa_var(var))
        ir_unsupported();  // (int){3} = 5 など、レジスタに置いた変数の番地（今までのコード生成で）
      IrInst *in = ir_new(IR_LADDR);
      in->imm = ir_slot(var);
      return ir_emit(in);
    }
    if (var->kind == OBJ_INDIR)
      ir_unsupported();
    return ir_sym_addr(var);
  }
  case ND_DEREF:
    return ir_expr(node->m.lhs);
  case ND_MEMBER: {
    if (node->m.member->is_bitfield)
      ir_unsupported();
    IrInst *base = ir_addr(node->m.lhs);
    if (!node->m.member->offset)
      return base;
    return ir_bin(IR_ADD, base, ir_const(node->m.member->offset));
  }
  case ND_COMMA:
  case ND_CHAIN:
    ir_expr(node->m.lhs);
    return ir_addr(node->m.rhs);
  case ND_ASSIGN:
  case ND_COND:
  case ND_CAST:
  case ND_FUNCALL:
  case ND_STMT_EXPR:
    // 構造体の値はアドレス（今までのコード生成の gen_addr と同じ）
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
      return ir_expr(node);
    ir_unsupported();
  default:
    ir_unsupported();
  }
}

static bool ir_struct_ty(Type *ty) {
  return ty->kind == TY_STRUCT || ty->kind == TY_UNION;
}

static void ir_memcpy(IrInst *dst, IrInst *src, int64_t size) {
  IrInst *in = ir_new(IR_MEMCPY);
  in->a = dst;
  in->b = src;
  in->imm = size;
  ir_emit(in);
}

static IrInst *ir_laddr(int ofs) {
  IrInst *in = ir_new(IR_LADDR);
  in->imm = ofs;
  return ir_emit(in);
}

static uint8_t ir_tbaa(Type *ty);

static IrInst *ir_load(IrInst *addr, Type *ty) {
  if (ir_aggregate_ty(ty))
    return addr; // 配列・構造体・関数の「値」はアドレス
  if (!ir_value_ty(ty))
    ir_unsupported();
  IrInst *in = ir_new(IR_LOAD);
  ir_set_class(in, ty);
  in->a = addr;
  in->size = ty->size;
  in->sign = ir_is_signed(ty);
  in->vol = (ty->qual & (Q_VOLATILE | Q_ATOMIC)) != 0;
  in->acq = (ty->qual & Q_ATOMIC) != 0;
  in->tbaa = ir_tbaa(ty);
  return ir_emit(in);
}

// 型の組（2026-11-16）: 大きさの同じ整数（符号は問わない）、ポインタ、float、double。char・_Bool・
// 構造体などは 0（何とでも重なる。char で読み書きするのは C で許されている）
static uint8_t ir_tbaa(Type *ty) {
  if (!opt_strict_aliasing)
    return 0;
  switch (ty->kind) {
  case TY_SHORT: case TY_INT: case TY_LONG: case TY_LONGLONG:
    return ty->size == 2 ? 1 : ty->size == 4 ? 2 : ty->size == 8 ? 3 : 0;
  case TY_PTR: case TY_NULLPTR:
    return 4;
  case TY_FLOAT:
    return 5;
  case TY_DOUBLE:
    return 6;
  default:
    return 0;
  }
}

static IrInst *ir_store(IrInst *addr, IrInst *val, Type *ty) {
  if (!ir_value_ty(ty))
    ir_unsupported();
  IrInst *in = ir_new(IR_STORE);
  in->a = addr;
  in->b = val;
  in->size = ty->size;
  in->vol = (ty->qual & (Q_VOLATILE | Q_ATOMIC)) != 0;
  in->tbaa = ir_tbaa(ty);
  return ir_emit(in);
}

static void ir_copy_words(IrInst *dst, IrInst *src, int64_t size);
static IrInst *ir_load_word(IrInst *addr, int64_t k);
static void ir_store_word(IrInst *addr, int64_t k, IrInst *v);

// 8 バイトの語に分けて扱う構造体（2026-11-21。SROA）: 大きさが 8 の倍数で 32 バイトまで、境界が 8 以上。
// 代入・0 での初期化・レジスタでの受け渡しを 8 バイトずつの読み書きにし、局所変数を値にできるようにする
// （ir_mem2reg）
static bool ir_word_struct(Type *ty) {
  return ty->size > 0 && ty->size % 8 == 0 && ty->size <= 32 && ty->align >= 8;
}

// レジスタ（x）で渡す・返す構造体で、語に分けられるもの（16 バイトまで。HFA でない）
static bool ir_word_reg_struct(Type *ty) {
  if (!ir_struct_ty(ty) || !ir_word_struct(ty) || ty->size > 16)
    return false;
  return classify(ty).cls == AC_GP;
}

static IrInst *ir_load_word(IrInst *addr, int64_t k) {
  IrInst *v = ir_load(k ? ir_bin(IR_ADD, addr, ir_const(k)) : addr, ty_ulong);
  v->tbaa = 0;
  return v;
}

static void ir_store_word(IrInst *addr, int64_t k, IrInst *v) {
  ir_store(k ? ir_bin(IR_ADD, addr, ir_const(k)) : addr, v, ty_ulong)->tbaa = 0;
}

// 構造体を写す（インライン展開の引数・戻り値。2026-11-21）。8 バイトの倍数で 32 バイトまでは、8 バイトずつの
// 読み書きにする（IR_MEMCPY は呼び出しと同じ扱いで、知っている中身を捨て・ループの外に出す最適化を止めるため）。
// 型の組は 0（何とでも重なる。構造体の中身はどの型で書かれたかわからない）
static void ir_copy_struct(IrInst *dst, IrInst *src, int64_t size) {
  if (size % 8 || size > 32) {
    ir_memcpy(dst, src, size);
    return;
  }
  ir_copy_words(dst, src, size);
}

// 8 バイトずつ写す（型の組 0）
static void ir_copy_words(IrInst *dst, IrInst *src, int64_t size) {
  for (int64_t k = 0; k < size; k += 8)
    ir_store_word(dst, k, ir_load_word(src, k));
}

static Type *ir_canon_type(Type *ty) {
  while (ty->origin)
    ty = ty->origin;
  return ty;
}

// 読み書き mem が、左辺値 lv（構造体のメンバー）に対するものなら、目印を付ける
static IrInst *ir_tag_member(IrInst *mem, Node *lv) {
  if (mem && (mem->op == IR_LOAD || mem->op == IR_STORE) && lv->kind == ND_MEMBER &&
      !lv->m.member->is_bitfield && lv->m.lhs->ty->kind == TY_STRUCT) {
    mem->mem_struct = ir_canon_type(lv->m.lhs->ty);
    mem->mem_off = lv->m.member->offset;
  }
  return mem;
}

static bool ir_is_ssa_node(Node *node) {
  return node->kind == ND_VAR && node->m.var->kind == OBJ_LOCAL && ir_is_ssa_var(node->m.var);
}

// 境界にそろったビットフィールド（格納単位を読み書きする。今までのコード生成と同じ単位）
static bool ir_is_bitfield(Node *node) {
  return node->kind == ND_MEMBER && node->m.member->is_bitfield;
}

static Member *ir_bitfield(Node *node) {
  Member *mem = node->m.member;
  if (!mem->is_aligned_bitfield || mem->ty->kind == TY_BITINT || !ir_scalar_ty(mem->ty))
    ir_unsupported();
  return mem;
}

static IrInst *ir_bitfield_unit_addr(Node *node) {
  IrInst *base = ir_addr(node->m.lhs);
  if (!node->m.member->offset)
    return base;
  return ir_bin(IR_ADD, base, ir_const(node->m.member->offset));
}

static int ir_bf_unit(Member *mem) {
  return next_pow_of_two(mem->bit_offset + mem->bit_width) / 8;
}

// v の下位 width ビットを、型に合わせて拡張する
static IrInst *ir_bf_extend(IrInst *v, int width, bool sign) {
  if (width >= 64)
    return v;
  IrInst *l = ir_bin(IR_SHL, v, ir_const(64 - width));
  return ir_bin(sign ? IR_SAR : IR_SHR, l, ir_const(64 - width));
}

static IrInst *ir_bitfield_load(Node *node, IrInst *addr) {
  Member *mem = ir_bitfield(node);
  int sz = ir_bf_unit(mem);
  bool sign = ir_is_signed(mem->ty);
  IrInst *ld = ir_new(IR_LOAD);
  ld->a = addr;
  ld->size = sz;
  ld->sign = mem->bit_width == sz * 8 && sign;
  ld->vol = (node->ty->qual & Q_VOLATILE) != 0;
  ir_emit(ld);
  if (mem->bit_width == sz * 8)
    return ld;
  IrInst *v = mem->bit_offset ? ir_bin(IR_SHR, ld, ir_const(mem->bit_offset)) : ld;
  return ir_bf_extend(v, mem->bit_width, sign);
}

// 書き込み、式の値（書いた値を幅で切り詰めて拡張したもの）を返す
static IrInst *ir_bitfield_store(Node *node, IrInst *addr, IrInst *val) {
  Member *mem = ir_bitfield(node);
  int sz = ir_bf_unit(mem);
  IrInst *st = ir_new(IR_STORE);
  st->a = addr;
  st->size = sz;
  st->vol = (node->ty->qual & Q_VOLATILE) != 0;
  if (mem->bit_width == sz * 8) {
    st->b = val;
  } else {
    int64_t mask = (int64_t)((1ULL << mem->bit_width) - 1);
    IrInst *old = ir_new(IR_LOAD);
    old->a = addr;
    old->size = sz;
    old->vol = st->vol;
    ir_emit(old);
    IrInst *cleared = ir_bin(IR_AND, old, ir_const((int64_t)~((uint64_t)mask << mem->bit_offset)));
    IrInst *ins = ir_bin(IR_AND, val, ir_const(mask));
    if (mem->bit_offset)
      ins = ir_bin(IR_SHL, ins, ir_const(mem->bit_offset));
    st->b = ir_bin(IR_OR, cleared, ins);
  }
  ir_emit(st);
  return ir_bf_extend(val, mem->bit_width, ir_is_signed(mem->ty));
}

static IrInst *ir_read_lvalue(Node *lv, IrInst **addr) {
  if (ir_is_bitfield(lv)) {
    *addr = ir_bitfield_unit_addr(lv);
    return ir_bitfield_load(lv, *addr);
  }
  if (ir_is_ssa_node(lv)) {
    *addr = NULL;
    return ir_read_var(lv->m.var, ir_cur);
  }
  *addr = ir_addr(lv);
  return ir_tag_member(ir_load(*addr, lv->ty), lv);
}

static void ir_write_lvalue(Node *lv, IrInst *addr, IrInst *val) {
  if (ir_is_bitfield(lv))
    ir_bitfield_store(lv, addr, val);
  else if (ir_is_ssa_node(lv))
    ir_write_var(lv->m.var, ir_cur, val);
  else
    ir_tag_member(ir_store(addr, val, lv->ty), lv);
}

static IrInst *ir_fconst(uint64_t bits, int fsz) {
  IrInst *in = ir_new(IR_FCONST);
  in->imm = (int64_t)bits;
  in->fp = true;
  in->fsz = fsz;
  return ir_emit(in);
}

static IrInst *ir_fcmp(IrInst *a, IrInst *b, int cond) {
  IrInst *in = ir_bin(IR_FCMP, a, b);
  in->cond = cond;
  return in;
}

static IrInst *ir_fzero(int fsz) {
  return ir_fconst(0, fsz);
}

static IrInst *ir_cast(IrInst *v, Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return v;
  if (ir_struct_ty(to) && ir_struct_ty(from))
    return v; // 構造体の値（アドレス）のまま
  if (!ir_value_ty(to) || (!ir_value_ty(from) && !ir_aggregate_ty(from)))
    ir_unsupported();
  if (ir_fp_ty(to)) {
    if (ir_fp_ty(from)) {
      if (from->size == to->size)
        return v;
      IrInst *in = ir_un(IR_F2F, v);
      ir_set_class(in, to);
      return in;
    }
    IrInst *in = ir_un(IR_I2F, v);
    in->sign = ir_is_signed(from);
    ir_set_class(in, to);
    return in;
  }
  if (ir_fp_ty(from)) {
    if (to->kind == TY_BOOL)
      return ir_fcmp(v, ir_fzero(from->size), CC_NE);
    IrInst *in = ir_un(IR_F2I, v);
    in->sign = ir_is_signed(to);
    return ir_normalize(in, to);
  }
  return ir_normalize(v, to);
}

// 二項演算（オペランドは通常の算術変換を済ませてある）
static IrInst *ir_binary(NodeKind kind, IrInst *l, IrInst *r, Type *opty, Type *resty) {
  if (ir_fp_ty(opty)) {
    IrOp fop;
    switch (kind) {
    case ND_ADD: fop = IR_FADD; break;
    case ND_SUB: fop = IR_FSUB; break;
    case ND_MUL: fop = IR_FMUL; break;
    case ND_DIV: fop = IR_FDIV; break;
    case ND_EQ: return ir_fcmp(l, r, CC_EQ);
    case ND_NE: return ir_fcmp(l, r, CC_NE);
    case ND_LT: return ir_fcmp(l, r, CC_FLT);
    case ND_LE: return ir_fcmp(l, r, CC_FLE);
    case ND_GT: return ir_fcmp(l, r, CC_FGT);
    case ND_GE: return ir_fcmp(l, r, CC_FGE);
    default: ir_unsupported();
    }
    IrInst *in = ir_bin(fop, l, r);
    ir_set_class(in, opty);
    return in;
  }
  bool sign = ir_is_signed(opty);
  IrOp op;
  switch (kind) {
  case ND_ADD: op = IR_ADD; break;
  case ND_SUB: op = IR_SUB; break;
  case ND_MUL: op = IR_MUL; break;
  case ND_DIV: op = sign ? IR_SDIV : IR_UDIV; break;
  case ND_REM: op = sign ? IR_SREM : IR_UREM; break;
  case ND_BITAND: op = IR_AND; break;
  case ND_BITOR: op = IR_OR; break;
  case ND_BITXOR: op = IR_XOR; break;
  case ND_SHL: op = IR_SHL; break;
  case ND_SHR: op = IR_SHR; break;
  case ND_SAR: op = IR_SAR; break;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE: {
    IrInst *in = ir_bin(IR_CMP, l, r);
    switch (kind) {
    case ND_EQ: in->cond = CC_EQ; break;
    case ND_NE: in->cond = CC_NE; break;
    case ND_LT: in->cond = sign ? CC_LT : CC_LO; break;
    case ND_LE: in->cond = sign ? CC_LE : CC_LS; break;
    case ND_GT: in->cond = sign ? CC_GT : CC_HI; break;
    default:    in->cond = sign ? CC_GE : CC_HS; break;
    }
    return in;
  }
  default:
    ir_unsupported();
  }
  // 32 ビット以下の右シフトは、拡張した値で正しい（符号なしはゼロ拡張、符号つきは符号拡張してある）
  IrInst *in = ir_bin(op, l, r);
  if (opty->size == 4 && resty->size == 4) {
    // 符号つきの int: あふれは未定義動作なので、正しいプログラムでは結果は 32 ビットに収まる（GCC と同じ前提）
    if (sign && (op == IR_ADD || op == IR_SUB || op == IR_MUL || op == IR_SDIV || op == IR_SREM))
      in->nsw = true;
    // 符号なしの 32 ビット: w レジスタの命令にすれば、結果は自動でゼロ拡張される
    if (!sign && (op == IR_ADD || op == IR_SUB || op == IR_MUL || op == IR_SHL))
      in->wform = true;
  }
  return ir_normalize(in, resty);
}

static bool ir_is_binary(NodeKind k) {
  switch (k) {
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_REM:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR: case ND_SAR:
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
    return true;
  default:
    return false;
  }
}

// 複合代入・後置の増減の右辺: codegen.c の gen_expr_null_lhs と同じ式を作り、ND_NULL_EXPR を cur にする
static IrInst *ir_update(NodeKind kind, Type *ty, Node *rhs, IrInst *cur) {
  Node null = {.kind = ND_NULL_EXPR, .ty = ty, .tok = rhs->tok};
  Node expr = {.kind = kind, .m.lhs = &null, .m.rhs = rhs, .tok = rhs->tok};
  add_type(&expr);
  IrInst *saved = ir_null_val;
  ir_null_val = cur;
  IrInst *v = ir_expr(new_cast(&expr, ty));
  ir_null_val = saved;
  return v;
}

// 積和演算にまとめる（1 つの式の中の a*b + c など。C の FP_CONTRACT。GCC と同じく、既定では GNU の規格の
// ときだけ。-ffp-contract=on・off で変えられる）。掛け算の途中で丸めないので、最後の桁が変わることがある
static bool ir_fp_mul_of(Node *n, Type *ty) {
  return n->kind == ND_MUL && n->ty->kind == ty->kind && n->m.lhs->ty->kind == ty->kind &&
         n->m.rhs->ty->kind == ty->kind;
}

static IrInst *ir_fp_contract(Node *node) {
  Type *ty = node->ty;
  bool on = opt_fp_contract >= 0 ? opt_fp_contract : !is_iso_std;
  if (!on || (node->kind != ND_ADD && node->kind != ND_SUB) ||
      (ty->kind != TY_DOUBLE && ty->kind != TY_FLOAT) || node->m.lhs->ty->kind != ty->kind ||
      node->m.rhs->ty->kind != ty->kind)
    return NULL;
  Node *l = node->m.lhs, *r = node->m.rhs;
  IrOp op;
  IrInst *a, *b, *c;
  if (ir_fp_mul_of(r, ty)) {    // c + a*b、c - a*b（左から順に評価する）
    c = ir_expr(l);
    a = ir_expr(r->m.lhs);
    b = ir_expr(r->m.rhs);
    op = node->kind == ND_ADD ? IR_FMADD : IR_FMSUB;
  } else if (ir_fp_mul_of(l, ty)) {     // a*b + c、a*b - c
    a = ir_expr(l->m.lhs);
    b = ir_expr(l->m.rhs);
    c = ir_expr(r);
    op = node->kind == ND_ADD ? IR_FMADD : IR_FNMSUB;
  } else {
    return NULL;
  }
  IrInst *in = ir_new(op);
  in->a = a;
  in->b = b;
  in->c = c;
  ir_set_class(in, ty);
  return ir_emit(in);
}

// sqrt・sqrtf・fabs・fabsf（この翻訳単位で定義していないもの）は命令にする
static IrInst *ir_math_builtin(Node *node) {
  Node *fexpr = node->call.expr;
  if (!is_direct_fncall(fexpr))
    return NULL;
  Obj *fn = fexpr->m.var;
  Obj *arg = node->call.args;
  if (fn->is_definition || !fn->name || !arg || arg->param_next || !ir_fp_ty(arg->ty) ||
      !ir_fp_ty(node->ty) || arg->ty->size != node->ty->size)
    return NULL;
  int fsz = node->ty->size;
  bool is_sqrt = !strcmp(fn->name, fsz == 8 ? "sqrt" : "sqrtf");
  bool is_fabs = !strcmp(fn->name, fsz == 8 ? "fabs" : "fabsf");
  if (!is_sqrt && !is_fabs)
    return NULL;
  IrInst *v = ir_cast(ir_expr(arg->arg_expr), arg->arg_expr->ty, arg->ty);
  IrInst *in = ir_un(is_sqrt ? IR_FSQRT : IR_FABS, v);
  in->fp = true;
  in->fsz = fsz;
  in->sym = fn;
  return in;
}

static bool ir_can_inline(Node *node) {
  Node *fexpr = node->call.expr;
  if (!is_direct_fncall(fexpr) || ir_ninlines == IR_INLINE_DEPTH)
    return false;
  Obj *fn = fexpr->m.var;
  if (!fn->ir_inlinable || fn == ir_fn || fn->is_weak)
    return false;
  if (fn->ir_size > (fn->is_always_inline ? IR_INLINE_SIZE_ALWAYS : IR_INLINE_SIZE))
    return false;
  for (int i = 0; i < ir_ninlines; i++)
    if (ir_inlines[i].fn == fn)
      return false; // 再帰
  for (Obj *a = node->call.args; a; a = a->param_next)
    if (!ir_value_ty(a->ty) && !ir_struct_ty(a->ty))
      return false;
  if (ir_struct_ty(node->ty) && !node->call.rtn_buf)
    return false;
  return true;
}

static IrInst *ir_inline_call(Node *node) {
  Obj *fn = node->call.expr->m.var;
  // 引数は呼び出し側で評価する
  int n = 0;
  for (Obj *a = node->call.args; a; a = a->param_next)
    n++;
  IrInst **vals = ir_alloc(sizeof(IrInst *) * (n + 1));
  n = 0;
  for (Obj *a = node->call.args; a; a = a->param_next, n++)
    vals[n] = ir_struct_ty(a->ty) ? ir_expr(a->arg_expr) // 構造体はアドレス（下で引数の場所に写す）
                                  : ir_cast(ir_expr(a->arg_expr), a->arg_expr->ty, a->ty);

  // 呼ばれる側の変数（SSA にするもの）を加える
  IrSet pinned = {0};
  bool saved_asm = ir_has_asm;
  ir_scan_addr(fn->body, &pinned);
  ir_has_asm = saved_asm;
  ir_collect_vars(fn->ty->scopes, &pinned);

  IrInline *ctx = &ir_inlines[ir_ninlines++];
  ctx->fn = fn;
  ctx->exit = ir_new_block();
  ctx->result = ir_alloc(sizeof(Obj));
  ctx->result->ty = fn->ty->return_ty;
  ctx->label_base = ir_nlabels;
  ctx->rtn_ofs = ir_struct_ty(fn->ty->return_ty) ? ir_slot(node->call.rtn_buf) : 0;

  n = 0;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next, n++) {
    if (ir_struct_ty(p->ty)) {
      ir_copy_struct(ir_laddr(ir_slot(p)), vals[n], p->ty->size);
      continue;
    }
    IrInst *v = ir_normalize(vals[n], p->ty);
    if (ir_is_ssa_var(p)) {
      ir_write_var(p, ir_cur, v);
    } else {
      IrInst *addr = ir_laddr(ir_slot(p));
      ir_store(addr, v, p->ty);
    }
  }
  ir_stmt(fn->body);
  ir_jump(ctx->exit); // 最後まで来た（戻り値は不定）
  for (int i = ctx->label_base; i < ir_nlabels; i++)
    ir_seal(ir_labels[i].bb);
  ir_nlabels = ctx->label_base;
  ir_seal(ctx->exit);
  ir_cur = ctx->exit;
  Obj *result = ctx->result;
  ir_ninlines--;

  ir_inlined = ir_grow(ir_inlined, &ir_capinlined, ir_ninlined + 1, sizeof(Obj *));
  ir_inlined[ir_ninlined++] = fn;
  if (fn->ty->return_ty->kind == TY_VOID)
    return ir_const(0);
  if (ir_struct_ty(fn->ty->return_ty))
    return ir_laddr(ctx->rtn_ofs);   // ctx は ir_inlines の中（ir_ninlines を戻しても中身はそのまま）
  return ir_read_var(result, ir_cur);
}

static IrInst *ir_funcall(Node *node) {
  Node *fexpr = node->call.expr;
  if (node->call.alloca_args)
    ir_unsupported();
  if (is_setjmp_call(fexpr))
    ir_unsupported();
  Type *rty = node->ty;
  bool struct_rtn = ir_struct_ty(rty);
  if (rty->kind != TY_VOID && !ir_value_ty(rty) && !struct_rtn)
    ir_unsupported();
  if (struct_rtn && !node->call.rtn_buf)
    ir_unsupported();
  IrInst *builtin = ir_math_builtin(node);
  if (builtin)
    return builtin;
  if (ir_can_inline(node))
    return ir_inline_call(node);
  int gp = 0, fp = 0;
  int stk = calling_convention(node->call.args, &gp, &fp);
  IrInst *in = ir_new(IR_CALL);
  in->stkargs = align_to(stk, 16);
  int nargs = 0;
  for (Obj *a = node->call.args; a; a = a->param_next)
    nargs++;
  in->argregs = ir_alloc(sizeof(int) * (2 * nargs + 1));     // 構造体は 2 語まで（ir_word_reg_struct）
  in->argtys = ir_alloc(sizeof(Type *) * (2 * nargs + 1));
  for (Obj *a = node->call.args; a; a = a->param_next) {
    if (a->is_zero_sized_arg)
      ir_unsupported();
    IrInst *v = ir_expr(a->arg_expr);
    // スタックで渡すもの（2026-11-21）: 呼ぶ直前に [sp + stack_offset] に書く（IR_CALL の出力）。参照渡しの
    // ポインタがスタックになるときも同じ（値はコピーのアドレス）
    bool on_stack = a->pass_by_stack || a->arg_reg < 0;
    in->argregs[in->nargs] = on_stack ? -1 - a->stack_offset : a->arg_reg;
    if (a->pass_by_ref) {
      // 大きな構造体: コピーを作り、そのアドレスを x レジスタで渡す
      int ofs = ir_slot(a);
      IrInst *copy = ir_laddr(ofs);
      ir_memcpy(copy, v, a->ty->size);
      in->argtys[in->nargs] = NULL;
      ir_add_arg(in, ir_laddr(ofs));
      continue;
    }
    if (!on_stack && ir_word_reg_struct(a->ty)) {
      // 語に分けて、x レジスタの値として渡す（SROA。2026-11-21）
      int r0 = a->arg_reg;
      for (int64_t k = 0; k < a->ty->size; k += 8) {
        in->argregs[in->nargs] = r0 + (int)(k / 8);
        in->argtys[in->nargs] = NULL;
        ir_add_arg(in, ir_load_word(v, k));
      }
      continue;
    }
    if (ir_struct_ty(a->ty)) {
      ArgInfo ai = classify(a->ty);
      if (ai.cls != AC_GP && ai.cls != AC_HFA)
        ir_unsupported();
      in->argtys[in->nargs] = a->ty; // アドレスから x（HFA なら v）レジスタに読み込む
      ir_add_arg(in, v);
      continue;
    }
    if (!ir_value_ty(a->ty))
      ir_unsupported();
    in->argtys[in->nargs] = NULL;
    ir_add_arg(in, ir_cast(v, a->arg_expr->ty, a->ty));
  }
  if (is_direct_fncall(fexpr))
    in->sym = fexpr->m.var;
  else
    in->a = ir_expr(fexpr);
  bool word_rtn = struct_rtn && ir_word_reg_struct(rty);
  if (word_rtn) {
    // x0・x1 を値として受け取り、戻り値の領域に書く（SROA。2026-11-21）
    in->size = 8;
    in->sign = false;
    ir_set_class(in, ty_ulong);
  } else if (struct_rtn) {
    if (!is_mem_rtn(rty)) {
      ArgInfo ai = classify(rty);
      if (ai.cls != AC_GP && ai.cls != AC_HFA)
        ir_unsupported();
    }
    in->ty = rty;
    in->rtn_slot = ir_slot(node->call.rtn_buf);
    in->size = 0;
  } else {
    in->size = rty->kind == TY_VOID ? 0 : rty->size;
    in->sign = rty->kind != TY_VOID && ir_is_signed(rty);
    ir_set_class(in, rty);
  }
  ir_emit(in);
  if (in->sym && in->sym->is_noreturn) {
    // 戻らない（今までのコード生成と同じく、後ろのコードは出力しない）
    ir_emit(ir_new(IR_UNREACH));
    return ir_const(0);
  }
  if (word_rtn) {
    IrInst *x1 = NULL;
    if (rty->size > 8) {
      x1 = ir_new(IR_X1);
      x1->size = 8;
      ir_set_class(x1, ty_ulong);
      ir_emit(x1);
    }
    IrInst *buf = ir_laddr(ir_slot(node->call.rtn_buf));
    ir_store_word(buf, 0, in);
    if (x1)
      ir_store_word(buf, 8, x1);
    return ir_laddr(ir_slot(node->call.rtn_buf));
  }
  if (struct_rtn)
    return ir_laddr(in->rtn_slot);
  if (rty->kind == TY_BOOL) {
    in->rbool = true;
    return ir_un(IR_BOOL, ir_ext(in, 1, false));
  }
  return in;
}

// 値を 0/1 にする（条件を値として使う）
static IrInst *ir_cond_value(Node *node) {
  IrBlock *t = ir_new_block(), *f = ir_new_block(), *end = ir_new_block();
  Obj *tmp = ir_alloc(sizeof(Obj)); // 値を合流させるための変数
  tmp->ty = ty_int;
  ir_cond(node, t, f);
  ir_seal(t);
  ir_seal(f);
  ir_cur = t;
  ir_write_var(tmp, ir_cur, ir_const(1));
  ir_jump(end);
  ir_cur = f;
  ir_write_var(tmp, ir_cur, ir_const(0));
  ir_jump(end);
  ir_seal(end);
  ir_cur = end;
  return ir_read_var(tmp, end);
}

// va_arg(ap, T): 引数の番地（2026-11-15。codegen.c の gen_va_arg と同じ手順。AAPCS64 の va_list:
// stack・gr_top・vr_top・gr_offs・vr_offs）。それまでは va_arg を使う関数（vfprintf など）は丸ごと
// 今までのコード生成だった
static IrInst *ir_va_arg(Node *node) {
  Type *ty = node->ty->base;
  ArgInfo ai = classify(ty);
  bool use_fp = ai.cls == AC_FP || ai.cls == AC_HFA;
  bool align16 = ai.cls != AC_REF && ty->align >= 16;
  int nreg = ai.cls == AC_REF ? 1 : ai.nreg;
  int offs_pos = use_fp ? VA_VR_OFFS : VA_GR_OFFS;
  int top_pos = use_fp ? VA_VR_TOP : VA_GR_TOP;
  int step = use_fp ? 16 : 8;

  IrInst *ap = ir_expr(node->m.lhs);
  IrBlock *regs = ir_new_block(), *regs2 = ir_new_block(), *stk = ir_new_block(), *end = ir_new_block();
  Obj *tmp = ir_alloc(sizeof(Obj));
  tmp->ty = ty_ulong;
  // レジスタ保存領域から取れるか（offs < 0）
  IrInst *offs_addr = ir_bin(IR_ADD, ap, ir_const(offs_pos));
  IrInst *offs = ir_load(offs_addr, ty_int);
  IrInst *c = ir_bin(IR_CMP, offs, ir_const(0));
  c->cond = CC_GE;
  ir_branch(c, stk, regs);
  ir_seal(regs);
  ir_cur = regs;
  if (align16 && !use_fp)
    offs = ir_normalize(ir_bin(IR_AND, ir_bin(IR_ADD, offs, ir_const(15)), ir_const(-16)), ty_int);
  IrInst *noffs = ir_normalize(ir_bin(IR_ADD, offs, ir_const(nreg * step)), ty_int);
  ir_store(offs_addr, noffs, ty_int);
  IrInst *c2 = ir_bin(IR_CMP, noffs, ir_const(0));
  c2->cond = CC_GT;
  ir_branch(c2, stk, regs2);
  ir_seal(regs2);
  ir_seal(stk);
  ir_cur = regs2;
  IrInst *top = ir_load(ir_bin(IR_ADD, ap, ir_const(top_pos)), ty_ulong);
  ir_write_var(tmp, ir_cur, ir_bin(IR_ADD, top, offs));
  ir_jump(end);
  // スタックから取る
  ir_cur = stk;
  IrInst *sp = ir_load(ir_bin(IR_ADD, ap, ir_const(VA_STACK)), ty_ulong);
  if (align16)
    sp = ir_bin(IR_AND, ir_bin(IR_ADD, sp, ir_const(15)), ir_const(-16));
  int64_t stk_sz = ai.cls == AC_REF ? 8 : align_to(ty->size, 8);
  ir_store(ir_bin(IR_ADD, ap, ir_const(VA_STACK)), ir_bin(IR_ADD, sp, ir_const(stk_sz)), ty_ulong);
  ir_write_var(tmp, ir_cur, sp);
  ir_jump(end);
  ir_seal(end);
  ir_cur = end;
  IrInst *addr = ir_read_var(tmp, end);
  if (ai.cls == AC_REF)
    addr = ir_load(addr, ty_ulong);
  return addr;
}

// __builtin_add・sub・mul_overflow（2026-11-15）: 2 つの入力と結果が同じ 32・64 ビットの整数のときだけ
// （ほかは今までのコード生成。_BitInt の実行時関数で計算する）。それまでは、これを使う picolibc の fwrite・
// strtoull などが丸ごと今までのコード生成で、計算も _BitInt の掛け算（遅い）だった
static bool ir_ckd_same(Type *a, Type *b) {
  return is_integer(a) && is_integer(b) && a->kind != TY_BOOL && b->kind != TY_BOOL &&
         a->kind != TY_BITINT && b->kind != TY_BITINT && a->size == b->size &&
         a->is_unsigned == b->is_unsigned && (a->size == 4 || a->size == 8);
}

static bool ir_ckd_ok(Node *node) {
  Type *rt = node->m.target->ty->base;
  if (!ir_ckd_same(node->m.lhs->ty, rt) || !ir_ckd_same(node->m.rhs->ty, rt))
    return false;
  int k = node->arith_kind;
  if (k != ND_ADD && k != ND_SUB && k != ND_MUL)
    return false;
  return !(k == ND_MUL && rt->size == 8 && !rt->is_unsigned);  // 64 ビットの符号つきの掛け算は除く
}

static IrInst *ir_cmp(IrInst *a, IrInst *b, int cond) {
  IrInst *c = ir_bin(IR_CMP, a, b);
  c->cond = cond;
  return c;
}

static IrInst *ir_ckd_arith(Node *node) {
  Type *rt = node->m.target->ty->base;
  IrInst *dst = ir_expr(node->m.target);
  IrInst *a = ir_expr(node->m.lhs);
  IrInst *b = ir_expr(node->m.rhs);
  int k = node->arith_kind;
  IrOp op = k == ND_ADD ? IR_ADD : k == ND_SUB ? IR_SUB : IR_MUL;
  IrInst *r = ir_bin(op, a, b);
  IrInst *ovf;
  if (rt->size == 4) {
    // 入力は 64 ビットに広げてあるので、64 ビットの結果は正確。32 ビットに収まらなければあふれ
    IrInst *n = ir_normalize(r, rt);
    ovf = ir_cmp(n, r, CC_NE);
    r = n;
  } else if (rt->is_unsigned) {
    if (k == ND_ADD) {
      ovf = ir_cmp(r, a, CC_LO);
    } else if (k == ND_SUB) {
      ovf = ir_cmp(a, b, CC_LO);
    } else {
      // a ≠ 0 かつ r / a ≠ b（a が 0 なら 1 で割る）
      IrInst *az = ir_cmp(a, ir_const(0), CC_EQ);
      IrInst *q = ir_bin(IR_UDIV, r, ir_bin(IR_ADD, a, az));
      ovf = ir_bin(IR_AND, ir_cmp(a, ir_const(0), CC_NE), ir_cmp(q, b, CC_NE));
    }
  } else {
    // 符号つき 64 ビットの足し算・引き算: 符号のビットで調べる
    IrInst *t = k == ND_ADD ? ir_bin(IR_AND, ir_bin(IR_XOR, a, r), ir_bin(IR_XOR, b, r))
                            : ir_bin(IR_AND, ir_bin(IR_XOR, a, b), ir_bin(IR_XOR, a, r));
    ovf = ir_cmp(t, ir_const(0), CC_LT);
  }
  ir_store(dst, r, rt);
  return ovf;
}

static IrInst *ir_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    if (ir_null_val)
      return ir_null_val;
    return ir_const(0);
  case ND_UNREACHABLE:
    ir_emit(ir_new(IR_UNREACH));
    return ir_const(0);
  case ND_CAS: {
    IrInst *in = ir_new(IR_CAS);
    in->a = ir_expr(node->cas.addr);
    in->b = ir_expr(node->cas.old_val);
    in->c = ir_expr(node->cas.new_val);
    in->size = node->cas.addr->ty->base->size;
    return ir_emit(in);
  }
  case ND_EXCH: {
    IrInst *in = ir_new(IR_XCHG);
    in->a = ir_expr(node->m.lhs);
    in->b = ir_expr(node->m.rhs);
    in->size = node->ty->size;
    in->sign = !node->ty->is_unsigned && node->ty->kind != TY_PTR;
    return ir_emit(in);
  }
  case ND_INIT_SEQ: {
    // 変数全体を 0 にしてから、各要素を代入する（今までのコード生成と同じ）
    IrInst *addr = ir_new(IR_LADDR);
    addr->imm = ir_slot(node->m.var);
    ir_emit(addr);
    if (ir_struct_ty(node->m.var->ty) && ir_word_struct(node->m.var->ty)) {
      for (int64_t k = 0; k < node->m.var->ty->size; k += 8)
        ir_store_word(addr, k, ir_const(0));
    } else {
      IrInst *z = ir_new(IR_MEMZERO);
      z->a = addr;
      z->imm = node->m.var->ty->size;
      ir_emit(z);
    }
    for (Node *n = node->m.lhs; n; n = n->next)
      ir_expr(n);
    return ir_const(0);
  }
  case ND_NUM:
    if (ir_fp_ty(node->ty)) {
      FPVal fval = {0};
      eval_fp(node, &fval);
      return ir_fconst(node->ty->size == 4 ? fval.chunk32[0] : fval.chunk[0], node->ty->size);
    }
    if (!ir_scalar_ty(node->ty))
      ir_unsupported();
    return ir_const(eval_sign_extend(node->ty, node->num.val));
  case ND_POS:
    return ir_expr(node->m.lhs);
  case ND_VAR:
    if (ir_is_ssa_node(node))
      return ir_read_var(node->m.var, ir_cur);
    return ir_load(ir_addr(node), node->ty);
  case ND_MEMBER:
    if (ir_is_bitfield(node))
      return ir_bitfield_load(node, ir_bitfield_unit_addr(node));
    return ir_tag_member(ir_load(ir_addr(node), node->ty), node);
  case ND_DEREF:
    return ir_load(ir_addr(node), node->ty);
  case ND_ADDR:
    return ir_addr(node->m.lhs);
  case ND_ASSIGN: {
    // 構造体・共用体・配列（局所変数の配列の初期化 char buf[N] = "..." など）は大きさの分を写す
    if (ir_struct_ty(node->m.lhs->ty) || node->m.lhs->ty->kind == TY_ARRAY) {
      IrInst *dst = ir_addr(node->m.lhs);
      IrInst *src = ir_expr(node->m.rhs);
      if (ir_struct_ty(node->m.lhs->ty) && ir_word_struct(node->m.lhs->ty))
        ir_copy_words(dst, src, node->m.lhs->ty->size);
      else
        ir_memcpy(dst, src, node->m.lhs->ty->size);
      return dst;
    }
    if (!ir_value_ty(node->m.lhs->ty))
      ir_unsupported();
    if (ir_is_ssa_node(node->m.lhs)) {
      IrInst *v = ir_expr(node->m.rhs);
      ir_write_var(node->m.lhs->m.var, ir_cur, v);
      return v;
    }
    if (ir_is_bitfield(node->m.lhs)) {
      IrInst *addr = ir_bitfield_unit_addr(node->m.lhs);
      return ir_bitfield_store(node->m.lhs, addr, ir_expr(node->m.rhs));
    }
    IrInst *addr = ir_addr(node->m.lhs);
    IrInst *v = ir_expr(node->m.rhs);
    ir_tag_member(ir_store(addr, v, node->m.lhs->ty), node->m.lhs);
    return v;
  }
  case ND_ARITH_ASSIGN: {
    if (!ir_value_ty(node->m.lhs->ty))
      ir_unsupported();
    IrInst *addr;
    IrInst *cur = ir_read_lvalue(node->m.lhs, &addr);
    IrInst *v = ir_update(node->arith_kind, node->m.lhs->ty, node->m.rhs, cur);
    if (ir_is_bitfield(node->m.lhs))
      return ir_bitfield_store(node->m.lhs, addr, v);
    ir_write_lvalue(node->m.lhs, addr, v);
    return v;
  }
  case ND_POST_INCDEC: {
    if (!ir_value_ty(node->m.lhs->ty))
      ir_unsupported();
    IrInst *addr;
    IrInst *cur = ir_read_lvalue(node->m.lhs, &addr);
    IrInst *v = ir_update(ND_ADD, node->m.lhs->ty, node->m.rhs, cur);
    ir_write_lvalue(node->m.lhs, addr, v);
    return cur;
  }
  case ND_COMMA:
  case ND_CHAIN:
    ir_expr(node->m.lhs);
    return ir_expr(node->m.rhs);
  case ND_CAST:
    return ir_cast(ir_expr(node->m.lhs), node->m.lhs->ty, node->ty);
  case ND_NEG:
    if (ir_fp_ty(node->ty)) {
      IrInst *in = ir_un(IR_FNEG, ir_expr(node->m.lhs));
      ir_set_class(in, node->ty);
      return in;
    }
    if (!ir_scalar_ty(node->ty))
      ir_unsupported();
    IrInst *neg = ir_un(IR_NEG, ir_expr(node->m.lhs));
    if (node->ty->size == 4) {
      if (ir_is_signed(node->ty))
        neg->nsw = true;
      else
        neg->wform = true;
    }
    return ir_normalize(neg, node->ty);
  case ND_BITNOT:
    if (!ir_scalar_ty(node->ty))
      ir_unsupported();
    IrInst *not = ir_un(IR_NOT, ir_expr(node->m.lhs));
    if (node->ty->size == 4 && !ir_is_signed(node->ty))
      not->wform = true;
    return ir_normalize(not, node->ty);
  case ND_NOT: {
    if (ir_fp_ty(node->m.lhs->ty))
      return ir_fcmp(ir_expr(node->m.lhs), ir_fzero(node->m.lhs->ty->size), CC_EQ);
    if (!ir_scalar_ty(node->m.lhs->ty))
      ir_unsupported();
    IrInst *in = ir_bin(IR_CMP, ir_expr(node->m.lhs), ir_const(0));
    in->cond = CC_EQ;
    return in;
  }
  case ND_LOGAND:
  case ND_LOGOR:
    return ir_cond_value(node);
  case ND_COND: {
    if (node->ty->kind != TY_VOID && !ir_value_ty(node->ty) && !ir_struct_ty(node->ty))
      ir_unsupported();
    int64_t val;
    if (is_const_expr(node->ctrl.cond, &val))
      return ir_expr(val ? node->ctrl.then : node->ctrl.els);
    IrBlock *t = ir_new_block(), *f = ir_new_block(), *end = ir_new_block();
    Obj *tmp = ir_alloc(sizeof(Obj));
    tmp->ty = node->ty;
    ir_cond(node->ctrl.cond, t, f);
    ir_seal(t);
    ir_seal(f);
    ir_cur = t;
    IrInst *tv = ir_expr(node->ctrl.then);
    ir_write_var(tmp, ir_cur, tv);
    ir_jump(end);
    ir_cur = f;
    IrInst *fv = ir_expr(node->ctrl.els);
    ir_write_var(tmp, ir_cur, fv);
    ir_jump(end);
    ir_seal(end);
    ir_cur = end;
    if (node->ty->kind == TY_VOID)
      return ir_const(0);
    return ir_read_var(tmp, end);
  }
  case ND_FUNCALL:
    return ir_funcall(node);
  case ND_VA_ARG:
    return ir_va_arg(node);
  case ND_LABEL_VAL: {
    IrInst *in = ir_new(IR_LABELADDR);
    in->imm = node->jmp.target->lbl.id;
    in->size = 8;
    return ir_emit(in);
  }
  case ND_ALLOCA: {
    IrInst *in = ir_new(IR_ALLOCA);
    in->a = ir_cast(ir_expr(node->m.lhs), node->m.lhs->ty, ty_ulong);
    in->size = 8;
    ir_emit(in);
    return in;
  }
  case ND_CKD_ARITH:
    return ir_ckd_arith(node);
  case ND_VA_COPY:
    ir_memcpy(ir_expr(node->m.lhs), ir_expr(node->m.rhs), 32);
    return ir_const(0);
  case ND_STMT_EXPR: {
    for (Node *n = node->blk.body; n != node->blk.result; n = n->next)
      ir_stmt(n);
    if (node->blk.result) {
      Node *r = node->blk.result->m.lhs;
      if (r->ty->kind != TY_VOID && !ir_value_ty(r->ty) && !ir_struct_ty(r->ty))
        ir_unsupported();
      return ir_expr(r);
    }
    return ir_const(0);
  }
  default:
    if (ir_is_binary(node->kind)) {
      Type *opty = node->m.lhs->ty;
      if (!ir_value_ty(opty) || !ir_value_ty(node->m.rhs->ty) || !ir_value_ty(node->ty))
        ir_unsupported();
      IrInst *fused = ir_fp_contract(node);
      if (fused)
        return fused;
      IrInst *l = ir_expr(node->m.lhs);
      IrInst *r = ir_expr(node->m.rhs);
      return ir_binary(node->kind, l, r, opty, node->ty);
    }
    ir_unsupported();
  }
}

// 条件分岐（比較・論理演算は、値を作らずに分岐にする）
static void ir_cond(Node *node, IrBlock *t, IrBlock *f) {
  int64_t val;
  if (is_const_expr(node, &val)) {
    ir_jump(val ? t : f); // 通らない側は到達しないので出力されない
    return;
  }
  switch (node->kind) {
  case ND_LOGAND: {
    IrBlock *mid = ir_new_block();
    ir_cond(node->m.lhs, mid, f);
    ir_seal(mid);
    ir_cur = mid;
    ir_cond(node->m.rhs, t, f);
    return;
  }
  case ND_LOGOR: {
    IrBlock *mid = ir_new_block();
    ir_cond(node->m.lhs, t, mid);
    ir_seal(mid);
    ir_cur = mid;
    ir_cond(node->m.rhs, t, f);
    return;
  }
  case ND_NOT:
    ir_cond(node->m.lhs, f, t);
    return;
  default:
    break;
  }
  if (ir_fp_ty(node->ty)) {
    ir_branch(ir_fcmp(ir_expr(node), ir_fzero(node->ty->size), CC_NE), t, f);
    return;
  }
  if (!ir_scalar_ty(node->ty))
    ir_unsupported();
  ir_branch(ir_expr(node), t, f);
}

static IrLoop *ir_find_loop(Node *target) {
  for (int i = ir_nloops - 1; i >= 0; i--)
    if (ir_loops[i].node == target)
      return &ir_loops[i];
  ir_unsupported();
}

static void ir_push_loop(Node *node, IrBlock *brk, IrBlock *cont) {
  ir_loops = ir_grow(ir_loops, &ir_caploops, ir_nloops + 1, sizeof(IrLoop));
  ir_loops[ir_nloops++] = (IrLoop){node, brk, cont};
}

// インラインアセンブリ: オペランドに x0〜x7・v0〜v7 を順に割り当てる（値の割り当てには使わないレジスタ）。
// 本文が壊してよいのは、それらと x16・x17 だけ（ほかを壊すものは、今までのコード生成に任せる）
static void ir_asm(Node *node) {
  if (node->gasm.labels)
    ir_unsupported();
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
      if (!(r <= 7 || r == 16 || r == 17 || (r >= ASM_FP_BASE && r <= ASM_FP_BASE + 7)))
        ir_unsupported();
    }
  }
  IrInst *in = ir_new(IR_ASM);
  in->asm_node = node;
  in->argregs = ir_alloc(sizeof(int) * 32);
  int ngp = 0, nfp = 0;
  for (int k = 0; k < 2; k++) {
    for (AsmParam *ap = k ? node->gasm.inputs : node->gasm.outputs; ap; ap = ap->next) {
      if (ap->kind == ASMOP_NUM && k == 1)
        continue;
      if (ap->kind == ASMOP_REG && ap->arg->kind == ND_VAR && ap->arg->m.var->asm_name)
        ir_unsupported(); // レジスタ変数
      if (k == 1 && ap->match) {
        ap->reg = ap->match->reg;
      } else if (ap->kind == ASMOP_REG && ap->is_fp_reg) {
        if (nfp == 8)
          ir_unsupported();
        ap->reg = ASM_FP_BASE + nfp++;
      } else if (ap->kind == ASMOP_REG || ap->kind == ASMOP_MEM) {
        if (ngp == 8)
          ir_unsupported();
        ap->reg = ngp++;
      } else {
        ir_unsupported();
      }
      IrInst *v;
      if (ap->kind == ASMOP_MEM) {
        v = ir_addr(ap->arg); // 出力でも、場所のアドレスを渡す
      } else if (k == 0) {
        if (!ir_value_ty(ap->arg->ty) || ir_is_bitfield(ap->arg))
          ir_unsupported();
        if (!ap->is_inout)
          continue; // 出力だけ: 本文の後で受け取る
        IrInst *addr;
        v = ir_read_lvalue(ap->arg, &addr);
      } else {
        Type *ty = ap->var ? ap->var->ty : ap->arg->ty;
        if (!ir_value_ty(ty) || (ap->is_fp_reg && !ir_fp_ty(ty)) || (!ap->is_fp_reg && ir_fp_ty(ty)))
          ir_unsupported();
        v = ir_cast(ir_expr(ap->arg), ap->arg->ty, ty);
      }
      in->argregs[in->nargs] = ap->reg;
      ir_add_arg(in, v);
    }
  }
  ir_emit(in);
  // 出力を、x0〜x7 を壊しうる命令（TLS のアドレスなど）より前に、すべて受け取る
  IrInst *outs[16];
  int nout = 0;
  for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next) {
    if (ap->kind != ASMOP_REG)
      continue;
    IrInst *o = ir_new(IR_ASMOUT);
    o->imm = ap->reg;
    ir_set_class(o, ap->arg->ty);
    if (o->fp != (ap->reg >= ASM_FP_BASE))
      ir_unsupported();
    outs[nout++] = ir_emit(o);
  }
  nout = 0;
  for (AsmParam *ap = node->gasm.outputs; ap; ap = ap->next) {
    if (ap->kind != ASMOP_REG)
      continue;
    IrInst *v = ir_normalize(outs[nout++], ap->arg->ty);
    IrInst *addr = ir_is_ssa_node(ap->arg) ? NULL : ir_addr(ap->arg);
    ir_write_lvalue(ap->arg, addr, v);
  }
}

static void ir_add_succ(IrBlock *bb, IrBlock *s) {
  if (bb->succ == bb->succ_buf) {
    IrBlock **n = ir_grow(NULL, &bb->capsucc, bb->nsucc + 1, sizeof(IrBlock *));
    memcpy(n, bb->succ_buf, sizeof(IrBlock *) * bb->nsucc);
    bb->succ = n;
  } else {
    bb->succ = ir_grow(bb->succ, &bb->capsucc, bb->nsucc + 1, sizeof(IrBlock *));
  }
  bb->succ[bb->nsucc++] = s;
  ir_add_pred(s, bb);
}

static IrBlock *ir_label_block(Node *label);

// 振り分けのブロックを仕上げる（関数の終わり、ラベルを封じる前）: goto *p の番地で IR_INDBR し、後続は、この関数で
// 番号を付けたラベルごとの入口のブロック（.L.jmp.N を出してラベルのブロックへ飛ぶ）
static void ir_finish_dispatch(void) {
  if (!ir_dispatch)
    ir_unsupported();   // &&label だけで goto *p がない（入口のブロックに届かない。今までのコード生成で）
  IrBlock *saved = ir_cur;
  ir_seal(ir_dispatch);
  ir_cur = ir_dispatch;
  IrInst *br = ir_new(IR_INDBR);
  br->a = ir_read_var(ir_dispatch_var, ir_dispatch);
  ir_append(ir_dispatch, br);
  int n = ir_nlabels;
  for (int i = 0; i < n; i++) {
    Node *lb = ir_labels[i].label;
    if (lb->kind != ND_LABEL || lb->lbl.id < ir_lbl_lo || lb->lbl.id >= ir_lbl_hi)
      continue;
    if (!ir_labels[i].defined)
      ir_unsupported();
    IrBlock *t = ir_new_block();
    t->jmp_id = lb->lbl.id;
    ir_add_succ(ir_dispatch, t);
    t->sealed = true;
    ir_cur = t;
    ir_jump(ir_labels[i].bb);
  }
  ir_cur = saved;
}

static IrBlock *ir_label_block(Node *label) {
  int base = ir_ninlines ? ir_inlines[ir_ninlines - 1].label_base : 0;
  for (int i = base; i < ir_nlabels; i++)
    if (ir_labels[i].label == label)
      return ir_labels[i].bb;
  ir_labels = ir_grow(ir_labels, &ir_caplabels, ir_nlabels + 1, sizeof(IrLabel));
  IrBlock *bb = ir_new_block();
  ir_labels[ir_nlabels++] = (IrLabel){label, bb, false};
  return bb;
}

typedef struct {
  int64_t lo, hi;
  IrBlock *target;
} IrCase;

TLS static bool ir_case_unsigned;

static int ir_case_cmp(const void *x, const void *y) {
  const IrCase *a = x, *b = y;
  if (ir_case_unsigned)
    return (uint64_t)a->lo < (uint64_t)b->lo ? -1 : (uint64_t)a->lo > (uint64_t)b->lo;
  return a->lo < b->lo ? -1 : a->lo > b->lo;
}

// case の範囲 cs[0..n) を、今のブロックから振り分ける。どれにも合わなければ def へ。
// 多いときは二分探索（真ん中の範囲の下端より小さいかで左右に分ける）、少なければ順に比べる
// （2026-11-16。前はいつも順に比べていて、case が 50 個なら平均 25 回比べていた。GCC は表引きか二分探索）
#define IR_SWITCH_LINEAR 4

static void ir_switch_tree(IrInst *v, IrCase *cs, int n, IrBlock *def) {
  if (n > IR_SWITCH_LINEAR) {
    int mid = n / 2;
    IrBlock *left = ir_new_block(), *right = ir_new_block();
    IrInst *c = ir_bin(IR_CMP, v, ir_const(cs[mid].lo));
    c->cond = ir_case_unsigned ? CC_LO : CC_LT;
    ir_branch(c, left, right);
    ir_seal(left);
    ir_seal(right);
    ir_cur = left;
    ir_switch_tree(v, cs, mid, def);
    ir_cur = right;
    ir_switch_tree(v, cs + mid, n - mid, def);
    return;
  }
  // 振り分け: case の範囲ごとに比べる（範囲は (v - lo) を符号なしで hi - lo 以下か）
  for (int i = 0; i < n; i++) {
    IrBlock *next = ir_new_block();
    int64_t lo = cs[i].lo, hi = cs[i].hi;
    IrInst *c;
    if (lo == hi) {
      c = ir_bin(IR_CMP, v, ir_const(lo));
      c->cond = CC_EQ;
    } else {
      c = ir_bin(IR_CMP, ir_bin(IR_SUB, v, ir_const(lo)), ir_const((int64_t)((uint64_t)hi - (uint64_t)lo)));
      c->cond = CC_LS;
    }
    ir_branch(c, cs[i].target, next);
    ir_seal(next);
    ir_cur = next;
  }
  ir_jump(def);
}

// 表引き（2026-11-27）: case の範囲が IR_SWTAB_MIN 個以上で、下端から上端までの値の数が、case の値の数の
// IR_SWTAB_DENSITY 倍まで（で IR_SWTAB_MAX まで）なら、(v - 下端) を符号なしで幅と比べて外れたら def へ、
// 中なら表で飛ぶ。二分探索では、case が 50 個なら比べと分岐が 6 回ほど（GCC も密なら表引き）
#define IR_SWTAB_MIN 6
#define IR_SWTAB_DENSITY 3
#define IR_SWTAB_MAX 4096

static bool ir_switch_table(IrInst *v, IrCase *cs, int n, IrBlock *def) {
  if (n < IR_SWTAB_MIN)
    return false;
  uint64_t lo = (uint64_t)cs[0].lo, range = (uint64_t)cs[n - 1].hi - lo;   // cs は下端の順
  uint64_t nvals = 0;
  for (int i = 0; i < n; i++)
    nvals += (uint64_t)cs[i].hi - (uint64_t)cs[i].lo + 1;
  if (range >= IR_SWTAB_MAX || range + 1 > IR_SWTAB_DENSITY * nvals)
    return false;
  IrInst *idx = lo ? ir_bin(IR_SUB, v, ir_const((int64_t)lo)) : v;
  IrInst *c = ir_bin(IR_CMP, idx, ir_const((int64_t)range));
  c->cond = CC_HI;
  IrBlock *d = ir_new_block();
  ir_branch(c, def, d);
  ir_seal(d);
  ir_cur = d;
  IrInst *br = ir_new(IR_SWTAB);
  br->a = idx;
  br->nswtab = (int)range + 1;
  br->swtab = ir_alloc(sizeof(int) * br->nswtab);
  ir_append(d, br);
  // 行き先ごとの入口のブロック（同じ行き先は 1 つ。最初の 1 つは def）
  IrBlock **targets = ir_alloc(sizeof(IrBlock *) * (n + 1));
  int nt = 0;
  targets[nt++] = def;
  for (int i = 0; i < br->nswtab; i++)
    br->swtab[i] = 0;
  for (int i = 0; i < n; i++) {
    int k = 0;
    while (k < nt && targets[k] != cs[i].target)
      k++;
    if (k == nt)
      targets[nt++] = cs[i].target;
    for (uint64_t x = (uint64_t)cs[i].lo - lo; x <= (uint64_t)cs[i].hi - lo; x++)
      br->swtab[x] = k;
  }
  for (int k = 0; k < nt; k++) {
    IrBlock *e = ir_new_block();
    ir_add_succ(d, e);
    e->sealed = true;
    ir_cur = e;
    ir_jump(targets[k]);
  }
  return true;
}

static void ir_switch(Node *node) {
  Type *ty = node->ctrl.cond->ty;
  if (!ir_scalar_ty(ty))
    ir_unsupported();
  IrInst *v = ir_expr(node->ctrl.cond);
  IrBlock *brk = ir_new_block();
  int n = 0;
  for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next)
    n++;
  IrCase *cs = ir_alloc(sizeof(IrCase) * (n + 1));
  n = 0;
  for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next) {
    if (cr->label == node->ctrl.sw_default)
      continue;
    cs[n++] = (IrCase){eval_sign_extend(ty, cr->lo), eval_sign_extend(ty, cr->hi), ir_label_block(cr->label)};
  }
  IrBlock *def = node->ctrl.sw_default ? ir_label_block(node->ctrl.sw_default) : brk;
  if (n > IR_SWITCH_LINEAR) {
    // 範囲は重ならない（同じ値の case はコンパイルのエラー）。下端の順に並べる
    ir_case_unsigned = ty->is_unsigned || ty->kind == TY_PTR;
    qsort(cs, n, sizeof(IrCase), ir_case_cmp);
  }
  if (!ir_switch_table(v, cs, n, def))
    ir_switch_tree(v, cs, n, def);
  ir_cur = ir_new_block(); // 本体の先頭（最初の case の前）には来ない
  ir_cur->sealed = true;
  ir_push_loop(node, brk, NULL);
  ir_stmt(node->ctrl.then);
  ir_nloops--;
  ir_jump(brk);
  ir_seal(brk);
  ir_cur = brk;
}

// ループの条件の式を 2 か所に出してよいか（ラベル・文の式・宣言を含まない、小さな式）。*n は節点の数。
// メモリを読む条件（while (*s) など）も出さない: 反転すると、本体の中の同じ読み出しを条件で読んだ値で
// 置き換えられなくなり（前と後ろの 2 か所から来る）、1 回ごとの読み出しが増える
static bool ir_dup_ok(Node *n, int *count) {
  if (!n)
    return true;
  if (++*count > 40 || n->dfr_from || n->dfr_dest)
    return false;
  switch (n->kind) {
  case ND_NUM: case ND_VAR:
    return true;
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_REM:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR: case ND_SAR:
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
  case ND_ASSIGN: case ND_ARITH_ASSIGN: case ND_POST_INCDEC: case ND_COMMA:
  case ND_LOGAND: case ND_LOGOR:
    return ir_dup_ok(n->m.lhs, count) && ir_dup_ok(n->m.rhs, count);
  case ND_POS: case ND_NEG: case ND_BITNOT: case ND_NOT: case ND_CAST: case ND_ADDR:
    return ir_dup_ok(n->m.lhs, count);
  case ND_COND:
    return ir_dup_ok(n->ctrl.cond, count) && ir_dup_ok(n->ctrl.then, count) &&
           ir_dup_ok(n->ctrl.els, count);
  default:
    return false;
  }
}

// ループの展開（2 回分。2026-11-15）: for (...; V op E; V += s) の形（V は SSA の符号つきの整数、
// s は 0 でない定数、op は s > 0 なら < か <=（E op V なら > か >=）、s < 0 なら逆）で、本体が V・E を
// 書き換えず、2 か所に出してよい小さなもののとき。
//   前: V op E でなければ抜ける → 次の分（V + s）op E なら本体・V += s・本体・V += s を回し、
//   回すたびに後ろで（V + s）op E を調べる。成り立たなくなったら、V op E なら本体を 1 回して抜ける。
// 符号つきなので V + s があふれるのは元のプログラムでも未定義動作。それ以外では（V + s）op E が
// 成り立てば V op E も成り立つ（s と op の向きが同じ）ので、本体を実行する回数は元と同じ。
// 符号なしはあふれが決まっている（回り込む）ので展開しない。
typedef struct {
  Obj *var;       // V
  Obj *bound;     // E が変数ならその変数（定数なら NULL）
  int64_t step;   // s
} IrUnroll;

static Node *ir_strip_cast(Node *n) {
  while (n && n->kind == ND_CAST && is_integer(n->ty))
    n = n->m.lhs;
  return n;
}

static bool ir_const_node(Node *n, int64_t *v) {
  n = ir_strip_cast(n);
  if (!n || n->kind != ND_NUM || !is_integer(n->ty))
    return false;
  *v = n->num.val;
  return true;
}

// 書き換えられる変数（代入・複合代入・増減の左辺）が var か
static bool ir_writes_var(Node *lhs, Obj *var) {
  lhs = ir_strip_cast(lhs);
  return var && lhs && lhs->kind == ND_VAR && lhs->m.var == var;
}

// 本体を 2 か所に出してよいか（V・E を書き換えない、ラベル・switch・asm・ループなどがない）。*count は
// 節点の数。中にループがあるもの（いちばん内側でないループ）は展開しない（本体ごと 3 つになってコードが
// 膨れ、外側の分岐が減るだけ）
static bool ir_unroll_body_ok(Node *n, IrUnroll *u, int *count) {
  if (!n)
    return true;
  if (++*count > 40 || n->dfr_from || n->dfr_dest)
    return false;
  switch (n->kind) {
  case ND_NUM: case ND_VAR: case ND_NULL_EXPR: case ND_NULL_STMT: case ND_BREAK: case ND_CONT:
    return true;
  case ND_ASSIGN: case ND_ARITH_ASSIGN: case ND_POST_INCDEC:
    if (ir_writes_var(n->m.lhs, u->var) || ir_writes_var(n->m.lhs, u->bound))
      return false;
    // fallthrough
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_REM:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR: case ND_SAR:
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_GT: case ND_GE:
  case ND_COMMA: case ND_LOGAND: case ND_LOGOR: case ND_CHAIN:
    return ir_unroll_body_ok(n->m.lhs, u, count) && ir_unroll_body_ok(n->m.rhs, u, count);
  case ND_POS: case ND_NEG: case ND_BITNOT: case ND_NOT: case ND_CAST: case ND_DEREF:
  case ND_MEMBER: case ND_ADDR: case ND_EXPR_STMT: case ND_RETURN:
    return ir_unroll_body_ok(n->m.lhs, u, count);
  case ND_COND: case ND_IF:
    return ir_unroll_body_ok(n->ctrl.cond, u, count) && ir_unroll_body_ok(n->ctrl.then, u, count) &&
           ir_unroll_body_ok(n->ctrl.els, u, count);
  case ND_BLOCK:
    if (n->blk.local_labels)
      return false;
    for (Node *c = n->blk.body; c; c = c->next)
      if (!ir_unroll_body_ok(c, u, count))
        return false;
    return true;
  case ND_FUNCALL:
    if (n->call.alloca_args || !ir_unroll_body_ok(n->call.expr, u, count))
      return false;
    for (Obj *a = n->call.args; a; a = a->param_next)
      if (!ir_unroll_body_ok(a->arg_expr, u, count))
        return false;
    return true;
  default:
    return false;  // ラベル・case・switch・asm・goto・文の式・可変長配列など
  }
}

static bool ir_unroll_ok(Node *node, IrUnroll *u) {
  Node *cond = node->ctrl.cond, *inc = node->ctrl.for_inc;
  if (!cond || !inc || !node->ctrl.then)
    return false;
  // 値を使わない式（void への変換）、後置の増減を「足してから戻す」にした形（parse.c の new_inc_dec）
  while (inc->kind == ND_CAST)
    inc = inc->m.lhs;
  if (inc->kind == ND_ADD && inc->m.rhs && inc->m.rhs->kind == ND_NUM)
    inc = ir_strip_cast(inc->m.lhs);
  // 増やす式: V++・V--・V += c・V -= c
  Node *lv = ir_strip_cast(inc->m.lhs);
  if (!lv || lv->kind != ND_VAR)
    return false;
  Obj *var = lv->m.var;
  if (!ir_is_ssa_var(var) || !is_integer(var->ty) || var->ty->kind == TY_BOOL || !ir_is_signed(var->ty))
    return false;
  int64_t c;
  if (inc->kind == ND_POST_INCDEC && ir_const_node(inc->m.rhs, &c))
    u->step = c;
  else if (inc->kind == ND_ARITH_ASSIGN && (inc->arith_kind == ND_ADD || inc->arith_kind == ND_SUB) &&
           ir_const_node(inc->m.rhs, &c))
    u->step = inc->arith_kind == ND_ADD ? c : -c;
  else
    return false;
  if (u->step == 0 || u->step > (1 << 20) || u->step < -(1 << 20))
    return false;
  // 条件: V op E か E op V
  int k = cond->kind;
  if (k != ND_LT && k != ND_LE && k != ND_GT && k != ND_GE)
    return false;
  Node *l = ir_strip_cast(cond->m.lhs), *r = ir_strip_cast(cond->m.rhs);
  Node *e;
  bool up;  // V が増えると、いつか成り立たなくなる向き
  if (l && l->kind == ND_VAR && l->m.var == var) {
    e = r;
    up = k == ND_LT || k == ND_LE;
  } else if (r && r->kind == ND_VAR && r->m.var == var) {
    e = l;
    up = k == ND_GT || k == ND_GE;
  } else {
    return false;
  }
  if (up != (u->step > 0))
    return false;
  // 比べる型も符号つき（符号なしに変換して比べると、向きが変わりうる）
  if (!ir_is_signed(cond->m.lhs->ty))
    return false;
  u->var = var;
  u->bound = NULL;
  if (!e)
    return false;
  if (e->kind == ND_VAR) {
    if (!ir_is_ssa_var(e->m.var) || e->m.var == var)
      return false;
    u->bound = e->m.var;
  } else if (e->kind != ND_NUM) {
    return false;
  }
  int count = 0;
  return ir_unroll_body_ok(node->ctrl.then, u, &count);
}

// 本体を 1 つ出す（continue は cont へ。cont で V を増やして next へ）
static void ir_unroll_copy(Node *node, IrBlock *brk, IrBlock *next) {
  IrBlock *cont = ir_new_block();
  ir_push_loop(node, brk, cont);
  ir_stmt(node->ctrl.then);
  ir_nloops--;
  ir_jump(cont);
  ir_seal(cont);
  ir_cur = cont;
  ir_expr(node->ctrl.for_inc);
  ir_jump(next);
}

// （V + s）op E の値（V の値は変えない）
static IrInst *ir_unroll_next_cond(Node *node, IrUnroll *u) {
  IrInst *v = ir_read_var(u->var, ir_cur);
  IrInst *v2 = ir_normalize(ir_bin(IR_ADD, v, ir_const(u->step)), u->var->ty);
  ir_write_var(u->var, ir_cur, v2);
  IrInst *c = ir_expr(node->ctrl.cond);
  ir_write_var(u->var, ir_cur, v);
  return c;
}

static void ir_unroll_for(Node *node, IrUnroll *u) {
  IrBlock *chk = ir_new_block(), *loop = ir_new_block(), *rem = ir_new_block();
  IrBlock *brk = ir_new_block();
  ir_cond(node->ctrl.cond, chk, brk);
  int v0 = ir_nvals;
  ir_seal(chk);
  ir_cur = chk;
  ir_branch(ir_unroll_next_cond(node, u), loop, rem);
  // 2 回分
  ir_cur = loop;
  IrBlock *mid = ir_new_block(), *tail = ir_new_block();
  int c1 = ir_nvals;
  ir_unroll_copy(node, brk, mid);
  int c1e = ir_nvals;
  ir_seal(mid);
  ir_cur = mid;
  ir_unroll_copy(node, brk, tail);
  ir_seal(tail);
  ir_cur = tail;
  ir_branch(ir_unroll_next_cond(node, u), loop, rem);
  ir_seal(loop);
  // 残りの 1 回
  ir_seal(rem);
  ir_cur = rem;
  IrBlock *last = ir_new_block();
  ir_cond(node->ctrl.cond, last, brk);
  ir_seal(last);
  ir_cur = last;
  ir_unroll_copy(node, brk, brk);
  ir_seal(brk);
  ir_cur = brk;
  // 最初の条件と 1 つ目の本体のほかは、2 か所目以降に出した分
  ir_dup_range(v0, c1);
  ir_dup_range(c1e, ir_nvals);
}

static void ir_stmt(Node *node) {
  if (node->dfr_from || node->dfr_dest)
    ir_unsupported();
  switch (node->kind) {
  case ND_NULL_STMT:
    return;
  case ND_BLOCK:
    for (Node *n = node->blk.body; n; n = n->next)
      ir_stmt(n);
    return;
  case ND_EXPR_STMT:
    ir_expr(node->m.lhs);
    return;
  case ND_RETURN:
    if (ir_ninlines) {
      // 展開した関数の return: 結果の変数に書いて出口へ
      IrInline *ctx = &ir_inlines[ir_ninlines - 1];
      if (node->m.lhs && ir_struct_ty(ctx->fn->ty->return_ty)) {
        ir_copy_struct(ir_laddr(ctx->rtn_ofs), ir_expr(node->m.lhs), ctx->fn->ty->return_ty->size);
      } else if (node->m.lhs) {
        IrInst *v = ir_cast(ir_expr(node->m.lhs), node->m.lhs->ty, ctx->fn->ty->return_ty);
        ir_write_var(ctx->result, ir_cur, v);
      }
      ir_jump(ctx->exit);
      return;
    }
    {
    IrInst *in = ir_new(IR_RET);
    if (node->m.lhs) {
      Type *rty = ir_fn->ty->return_ty;
      if (ir_struct_ty(rty)) {
        IrInst *src = ir_expr(node->m.lhs);
        if (ir_rtn_ptr) {
          // x8 の領域にコピーし、そのアドレスを返す（今までのコード生成と同じ）
          ir_memcpy(ir_rtn_ptr, src, rty->size);
          in->a = ir_rtn_ptr;
        } else if (ir_word_reg_struct(rty)) {
          // 語に分けて x0・x1 で返す（SROA。2026-11-21。args に語）
          for (int64_t k = 0; k < rty->size; k += 8)
            ir_add_arg(in, ir_load_word(src, k));
        } else {
          ArgInfo ai = classify(rty);
          if (ai.cls != AC_GP && ai.cls != AC_HFA)
            ir_unsupported();
          in->a = src; // アドレスから x0・x1（HFA なら v0〜v3）に読み込んで返す
          in->ty = rty;
        }
      } else {
        if (!ir_value_ty(rty))
          ir_unsupported();
        in->a = ir_cast(ir_expr(node->m.lhs), node->m.lhs->ty, rty);
      }
    }
    ir_emit(in);
    return;
    }
  case ND_IF: {
    IrBlock *t = ir_new_block(), *f = ir_new_block(), *end = ir_new_block();
    ir_cond(node->ctrl.cond, t, f);
    ir_seal(t);
    ir_seal(f);
    ir_cur = t;
    ir_stmt(node->ctrl.then);
    ir_jump(end);
    ir_cur = f;
    if (node->ctrl.els)
      ir_stmt(node->ctrl.els);
    ir_jump(end);
    ir_seal(end);
    ir_cur = end;
    return;
  }
  case ND_FOR: {
    if (node->ctrl.for_init)
      ir_stmt(node->ctrl.for_init);
    IrUnroll un;
    if (opt_unroll && ir_unroll_ok(node, &un) && ir_dup_ok(node->ctrl.cond, &(int){0})) {
      ir_unroll_for(node, &un);
      return;
    }
    if (node->ctrl.cond && ir_dup_ok(node->ctrl.cond, &(int){0})) {
      // ループの反転（2026-11-15）: 条件を前で 1 回、本体の後ろでもう 1 回調べる。1 回まわるごとの
      // 分岐が 1 つになる（実機で配列の和のループが 25% 速くなった）。条件の式は 2 か所に出す
      IrBlock *body = ir_new_block(), *cont = ir_new_block(), *brk = ir_new_block();
      ir_cond(node->ctrl.cond, body, brk);
      ir_cur = body;
      ir_push_loop(node, brk, cont);
      ir_stmt(node->ctrl.then);
      ir_nloops--;
      ir_jump(cont);
      ir_seal(cont);
      ir_cur = cont;
      if (node->ctrl.for_inc)
        ir_expr(node->ctrl.for_inc);
      int v0 = ir_nvals;
      ir_cond(node->ctrl.cond, body, brk);
      ir_dup_range(v0, ir_nvals);
      ir_seal(body);
      ir_seal(brk);
      ir_cur = brk;
      return;
    }
    IrBlock *head = ir_new_block(), *body = ir_new_block();
    IrBlock *cont = ir_new_block(), *brk = ir_new_block();
    ir_jump(head);
    ir_cur = head;
    if (node->ctrl.cond)
      ir_cond(node->ctrl.cond, body, brk);
    else
      ir_jump(body);
    ir_seal(body);
    ir_cur = body;
    ir_push_loop(node, brk, cont);
    ir_stmt(node->ctrl.then);
    ir_nloops--;
    ir_jump(cont);
    ir_seal(cont);
    ir_cur = cont;
    if (node->ctrl.for_inc)
      ir_expr(node->ctrl.for_inc);
    ir_jump(head);
    ir_seal(head);
    ir_seal(brk);
    ir_cur = brk;
    return;
  }
  case ND_DO: {
    IrBlock *body = ir_new_block(), *cont = ir_new_block(), *brk = ir_new_block();
    ir_jump(body);
    ir_cur = body;
    ir_push_loop(node, brk, cont);
    ir_stmt(node->ctrl.then);
    ir_nloops--;
    ir_jump(cont);
    ir_seal(cont);
    ir_cur = cont;
    ir_cond(node->ctrl.cond, body, brk);
    ir_seal(body);
    ir_seal(brk);
    ir_cur = brk;
    return;
  }
  case ND_BREAK:
    ir_jump(ir_find_loop(node->jmp.parent_loop)->brk);
    return;
  case ND_SWITCH:
    ir_switch(node);
    return;
  case ND_ASM:
    ir_asm(node);
    return;
  case ND_CASE:
  case ND_DEFAULT: {
    // 前の文から落ちてくる流れと、振り分けからの分岐。どちらもここまでに出そろっているので封じる
    IrBlock *bb = ir_label_block(node);
    ir_jump(bb);
    ir_seal(bb);
    ir_cur = bb;
    return;
  }
  case ND_LABEL: {
    // goto は後からも来るので、関数の終わりで封じる
    IrBlock *bb = ir_label_block(node);
    for (int i = 0; i < ir_nlabels; i++)
      if (ir_labels[i].label == node)
        ir_labels[i].defined = true;
    ir_jump(bb);
    ir_cur = bb;
    return;
  }
  case ND_GOTO:
    ir_jump(ir_label_block(node->jmp.target));
    return;
  case ND_GOTO_EXPR: {
    // 振り分けのブロックへ（番地は ir_dispatch_var で合流させる。ir_finish_dispatch）
    IrInst *v = ir_expr(node->m.lhs);
    if (ir_is_term(ir_cur->last))
      return;
    if (!ir_dispatch) {
      ir_dispatch = ir_new_block();
      ir_dispatch_var = ir_alloc(sizeof(Obj));
      ir_dispatch_var->ty = ty_ulong;
    }
    ir_write_var(ir_dispatch_var, ir_cur, v);
    ir_jump(ir_dispatch);
    return;
  }
  case ND_CONT: {
    IrLoop *l = ir_find_loop(node->jmp.parent_loop);
    if (!l->cont)
      ir_unsupported();
    ir_jump(l->cont);
    return;
  }
  default:
    ir_unsupported();
  }
}

//
// 片付け: 到達しないブロックを除き、自明な phi を置き換える
//

static void ir_mark_reachable(IrBlock *bb) {
  if (bb->reachable)
    return;
  bb->reachable = true;
  for (int i = 0; i < bb->nsucc; i++)
    ir_mark_reachable(bb->succ[i]);
}

static void ir_remove(IrInst *in) {
  IrBlock *bb = in->bb;
  if (in->prev)
    in->prev->next = in->next;
  else
    bb->first = in->next;
  if (in->next)
    in->next->prev = in->prev;
  else
    bb->last = in->prev;
}

static void ir_resolve_operands(IrInst *in) {
  in->a = ir_resolve(in->a);
  in->b = ir_resolve(in->b);
  for (int i = 0; i < in->nargs; i++)
    in->args[i] = ir_resolve(in->args[i]);
}

static void ir_cleanup(void) {
  ir_mark_reachable(ir_blocks[0]);
  // 到達しない前任を、phi の引数と一緒に除く
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable)
      continue;
    int k = 0;
    for (int i = 0; i < bb->npreds; i++) {
      if (!bb->preds[i]->reachable)
        continue;
      for (IrInst *in = bb->first; in; in = in->next)
        if (in->op == IR_PHI && in->nargs == bb->npreds)
          in->args[k] = in->args[i];
      bb->preds[k++] = bb->preds[i];
    }
    for (IrInst *in = bb->first; in; in = in->next)
      if (in->op == IR_PHI && in->nargs == bb->npreds)
        in->nargs = k;
    bb->npreds = k;
  }
  // 自明な phi（引数がすべて同じか自分自身）を置き換える。変わらなくなるまで
  for (bool changed = true; changed;) {
    changed = false;
    for (int b = 0; b < ir_nblocks; b++) {
      IrBlock *bb = ir_blocks[b];
      if (!bb->reachable)
        continue;
      for (IrInst *in = bb->first; in; in = in->next) {
        if (in->op != IR_PHI || in->repl)
          continue;
        ir_resolve_operands(in);
        IrInst *same = NULL;
        bool trivial = true;
        for (int i = 0; i < in->nargs; i++) {
          IrInst *op = in->args[i];
          if (op == same || op == in)
            continue;
          if (same) {
            trivial = false;
            break;
          }
          same = op;
        }
        if (trivial && same) {
          in->repl = same;
          changed = true;
        }
      }
    }
  }
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable)
      continue;
    for (IrInst *in = bb->first, *next; in; in = next) {
      next = in->next;
      if (in->repl) {
        ir_remove(in);
        continue;
      }
      ir_resolve_operands(in);
    }
  }
}

// 使われる回数
static void ir_count_uses(void) {
  for (int b = 0; b < ir_nblocks; b++)
    for (IrInst *in = ir_blocks[b]->first; in; in = in->next)
      in->uses = 0;
  for (int b = 0; b < ir_nblocks; b++) {
    if (!ir_blocks[b]->reachable)
      continue;
    for (IrInst *in = ir_blocks[b]->first; in; in = in->next) {
      if (in->a)
        in->a->uses++;
      if (in->b && in->op != IR_COPY) // コピーの b は書き込む先
        in->b->uses++;
      if (in->c)
        in->c->uses++;
      for (int i = 0; i < in->nargs; i++)
        in->args[i]->uses++;
    }
  }
}


//
// 最適化（SSA の上で）: 定数の畳み込みと簡約、不要な拡張の除去、共通部分式の除去、不要なコードの除去
//

TLS static int ir_visit_stamp;

static bool ir_is_const_val(IrInst *v, int64_t c) {
  return v && v->op == IR_CONST && v->imm == c;
}

static IrInst *ir_const_before(IrInst *pos, int64_t c) {
  IrInst *k = ir_new(IR_CONST);
  k->imm = c;
  ir_insert_before(pos, k);
  return k;
}

static int64_t ir_ext_val(int64_t v, int size, bool sign) {
  switch (size) {
  case 1: return sign ? (int64_t)(int8_t)v : (int64_t)(uint8_t)v;
  case 2: return sign ? (int64_t)(int16_t)v : (int64_t)(uint16_t)v;
  case 4: return sign ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
  }
  return v;
}

// 値 v が、下位 bits ビットを符号拡張（sign）またはゼロ拡張（!sign）した形か
static bool ir_known_fits(int b, bool s, int bits, bool sign) {
  if (sign)
    return (s && b <= bits) || (!s && b < bits);
  return !s && b <= bits;
}

// 結果を覚えないので、1 回の問い合わせで調べる数に上限を設ける（false は常に安全な答え）
TLS static int ir_fits_steps;

static bool ir_fits_r(IrInst *v, int bits, bool sign, int depth) {
  if (bits >= 64)
    return true;
  if (depth > 16 || ++ir_fits_steps > 256)
    return false;
  if (v->nsw)
    return ir_known_fits(32, true, bits, sign);
  if (v->wform)
    return ir_known_fits(32, false, bits, sign);
  switch (v->op) {
  case IR_CONST:
    if (sign)
      return v->imm >= -(1LL << (bits - 1)) && v->imm < (1LL << (bits - 1));
    return (uint64_t)v->imm < (1ULL << bits);
  case IR_EXT:
    return ir_known_fits(v->size * 8, v->sign, bits, sign);
  case IR_LOAD:
  case IR_CALL:
  case IR_XCHG:
    return v->size > 0 && v->size < 8 && ir_known_fits(v->size * 8, v->sign, bits, sign);
  case IR_CAS:
  case IR_CMP:
  case IR_FCMP:
  case IR_BOOL:
    return ir_known_fits(1, false, bits, sign);
  case IR_AND: {
    // どちらかがゼロ拡張された狭い値なら、結果も同じ幅に収まる（符号つきなら 1 ビット狭く）
    int b = sign ? bits - 1 : bits;
    if (ir_fits_r(v->a, b, false, depth + 1) || ir_fits_r(v->b, b, false, depth + 1))
      return true;
    return ir_fits_r(v->a, bits, sign, depth + 1) && ir_fits_r(v->b, bits, sign, depth + 1);
  }
  case IR_OR:
  case IR_XOR:
    // 両方が同じように拡張された値なら、ビットごとの演算の結果も同じ
    return ir_fits_r(v->a, bits, sign, depth + 1) && ir_fits_r(v->b, bits, sign, depth + 1);
  case IR_SHR:
    // ゼロ拡張された値の論理右シフト
    return !sign && ir_fits_r(v->a, bits, false, depth + 1);
  case IR_SAR:
    // 符号拡張された値の算術右シフト
    return sign && ir_fits_r(v->a, bits, true, depth + 1);
  case IR_PHI: {
    // 調べている途中の phi に戻ってきたら、満たすと仮定する（輪の外の定義が満たせば、帰納法で輪の中も満たす）。
    // 調べ終わったら印を消す（別の幅・符号で、または仮定なしで調べ直すことがあるので、結果を残さない）
    // 仮定は同じ幅・符号の問い合わせにだけ使う
    int key = bits * 2 + sign;
    if (v->visit == ir_visit_stamp)
      return v->fits_key == key;
    v->visit = ir_visit_stamp;
    v->fits_key = key;
    bool ok = true;
    for (int i = 0; i < v->nargs && ok; i++)
      ok = ir_fits_r(ir_resolve(v->args[i]), bits, sign, depth + 1);
    v->visit = 0;
    return ok;
  }
  default:
    return false;
  }
}

static bool ir_fits(IrInst *v, int bits, bool sign) {
  ir_visit_stamp++;
  ir_fits_steps = 0;
  return ir_fits_r(v, bits, sign, 0);
}

static bool ir_commutative(IrOp op) {
  return op == IR_ADD || op == IR_MUL || op == IR_AND || op == IR_OR || op == IR_XOR;
}

static int ir_log2(int64_t v) {
  if (v <= 0 || (v & (v - 1)))
    return -1;
  int k = 0;
  while ((1LL << k) != v)
    k++;
  return k;
}

static bool ir_fold(IrOp op, int64_t a, int64_t b, int cond, int64_t *r) {
  uint64_t ua = a, ub = b;
  switch (op) {
  case IR_ADD: *r = (int64_t)(ua + ub); return true;
  case IR_SUB: *r = (int64_t)(ua - ub); return true;
  case IR_MUL: *r = (int64_t)(ua * ub); return true;
  case IR_SDIV: if (!b || (a == INT64_MIN && b == -1)) return false; *r = a / b; return true;
  case IR_UDIV: if (!b) return false; *r = (int64_t)(ua / ub); return true;
  case IR_SREM: if (!b || (a == INT64_MIN && b == -1)) return false; *r = a % b; return true;
  case IR_UREM: if (!b) return false; *r = (int64_t)(ua % ub); return true;
  case IR_AND: *r = a & b; return true;
  case IR_OR: *r = a | b; return true;
  case IR_XOR: *r = a ^ b; return true;
  case IR_SHL: if (ub > 63) return false; *r = (int64_t)(ua << ub); return true;
  case IR_SHR: if (ub > 63) return false; *r = (int64_t)(ua >> ub); return true;
  case IR_SAR: if (ub > 63) return false; *r = a >> ub; return true;
  case IR_CMP:
    switch (cond) {
    case CC_EQ: *r = a == b; return true;
    case CC_NE: *r = a != b; return true;
    case CC_LT: *r = a < b; return true;
    case CC_LE: *r = a <= b; return true;
    case CC_GT: *r = a > b; return true;
    case CC_GE: *r = a >= b; return true;
    case CC_LO: *r = ua < ub; return true;
    case CC_LS: *r = ua <= ub; return true;
    case CC_HI: *r = ua > ub; return true;
    case CC_HS: *r = ua >= ub; return true;
    }
    return false;
  default:
    return false;
  }
}

static const int ir_swap_cond[] = {CC_EQ, CC_NE, CC_GT, CC_GE, CC_LT, CC_LE, CC_HI, CC_HS, CC_LO, CC_LS};

// 1 つの命令を簡単にする。同じ値になる命令（置き換え先）を返すか、命令を書き換えて NULL を返す
static IrInst *ir_simplify(IrInst *in) {
  IrInst *a = in->a, *b = in->b;
  switch (in->op) {
  case IR_EXT:
    if (a->op == IR_CONST)
      return ir_const_before(in, ir_ext_val(a->imm, in->size, in->sign));
    if (ir_fits(a, in->size * 8, in->sign))
      return a;
    if (a->op == IR_EXT && a->size >= in->size) {
      in->a = a->a; // 外側の方が狭い: 内側の拡張は要らない
      return NULL;
    }
    return NULL;
  case IR_BOOL:
    if (a->op == IR_CONST)
      return ir_const_before(in, a->imm != 0);
    if (ir_fits(a, 1, false))
      return a;
    return NULL;
  case IR_NEG:
  case IR_NOT:
    if (a->op == IR_CONST) {
      int64_t r = in->op == IR_NEG ? (int64_t)(0 - (uint64_t)a->imm) : ~a->imm;
      if (in->wform)
        r = (int64_t)(uint32_t)r;
      else if (in->nsw)
        r = (int64_t)(int32_t)r;
      return ir_const_before(in, r);
    }
    return NULL;
  case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM:
  case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR: case IR_SAR: case IR_CMP: {
    if (a->op == IR_CONST && b->op != IR_CONST && (ir_commutative(in->op) || in->op == IR_CMP)) {
      in->a = b;
      in->b = a;
      if (in->op == IR_CMP)
        in->cond = ir_swap_cond[in->cond];
      a = in->a;
      b = in->b;
    }
    int64_t r;
    if (a->op == IR_CONST && b->op == IR_CONST && ir_fold(in->op, a->imm, b->imm, in->cond, &r)) {
      if (in->wform)
        r = (int64_t)(uint32_t)r;
      else if (in->nsw)
        r = (int64_t)(int32_t)r;
      return ir_const_before(in, r);
    }
    if (b->op != IR_CONST) {
      if (a == b && (in->op == IR_SUB || in->op == IR_XOR))
        return ir_const_before(in, 0);
      return NULL;
    }
    int64_t c = b->imm;
    // (x ± c1) ± c2 → x + (±c1 ± c2)（後置の増減 x++ は (x += 1) - 1 の形で来る。2026-11-27）。同じ形
    // （w の命令か・符号つきのあふれなしか）のときだけ
    if ((in->op == IR_ADD || in->op == IR_SUB) && (a->op == IR_ADD || a->op == IR_SUB) && a->b &&
        a->b->op == IR_CONST && !a->fp && a->wform == in->wform && a->nsw == in->nsw) {
      uint64_t t = (in->op == IR_ADD ? (uint64_t)c : 0 - (uint64_t)c) +
                   (a->op == IR_ADD ? (uint64_t)a->b->imm : 0 - (uint64_t)a->b->imm);
      if (in->wform)
        t = (uint32_t)t;
      if (t == 0)
        return a->a;
      in->op = IR_ADD;
      in->a = a->a;
      in->b = ir_const_before(in, (int64_t)t);
      return NULL;
    }
    switch (in->op) {
    case IR_ADD:
    case IR_SUB:
    case IR_OR:
    case IR_XOR:
    case IR_SHL:
    case IR_SHR:
    case IR_SAR:
      if (c == 0)
        return a;
      return NULL;
    case IR_MUL:
      if (c == 1)
        return a;
      if (c == 0)
        return b;
      if (ir_log2(c) > 0) {
        in->op = IR_SHL;
        in->b = ir_const_before(in, ir_log2(c));
      }
      return NULL;
    case IR_UDIV:
      if (c == 1)
        return a;
      if (ir_log2(c) > 0) {
        in->op = IR_SHR;
        in->b = ir_const_before(in, ir_log2(c));
      }
      return NULL;
    case IR_SDIV:
      if (c == 1)
        return a;
      return NULL;
    case IR_AND:
      if (c == -1)
        return a;
      if (c == 0)
        return b;
      return NULL;
    default:
      return NULL;
    }
  }
  case IR_PHI: {
    // すべて同じ値の phi
    IrInst *same = NULL;
    for (int i = 0; i < in->nargs; i++) {
      IrInst *op = in->args[i];
      if (op == same || op == in)
        continue;
      if (same)
        return NULL;
      same = op;
    }
    return same;
  }
  case IR_CBR:
    // 0 との比較・真偽値への変換をはずす（cbz・cbnz にできる）
    if (a->op == IR_BOOL) {
      in->a = a->a;
      return NULL;
    }
    if (a->op == IR_CMP && ir_is_const_val(a->b, 0) && (a->cond == CC_NE || a->cond == CC_EQ)) {
      in->a = a->a;
      if (a->cond == CC_EQ) {
        IrBlock *t = in->bb->succ[0];
        in->bb->succ[0] = in->bb->succ[1];
        in->bb->succ[1] = t;
      }
      return NULL;
    }
    return NULL;
  default:
    return NULL;
  }
}

// 置き換えた値をたどり、置き換えた命令を除く
static void ir_fixup(void) {
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable)
      continue;
    for (IrInst *in = bb->first, *next; in; in = next) {
      next = in->next;
      if (in->repl) {
        ir_remove(in);
        continue;
      }
      ir_resolve_operands(in);
      in->c = ir_resolve(in->c);
    }
  }
}

static void ir_simplify_all(void) {
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable)
      continue;
    for (IrInst *in = bb->first; in; in = in->next) {
      if (in->repl)
        continue;
      ir_resolve_operands(in);
      IrInst *r = ir_simplify(in);
      if (r && r != in)
        in->repl = r;
    }
  }
  ir_fixup();
}

// 支配木（Cooper, Harvey, Kennedy, "A Simple, Fast Dominance Algorithm"）
TLS static IrBlock **ir_rpo;
TLS static int ir_nrpo;

static void ir_rpo_dfs(IrBlock *bb, bool *seen) {
  seen[bb->id] = true;
  for (int i = 0; i < bb->nsucc; i++)
    if (!seen[bb->succ[i]->id])
      ir_rpo_dfs(bb->succ[i], seen);
  ir_rpo[ir_nrpo++] = bb;
}

static IrBlock *ir_intersect(IrBlock *a, IrBlock *b) {
  while (a != b) {
    while (a->rpo > b->rpo)
      a = a->idom;
    while (b->rpo > a->rpo)
      b = b->idom;
  }
  return a;
}

static void ir_dominators(void) {
  ir_rpo = ir_alloc(sizeof(IrBlock *) * ir_nblocks);
  ir_nrpo = 0;
  bool *seen = ir_alloc(ir_nblocks);
  ir_rpo_dfs(ir_blocks[0], seen);
  for (int i = 0; i < ir_nrpo / 2; i++) {
    IrBlock *t = ir_rpo[i];
    ir_rpo[i] = ir_rpo[ir_nrpo - 1 - i];
    ir_rpo[ir_nrpo - 1 - i] = t;
  }
  for (int i = 0; i < ir_nrpo; i++) {
    ir_rpo[i]->rpo = i;
    ir_rpo[i]->idom = NULL;
  }
  IrBlock *entry = ir_rpo[0];
  entry->idom = entry;
  for (bool changed = true; changed;) {
    changed = false;
    for (int i = 1; i < ir_nrpo; i++) {
      IrBlock *bb = ir_rpo[i];
      IrBlock *nd = NULL;
      for (int k = 0; k < bb->npreds; k++) {
        IrBlock *p = bb->preds[k];
        if (!p->idom)
          continue;
        nd = nd ? ir_intersect(p, nd) : p;
      }
      if (nd && bb->idom != nd) {
        bb->idom = nd;
        changed = true;
      }
    }
  }
}

static bool ir_dominates(IrBlock *a, IrBlock *b) {
  for (;;) {
    if (a == b)
      return true;
    if (b->idom == b)
      return false;
    b = b->idom;
  }
}

static bool ir_cse_candidate(IrInst *in) {
  switch (in->op) {
  case IR_CONST: case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV:
  case IR_SREM: case IR_UREM: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
  case IR_SAR: case IR_NEG: case IR_NOT: case IR_EXT: case IR_BOOL: case IR_CMP:
  case IR_LADDR: case IR_GADDR: case IR_TP: case IR_TLSADDR: // TLS のアドレスは関数の中では変わらない
  case IR_FCONST: // 浮動小数点の演算は除く（fesetround で丸めモードを変えるコードがあるため）
    return true;
  default:
    return false;
  }
}

static bool ir_same_expr(IrInst *x, IrInst *y) {
  return x->op == y->op && x->a == y->a && x->b == y->b && x->c == y->c && x->imm == y->imm && x->size == y->size &&
         x->sign == y->sign && x->cond == y->cond && x->sym == y->sym && x->fsz == y->fsz &&
         x->wform == y->wform && x->nsw == y->nsw;
}

static uint64_t ir_expr_hash(IrInst *in) {
  uint64_t h = in->op * 0x9E3779B97F4A7C15ULL;
  h ^= (uint64_t)(in->a ? in->a->id + 1 : 0) * 0xC2B2AE3D27D4EB4FULL;
  h ^= (uint64_t)(in->b ? in->b->id + 1 : 0) * 0x165667B19E3779F9ULL;
  h ^= (uint64_t)in->imm * 0x27D4EB2F165667C5ULL;
  h ^= (uint64_t)(in->size * 2 + in->sign + in->cond * 16);
  h ^= (uint64_t)(uintptr_t)in->sym;
  return h ^ (h >> 29);
}

// 共通部分式の除去: 逆後順にたどり、自分を支配する同じ計算があれば置き換える
static void ir_cse(void) {
  int cap = 1;
  while (cap < ir_nvals * 2 + 16)
    cap *= 2;
  IrInst **table = ir_alloc(sizeof(IrInst *) * cap);
  IrInst **chain = ir_alloc_nz(sizeof(IrInst *) * (ir_nvals + 1)); // 同じ hash の次（番号で引く。表に入れるときに書く）
  for (int i = 0; i < ir_nrpo; i++) {
    IrBlock *bb = ir_rpo[i];
    for (IrInst *in = bb->first; in; in = in->next) {
      if (in->repl || !ir_cse_candidate(in))
        continue;
      ir_resolve_operands(in);
      uint64_t h = ir_expr_hash(in) & (cap - 1);
      IrInst *found = NULL;
      for (IrInst *c = table[h]; c; c = chain[c->id])
        if (ir_same_expr(c, in) && ir_dominates(c->bb, bb)) {
          found = c;
          break;
        }
      if (found) {
        in->repl = found;
        continue;
      }
      if (in->id < ir_nvals) {
        chain[in->id] = table[h];
        table[h] = in;
      }
    }
  }
  ir_fixup();
}

static bool ir_is_pure(IrInst *in) {
  switch (in->op) {
  case IR_STORE: case IR_CALL: case IR_RET: case IR_BR: case IR_CBR: case IR_UNREACH: case IR_COPY:
  case IR_INDBR: case IR_SWTAB:
  case IR_FSQRT: // 負の数では sqrt を呼んで errno を設定する
    return in->sqrt_nonneg;
  case IR_MEMZERO:
  case IR_MEMCPY:
  case IR_PARAM_STRUCT:
  case IR_ASM:
  case IR_CAS:
  case IR_XCHG:
  case IR_ALLOCA:   // 呼ぶたびに別のメモリ（まとめない・消さない）
  case IR_X1:       // 呼び出しのすぐ後でなければならない
    return false;
  case IR_LOAD:
    return !in->vol;
  default:
    return true;
  }
}

// 不要なコードの除去: 使われない純粋な計算を除く（使う命令が消えると、その引数も見直す）
static void ir_dce(void) {
  ir_count_uses();
  for (bool changed = true; changed;) {
    changed = false;
    for (int b = 0; b < ir_nblocks; b++) {
      IrBlock *bb = ir_blocks[b];
      if (!bb->reachable)
        continue;
      for (IrInst *in = bb->first, *next; in; in = next) {
        next = in->next;
        if (in->uses > 0 || !ir_is_pure(in))
          continue;
        if (in->a)
          in->a->uses--;
        if (in->b)
          in->b->uses--;
        if (in->c)
          in->c->uses--;
        for (int i = 0; i < in->nargs; i++)
          in->args[i]->uses--;
        ir_remove(in);
        changed = true;
      }
    }
  }
}

// 32 ビットの値として比べられるか。w レジスタで比べた結果が x レジスタで比べた結果と同じになるのは:
// 両方が符号拡張した 32 ビット（符号つき・符号なしのどちらの比較でも順序が変わらない）、
// または両方がゼロ拡張した 32 ビットで、等しいかの比較か符号なしの比較のとき
static bool ir_cmp_w(IrInst *in) {
  if (ir_fits(in->a, 32, true) && ir_fits(in->b, 32, true))
    return true;
  bool usig = in->cond == CC_EQ || in->cond == CC_NE || in->cond == CC_LO || in->cond == CC_LS ||
              in->cond == CC_HI || in->cond == CC_HS;
  return usig && ir_fits(in->a, 32, false) && ir_fits(in->b, 32, false);
}

static bool ir_is_nsw32(IrInst *v) {
  return v->nsw && (v->op == IR_ADD || v->op == IR_SUB || v->op == IR_MUL || v->op == IR_SDIV || v->op == IR_SREM);
}

// in が値 v（in のオペランド）の下位 32 ビットだけを読むか
static bool ir_use_low32(IrInst *in, IrInst *v) {
  switch (in->op) {
  case IR_STORE:
    return in->b == v && in->a != v && in->c != v && !in->b->fp && in->size <= 4;
  case IR_EXT:
    return in->size <= 4;
  case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_SREM:
    // 4 バイトの読み出し・戻り値（広げないで済ませるための印）は、符号つきの計算の結果も下位 32 ビットだけ
    // 使われるときだけ（でなければ結果を sxtw で広げ直すので、前の ldrsw より 1 命令増える。2026-11-27）
    if ((v->op == IR_LOAD || v->op == IR_CALL) && in->nsw && !in->wform)
      return in->low32;
    return in->nsw || in->wform; // w レジスタの命令にする
  case IR_SHL:
    return in->wform;
  case IR_NEG: case IR_NOT:
    return in->wform;
  case IR_CMP:
    return ir_cmp_w(in);
  case IR_BOOL:
  case IR_CBR:
    return ir_fits(v, 32, true) || ir_fits(v, 32, false);
  case IR_PHI:
    return in->low32;
  case IR_SELECT:     // 選ぶ値（b・c）は phi と同じ。条件（a）は IR_CBR と同じ
    if (v == in->a && v != in->b && v != in->c)
      return ir_fits(v, 32, true) || ir_fits(v, 32, false);
    return in->low32 && v != in->a;
  default:
    return false;
  }
}

// 下位 32 ビットだけが使われる値を求める（楽観的に始めて、そうでない使い方を見つけたら外す。phi を通して伝わる）
static void ir_mark_low32(void) {
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (bb->reachable)
      for (IrInst *in = bb->first; in; in = in->next)
        in->low32 = ir_is_nsw32(in) || ((in->op == IR_PHI || in->op == IR_SELECT) && !in->fp) ||
                    (in->op == IR_EXT && in->size == 4) ||
                    // 4 バイトの読み出し・戻り値（下位 32 ビットだけを使うなら符号拡張しない。2026-11-27）
                    (in->op == IR_LOAD && in->size == 4 && !in->fp) ||
                    (in->op == IR_CALL && in->size == 4 && !in->fp && !in->ty);
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (int b = 0; b < ir_nblocks; b++) {
      IrBlock *bb = ir_blocks[b];
      if (!bb->reachable)
        continue;
      for (IrInst *in = bb->first; in; in = in->next) {
        IrInst *ops[3] = {in->a, in->b, in->c};
        for (int j = 0; j < 3 + in->nargs; j++) {
          IrInst *v = j < 3 ? ops[j] : in->args[j - 3];
          if (v && v->low32 && !ir_use_low32(in, v)) {
            v->low32 = false;
            changed = true;
          }
        }
      }
    }
  }
}

// ロード・ストアのアドレスを、アドレッシングモードに畳み込む:
//   [x29 + ofs]（メモリに置いた局所変数）、[base + imm]、[base + idx]、[base + idx << log2(size)]
static void ir_fold_addresses(void) {
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable)
      continue;
    for (IrInst *in = bb->first; in; in = in->next) {
      if ((in->op != IR_LOAD && in->op != IR_STORE) || in->acq)
        continue;
      IrInst *addr = in->a;
      if (addr->op == IR_LADDR) {
        in->a = NULL;
        in->fp_base = true;
        in->imm = addr->imm;
        continue;
      }
      if (addr->op != IR_ADD)
        continue;
      IrInst *base = addr->a, *idx = addr->b;
      if (idx->op == IR_CONST && idx->imm >= -256 && idx->imm < 4096 * in->size) {
        if (base->op == IR_LADDR) {
          in->a = NULL;
          in->fp_base = true;
          in->imm = base->imm + idx->imm;
        } else {
          in->a = base;
          in->imm = idx->imm;
        }
        continue;
      }
      if (idx->op == IR_CONST || base->op == IR_LADDR)
        continue;
      in->a = base;
      in->c = idx;
      in->shift = 0;
      int k = ir_log2(in->size);
      if (idx->op == IR_SHL && idx->b->op == IR_CONST && idx->b->imm == k && k > 0) {
        in->c = idx->a;
        in->shift = k;
      }
    }
  }
}

// bb を pred から来る辺を除く（phi の引数も一緒に）
static void ir_remove_edge(IrBlock *pred, IrBlock *bb) {
  int k = -1;
  for (int i = 0; i < bb->npreds; i++)
    if (bb->preds[i] == pred)
      k = i;
  if (k < 0)
    return;
  for (IrInst *in = bb->first; in; in = in->next)
    if (in->op == IR_PHI && in->nargs == bb->npreds) {
      memmove(in->args + k, in->args + k + 1, sizeof(IrInst *) * (in->nargs - k - 1));
      in->nargs--;
    }
  memmove(bb->preds + k, bb->preds + k + 1, sizeof(IrBlock *) * (bb->npreds - k - 1));
  bb->npreds--;
}

// 条件が定数の分岐を、まっすぐな分岐にする。通らなくなったブロックを除く（インライン展開の後に多い）
static void ir_fold_branches(void) {
  bool changed = false;
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    IrInst *br = bb->last;
    if (!bb->reachable || !br || br->op != IR_CBR || br->a->op != IR_CONST)
      continue;
    IrBlock *taken = br->a->imm ? bb->succ[0] : bb->succ[1];
    IrBlock *other = br->a->imm ? bb->succ[1] : bb->succ[0];
    if (taken != other)
      ir_remove_edge(bb, other);
    br->op = IR_BR;
    br->a = NULL;
    bb->succ[0] = taken;
    bb->nsucc = 1;
    changed = true;
  }
  if (!changed)
    return;
  // 到達できなくなったブロックを除く
  for (int b = 0; b < ir_nblocks; b++)
    ir_blocks[b]->reachable = false;
  ir_mark_reachable(ir_blocks[0]);
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (bb->reachable)
      continue;
    for (int i = 0; i < bb->nsucc; i++)
      if (bb->succ[i]->reachable)
        ir_remove_edge(bb, bb->succ[i]);
  }
}

// 読み書き x と y が重なりうるか（LOAD・STORE）。わからなければ true
static bool ir_may_alias(IrInst *x, IrInst *y) {
  if (x->vol || y->vol)
    return true;
  // 同じ土台・同じ添字なら、ずれの範囲で決まる
  if (x->fp_base == y->fp_base && x->a == y->a && x->c == y->c && x->shift == y->shift)
    return x->imm < y->imm + y->size && y->imm < x->imm + x->size;
  // 同じ構造体の、重ならない別のメンバー
  if (x->mem_struct && x->mem_struct == y->mem_struct)
    return x->mem_off < y->mem_off + y->size && y->mem_off < x->mem_off + x->size;
  // 型の組の違う読み書き（土台が同じものは上で決めるので、共用体の型の読み替えはここに来ない）
  if (x->tbaa && y->tbaa && x->tbaa != y->tbaa)
    return false;
  return true;
}

// x と y が同じ場所の同じ大きさの読み書きか
static bool ir_same_location(IrInst *x, IrInst *y) {
  return x->fp_base == y->fp_base && x->a == y->a && x->c == y->c && x->shift == y->shift &&
         x->imm == y->imm && x->size == y->size;
}

// 冗長な読み出しの除去: 同じ場所をもう一度読むとき、書いた直後に読むときは、メモリを読まずにその値を使う。
// 基本ブロックごとに「わかっている中身」の表を持ち、前任が 1 つならその終わりの表を、2 つ以上なら
// すべての前任の表で一致するものだけを引き継ぐ（戻る辺の前任がまだなら、何も引き継がない）。
// 書き込みは重なりうるもの（ir_may_alias）を消す。呼び出し・memcpy・インラインアセンブリはすべて消す。
// FSQRT は負の数のとき sqrt を呼んで errno（int）を書くので、整数の読み出しだけ消す
#define IR_AVAIL_MAX 64
typedef struct {
  IrInst *mem;  // LOAD か STORE（場所）
  IrInst *val;  // その場所の値
} IrAvail;

// e は buf の中の窓（いっぱいになったら古いものを捨てるのに、先頭を進めるだけで済むように。端に来たら
// まとめて buf の先頭に戻す。2026-11-16。前は捨てるたびに 64 個をずらしていた）
typedef struct {
  IrAvail *e;
  int n;
  IrAvail buf[IR_AVAIL_MAX * 4];
} IrAvailSet;

static void ir_avail_kill(IrAvailSet *s, IrInst *st) {
  int k = 0;
  for (int i = 0; i < s->n; i++)
    if (!ir_may_alias(s->e[i].mem, st))
      s->e[k++] = s->e[i];
  s->n = k;
}

static void ir_avail_add(IrAvailSet *s, IrInst *mem, IrInst *val) {
  if (s->n == IR_AVAIL_MAX) {   // いっぱいなら古いものから捨てる
    s->e++;
    s->n--;
  }
  if (s->e + s->n == s->buf + IR_AVAIL_MAX * 4) {
    memmove(s->buf, s->e, sizeof(IrAvail) * s->n);
    s->e = s->buf;
  }
  s->e[s->n++] = (IrAvail){mem, val};
}

// 読み出し ld を、わかっている値 e で置き換えられるか
static bool ir_avail_match(IrAvail *e, IrInst *ld) {
  if (!ir_same_location(e->mem, ld) || e->val->fp != ld->fp)
    return false;
  if (ld->fp)
    return e->val->fsz == ld->fsz;
  if (e->mem->op == IR_LOAD)
    return e->mem->sign == ld->sign;
  // 書いた値: 読み出しの幅と符号に拡張された値として使えるか（例: int の値を char として書いた後、
  // char として読むなら、下位 8 ビットを符号拡張したものでなければならない）
  return ld->size == 8 || ir_fits(e->val, ld->size * 8, ld->sign);
}

// ブロックの出口の表（使った分だけ持つ。2026-11-16。前は 64 個分の表を丸ごと写し・0 にしていた）
typedef struct {
  IrAvail *e;
  int n;
  bool done;
} IrAvailOut;

static void ir_load_elim(void) {
  IrAvailOut *out = ir_alloc(sizeof(IrAvailOut) * ir_nblocks);
  IrAvailSet cur;   // 大きいので、ループの外に置く（ブロックごとに作り直さない）
  bool changed = false;
  for (int i = 0; i < ir_nrpo; i++) {
    IrBlock *bb = ir_rpo[i];
    cur.e = cur.buf;
    cur.n = 0;
    // 前任の表を引き継ぐ
    bool all_done = bb->npreds > 0;
    for (int p = 0; p < bb->npreds; p++)
      if (!out[bb->preds[p]->id].done)
        all_done = false;
    if (all_done) {
      IrAvailOut *o0 = &out[bb->preds[0]->id];
      cur.n = o0->n;
      if (o0->n)
        memcpy(cur.e, o0->e, sizeof(IrAvail) * o0->n);
      for (int p = 1; p < bb->npreds; p++) {
        IrAvailOut *o = &out[bb->preds[p]->id];
        int k = 0;
        for (int x = 0; x < cur.n; x++) {
          bool found = false;
          for (int y = 0; y < o->n && !found; y++)
            found = o->e[y].mem == cur.e[x].mem && o->e[y].val == cur.e[x].val;
          if (found)
            cur.e[k++] = cur.e[x];
        }
        cur.n = k;
      }
    }
    for (IrInst *in = bb->first; in; in = in->next) {
      switch (in->op) {
      case IR_LOAD: {
        if (in->acq)
          cur.n = 0; // 獲得（acquire）より後の読み出しを、前の値で済ませない
        if (in->vol)
          break;
        IrInst *v = NULL;
        for (int x = cur.n - 1; x >= 0 && !v; x--)
          if (ir_avail_match(&cur.e[x], in))
            v = cur.e[x].val;
        if (v) {
          in->repl = v;
          changed = true;
        } else {
          ir_avail_add(&cur, in, in);
        }
        break;
      }
      case IR_STORE:
        ir_avail_kill(&cur, in);
        if (!in->vol)
          ir_avail_add(&cur, in, in->b);
        break;
      case IR_FSQRT: {
        if (in->sqrt_nonneg)
          break;
        int k = 0;
        for (int x = 0; x < cur.n; x++)
          if (cur.e[x].mem->size != 4 || cur.e[x].val->fp)
            cur.e[k++] = cur.e[x];
        cur.n = k;
        break;
      }
      case IR_CALL: case IR_MEMZERO: case IR_MEMCPY: case IR_ASM: case IR_ASMOUT:
      case IR_PARAM_STRUCT: case IR_CAS: case IR_XCHG:
        cur.n = 0;
        break;
      default:
        break;
      }
    }
    IrAvailOut *o = &out[bb->id];
    o->done = true;
    o->n = cur.n;
    if (cur.n) {
      o->e = ir_alloc_nz(sizeof(IrAvail) * cur.n);
      memcpy(o->e, cur.e, sizeof(IrAvail) * cur.n);
    }
  }
  if (changed)
    ir_fixup();
}

// 浮動小数点の値が負にならないか（-0.0 は負とみなさない。NaN は sqrt で errno を設定しないので構わない）
static bool ir_fp_nonneg(IrInst *v, int depth) {
  v = ir_resolve(v);
  if (depth > 8)
    return false;
  switch (v->op) {
  case IR_FMUL:
    return ir_resolve(v->a) == ir_resolve(v->b) ||    // x * x
           (ir_fp_nonneg(v->a, depth + 1) && ir_fp_nonneg(v->b, depth + 1));
  case IR_FADD:
    return ir_fp_nonneg(v->a, depth + 1) && ir_fp_nonneg(v->b, depth + 1);
  case IR_FABS:
  case IR_FSQRT:
    return true;
  case IR_FCONST:
    return v->fsz == 8 ? !((uint64_t)v->imm >> 63) : !(((uint64_t)v->imm >> 31) & 1);
  default:
    return false;
  }
}

static void ir_mark_sqrt(void) {
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (bb->reachable)
      for (IrInst *in = bb->first; in; in = in->next)
        if (in->op == IR_FSQRT && !in->sqrt_nonneg && ir_fp_nonneg(in->a, 0))
          in->sqrt_nonneg = true;
  }
}

static void ir_licm(void);

// 中間表現の指紋（ir_optimize が、1 回まわして何も変わらなかったかを見る）
static uint64_t ir_fingerprint(void) {
  uint64_t h = 1469598103934665603ULL;
#define IR_FP(x) (h = (h ^ (uint64_t)(x)) * 1099511628211ULL)
  for (int b = 0; b < ir_nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable)
      continue;
    IR_FP(bb->id);
    IR_FP(bb->nsucc);
    for (int k = 0; k < bb->nsucc; k++)
      IR_FP(bb->succ[k]->id);
    IR_FP(bb->npreds);
    for (IrInst *in = bb->first; in; in = in->next) {
      IR_FP(in->op);
      IR_FP(in->id);
      IR_FP(in->a ? in->a->id + 1 : 0);
      IR_FP(in->b ? in->b->id + 1 : 0);
      IR_FP(in->c ? in->c->id + 1 : 0);
      IR_FP(in->nargs);
      for (int i = 0; i < in->nargs; i++)
        IR_FP(in->args[i] ? in->args[i]->id + 1 : 0);
      IR_FP(in->imm);
      IR_FP(in->cond);
      IR_FP(in->size);
      IR_FP(in->sign | in->sqrt_nonneg << 1 | in->fp << 2 | in->vol << 3);
      IR_FP((uintptr_t)in->sym);
      IR_FP(in->shift);
    }
  }
#undef IR_FP
  return h;
}

// 局所変数の場所を値にする（mem2reg。SROA の 2 段目。2026-11-21）: アドレスが、定数のオフセットの読み書き
// （と定数を足す計算）にしか使われない場所（構造体の局所変数・戻り値の領域・インライン展開の引数）を、8 バイトの
// 塊ごとの変数として SSA にする（Braun らの方法を、ブロックを逆後順にたどってやり直す: 前任がすべて済んだブロック
// から封じる）。1・2・4 バイトの読み書き（JSValue の u.int32 など）は、塊の値のシフトと切り詰め・合成で表す。
// 浮動小数点の読み書き・volatile・アトミック・添字のある読み書き・ほかへ渡すアドレスがあれば、その場所はそのまま
#define IR_M2R_MAX 32   // 値にする場所の大きさの上限

// 命令を pos の前に足す（ir_cur を一時的なブロックにして作り、つなぎ替える）
TLS static IrBlock ir_m2r_tmp;
static void ir_m2r_begin(void) {
  ir_m2r_tmp = (IrBlock){0};
  ir_cur = &ir_m2r_tmp;
}
static void ir_m2r_end(IrInst *pos) {
  for (IrInst *in = ir_m2r_tmp.first, *next; in; in = next) {
    next = in->next;
    in->bb = pos->bb;
    in->prev = pos->prev;
    in->next = pos;
    if (pos->prev)
      pos->prev->next = in;
    else
      pos->bb->first = in;
    pos->prev = in;
  }
}

static bool ir_m2r_size_ok(int64_t off, int size) {
  return (size == 1 || size == 2 || size == 4 || size == 8) && off % size == 0 && off / 8 == (off + size - 1) / 8;
}

static bool ir_mem2reg(void) {
  int ns = ir_nslots;
  if (!ns)
    return false;
  int *aslot = ir_alloc(sizeof(int) * (ir_nvals + 1));      // 場所の番号 + 1（0 はなし）
  int64_t *aoff = ir_alloc(sizeof(int64_t) * (ir_nvals + 1));
  bool *esc = ir_alloc(ns), *acc = ir_alloc(ns);
  for (int i = 0; i < ns; i++) {
    Type *ty = ir_slots[i].var->ty;
    if (ir_slots[i].ofs >= 0 || ty->size <= 0 || ty->size > IR_M2R_MAX)
      esc[i] = true;    // スタックで渡された引数（入口で書かない）・大きいもの
  }
  // アドレス: LADDR と、それに定数を足したもの
  for (int bi = 0; bi < ir_nrpo; bi++)
    for (IrInst *in = ir_rpo[bi]->first; in; in = in->next) {
      if (in->op == IR_LADDR) {
        for (int i = 0; i < ns; i++) {
          int64_t o = in->imm - ir_slots[i].ofs;
          if (o >= 0 && o < MAX(ir_slots[i].var->ty->size, 1)) {
            aslot[in->id] = i + 1;
            aoff[in->id] = o;
            break;
          }
        }
      } else if (in->op == IR_ADD && in->a && aslot[in->a->id] && in->b && in->b->op == IR_CONST) {
        int s = aslot[in->a->id] - 1;
        int64_t o = aoff[in->a->id] + in->b->imm;
        if (o < 0 || o >= ir_slots[s].var->ty->size)
          esc[s] = true;
        aslot[in->id] = s + 1;
        aoff[in->id] = o;
      }
      // 命令が直接書く場所（構造体の戻り値・レジスタで渡された構造体の引数）
      if ((in->op == IR_CALL && in->ty && in->rtn_slot) || in->op == IR_PARAM_STRUCT)
        for (int i = 0; i < ns; i++)
          if (in->rtn_slot >= ir_slots[i].ofs && in->rtn_slot < ir_slots[i].ofs + MAX(ir_slots[i].var->ty->size, 1))
            esc[i] = true;
    }
  // 使い方を調べる
  for (int bi = 0; bi < ir_nrpo; bi++)
    for (IrInst *in = ir_rpo[bi]->first; in; in = in->next) {
      IrInst *ops[3] = {in->a, in->b, in->c};
      for (int k = 0; k < 3 + in->nargs; k++) {
        IrInst *o = k < 3 ? ops[k] : in->args[k - 3];
        if (!o || !aslot[o->id])
          continue;
        int s = aslot[o->id] - 1;
        bool ok = false;
        int64_t off = aoff[o->id] + in->imm;
        if (in->op == IR_LOAD && k == 0 && !in->c && !in->vol && !in->acq && !in->fp)
          ok = ir_m2r_size_ok(off, in->size);
        else if (in->op == IR_STORE && k == 0 && in->b != o && !in->c && !in->vol && !in->b->fp)
          ok = ir_m2r_size_ok(off, in->size);
        else if (in->op == IR_ADD && k == 0 && in->b && in->b->op == IR_CONST)
          ok = true;    // 足した先の使い方はその命令で調べる
        if (!ok)
          esc[s] = true;
        else if (in->op == IR_LOAD || in->op == IR_STORE)
          acc[s] = true;
      }
    }
  bool any = false;
  for (int i = 0; i < ns; i++)
    if (acc[i] && !esc[i])
      any = true;
  if (!any)
    return false;

  // 塊の変数
  Obj ***fobj = ir_alloc(sizeof(Obj **) * ns);
  bool *filled = ir_alloc(ir_nblocks);
  // 元の変数の「ブロックの最新の値」は SSA を作り終えたので要らない（空にして、塊の変数だけを引く。大きな関数で
  // ir_read_var が何百もの元の変数をたどっていた）
  for (int bi = 0; bi < ir_nrpo; bi++) {
    ir_rpo[bi]->sealed = false;
    ir_rpo[bi]->ndefs = 0;
  }
  IrBlock *saved_cur = ir_cur;
  for (int bi = 0; bi < ir_nrpo; bi++) {
    IrBlock *bb = ir_rpo[bi];
    bool all = true;
    for (int q = 0; q < bb->npreds; q++)
      if (!filled[bb->preds[q]->id])
        all = false;
    if (all)
      ir_seal(bb);
    for (IrInst *in = bb->first, *next; in; in = next) {
      next = in->next;
      if ((in->op != IR_LOAD && in->op != IR_STORE) || !in->a || !aslot[in->a->id])
        continue;
      int s = aslot[in->a->id] - 1;
      if (esc[s] || !acc[s])
        continue;
      int64_t off = aoff[in->a->id] + in->imm;
      int64_t k = off / 8, sh = (off % 8) * 8;
      if (!fobj[s])
        fobj[s] = ir_alloc(sizeof(Obj *) * (IR_M2R_MAX / 8));
      if (!fobj[s][k]) {
        fobj[s][k] = ir_alloc(sizeof(Obj));
        fobj[s][k]->ty = ty_ulong;
      }
      Obj *f = fobj[s][k];
      ir_m2r_begin();
      if (in->op == IR_LOAD) {
        IrInst *v = ir_read_var(f, bb);
        ir_cur = &ir_m2r_tmp;   // ir_read_var は phi を bb に足すだけ（ir_cur は変えない）
        if (sh)
          v = ir_bin(IR_SHR, v, ir_const(sh));
        v = ir_ext(v, in->size, in->sign);
        ir_m2r_end(in);
        in->repl = v;
        ir_remove(in);
      } else {
        IrInst *val = ir_resolve(in->b);
        IrInst *nv;
        if (in->size == 8) {
          nv = val;
        } else {
          IrInst *old = ir_read_var(f, bb);
          ir_cur = &ir_m2r_tmp;
          uint64_t m = (in->size == 4 ? 0xffffffffULL : in->size == 2 ? 0xffffULL : 0xffULL) << sh;
          IrInst *part = ir_ext(val, in->size, false);
          if (sh)
            part = ir_bin(IR_SHL, part, ir_const(sh));
          nv = ir_bin(IR_OR, ir_bin(IR_AND, old, ir_const((int64_t)~m)), part);
        }
        ir_m2r_end(in);
        ir_write_var(f, bb, nv);
        ir_remove(in);
      }
    }
    filled[bb->id] = true;
    for (int q = 0; q < bb->nsucc; q++) {
      IrBlock *sb = bb->succ[q];
      if (sb->sealed)
        continue;
      bool ready = true;
      for (int r = 0; r < sb->npreds; r++)
        if (!filled[sb->preds[r]->id])
          ready = false;
      if (ready)
        ir_seal(sb);
    }
  }
  for (int bi = 0; bi < ir_nrpo; bi++)
    ir_seal(ir_rpo[bi]);
  ir_cur = saved_cur;
  // 置き換えた読み出しを指し直す（新しい phi のうち自明なものは、作るときに取り除いてある）
  for (int bi = 0; bi < ir_nrpo; bi++)
    for (IrInst *in = ir_rpo[bi]->first, *next; in; in = next) {
      next = in->next;
      if (in->repl) {
        ir_remove(in);
        continue;
      }
      ir_resolve_operands(in);
      in->c = ir_resolve(in->c);
    }
  return true;
}

//
// if 変換（2026-11-26）: 値を選ぶだけの小さな分岐を csel（IR_SELECT）にする。Cortex-A53 は分岐を読み違えると
// 7〜8 サイクル止まり、コンパイラのような分岐の多いコードでは読み違いが多い（GCC は csel をよく使う）。
// 形: ブロック B の終わりが IR_CBR(cond, T, F) で、
//   ひし形: T・F ともに前任が B だけ、中身が分岐だけか安い計算（IR_IFCVT_OPS 個まで）、どちらも同じ J へ飛ぶ。
//           J の前任は T と F だけ
//   三角形: T（か F）の前任が B だけで、中身が分岐だけか安い計算、もう一方が J そのもの。J の前任は B と T（F）だけ
// で、J の phi がすべて整数で IR_IFCVT_PHIS 個まで。T・F の計算を B の分岐の前に移し（止まらない・副作用のない
// 整数の計算だけなので、先に計算してもよい）、phi を IR_SELECT にして、B を J へまっすぐ飛ぶようにする。
// 計算を先にするので、片方の道しか通らないときより命令は増える。数を小さく抑える
//
#define IR_IFCVT_OPS 2
#define IR_IFCVT_PHIS 2

// 先に計算してよい（止まらない・副作用がない・安い整数の計算）
static bool ir_ifcvt_ok(IrInst *in) {
  if (in->fp)
    return false;
  switch (in->op) {
  case IR_CONST: case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
  case IR_SHL: case IR_SHR: case IR_SAR: case IR_NEG: case IR_NOT: case IR_EXT: case IR_BOOL: case IR_CMP:
    return true;
  default:
    return false;
  }
}

// 腕 arm（B の後の小さなブロック）の中身が、安い計算 IR_IFCVT_OPS 個までと、J への分岐だけか
static bool ir_ifcvt_arm(IrBlock *arm, IrBlock *b, IrBlock *j) {
  if (arm == b || arm == j || arm->npreds != 1 || arm->preds[0] != b || !arm->last || arm->last->op != IR_BR ||
      arm->nsucc != 1 || arm->succ[0] != j)
    return false;
  int n = 0;
  for (IrInst *in = arm->first; in != arm->last; in = in->next)
    if (in->op == IR_PHI || !ir_ifcvt_ok(in) || ++n > IR_IFCVT_OPS)
      return false;
  return true;
}

// arm の計算（分岐の前まで）を pos の前に移す
static void ir_ifcvt_hoist(IrBlock *arm, IrInst *pos) {
  for (IrInst *in = arm->first, *next; in && in != arm->last; in = next) {
    next = in->next;
    arm->first = next;
    next->prev = NULL;
    ir_insert_before(pos, in);
  }
}

static bool ir_if_convert(void) {
  bool changed = false;
  for (int bi = 0; bi < ir_nblocks; bi++) {
    IrBlock *b = ir_blocks[bi];
    IrInst *br = b->last;
    if (!b->reachable || !br || br->op != IR_CBR || br->a->fp || b->nsucc != 2)
      continue;
    IrBlock *t = b->succ[0], *f = b->succ[1], *j;
    IrBlock *ta, *fa;           // 腕（なければ NULL）。ta・fa から J に来る値が、真・偽のときの値
    if (t != f && t->nsucc == 1 && f->nsucc == 1 && t->succ[0] == f->succ[0] && ir_ifcvt_arm(t, b, t->succ[0]) &&
        ir_ifcvt_arm(f, b, t->succ[0])) {
      j = t->succ[0], ta = t, fa = f;                       // ひし形
    } else if (t->nsucc == 1 && t->succ[0] == f && ir_ifcvt_arm(t, b, f)) {
      j = f, ta = t, fa = NULL;                             // 三角形（真のときだけ T を通る）
    } else if (f->nsucc == 1 && f->succ[0] == t && ir_ifcvt_arm(f, b, t)) {
      j = t, ta = NULL, fa = f;                             // 三角形（偽のときだけ F を通る）
    } else {
      continue;
    }
    if (j == b || j->npreds != 2)
      continue;
    // J の前任のうち、真のとき・偽のときに来る辺の番号
    IrBlock *tp = ta ? ta : b, *fp = fa ? fa : b;
    int ti = -1, fi = -1;
    for (int i = 0; i < 2; i++) {
      if (j->preds[i] == tp)
        ti = i;
      if (j->preds[i] == fp)
        fi = i;
    }
    if (ti < 0 || fi < 0 || ti == fi)
      continue;
    int nphi = 0;
    bool ok = true;
    for (IrInst *in = j->first; in && ok; in = in->next) {
      if (in->op != IR_PHI)
        continue;
      if (in->repl)
        continue;
      ok = !in->fp && in->nargs == 2 && ++nphi <= IR_IFCVT_PHIS;
    }
    if (!ok)
      continue;
    // 変える: 腕の計算を B の分岐の前へ（分岐のすぐ前の比較があればその前へ。比較と csel をまとめるため）、
    // phi を IR_SELECT へ、B を J へまっすぐ
    IrInst *pos = br->prev && br->prev == br->a && br->a->op == IR_CMP ? br->a : br;
    while (pos->op == IR_CMP && pos->c && pos->c == pos->prev)
      pos = pos->c;     // ccmp の続き（ir_ccmp_chain）の頭の前へ（間にフラグを変える命令を入れない）
    if (ta)
      ir_ifcvt_hoist(ta, pos);
    if (fa)
      ir_ifcvt_hoist(fa, pos);
    for (IrInst *in = j->first; in; in = in->next) {
      if (in->op != IR_PHI || in->repl)
        continue;
      IrInst *sel = ir_new(IR_SELECT);
      sel->a = br->a;
      sel->b = in->args[ti];
      sel->c = in->args[fi];
      sel->size = in->size;
      sel->sign = in->sign;
      ir_insert_before(br, sel);
      in->repl = sel;
    }
    br->op = IR_BR;
    br->a = NULL;
    b->succ[0] = j;
    b->nsucc = 1;
    j->preds[0] = b;
    j->npreds = 1;
    for (IrInst *in = j->first; in; in = in->next)
      if (in->op == IR_PHI && !in->repl)
        in->nargs = 1;              // 来ないはず（phi はすべて select にした）
    if (ta) {
      ta->reachable = false;
      ta->npreds = 0;
    }
    if (fa) {
      fa->reachable = false;
      fa->npreds = 0;
    }
    changed = true;
  }
  if (changed)
    ir_fixup();
  return changed;
}

//
// 比べを続ける（ccmp。2026-11-27）: a && b・a || b のように、比べの分岐の先が、また比べて分岐するだけのブロックで、
// 2 つの分岐が同じ先 S を持つ形
//   B: … CMP1; CBR(CMP1) → X か S（どちらの向きでも）
//   X: （定数）; CMP2; CBR(CMP2) → S か N（どちらの向きでも）。X の前任は B だけ
// を、B: … CMP1; CMP2（ccmp）; CBR(CMP2) → N・S にする（「B から X へ行く条件」かつ「X から N へ行く条件」で N）。
// X の定数は CMP1 の前へ。S の phi は、B から来る値と X から来る値が同じときだけ。3 つ以上も続けてつなぐ。
// 分岐が 1 つ減り、その後の if 変換（1・0 を選ぶ形なら cset）にもかかるようになる
//
static bool ir_ccmp_block(IrBlock *x) {
  IrInst *br = x->last;
  if (!br || br->op != IR_CBR || x->nsucc != 2 || x->succ[0] == x->succ[1])
    return false;
  IrInst *c2 = br->a;
  if (c2->op != IR_CMP || c2->fp || c2->bb != x || c2->next != br || c2->uses != 1)
    return false;
  for (IrInst *in = x->first; in != c2; in = in->next)
    if (in->op != IR_CONST)
      return false;
  return true;
}

static bool ir_ccmp_chain(void) {
  bool changed = false;
  for (int bi = 0; bi < ir_nblocks; bi++) {
    IrBlock *b = ir_blocks[bi];
    while (b->reachable) {
      IrInst *br = b->last;
      if (!br || br->op != IR_CBR || b->nsucc != 2 || b->succ[0] == b->succ[1])
        break;
      IrInst *c1 = br->a;
      if (c1->op != IR_CMP || c1->fp || c1->next != br || c1->uses != 1)
        break;
      IrBlock *x = NULL, *s = NULL, *n = NULL;
      int xi = -1;
      for (int i = 0; i < 2 && !x; i++) {
        IrBlock *cand = b->succ[i], *other = b->succ[1 - i];
        if (cand == b || cand->npreds != 1 || !ir_ccmp_block(cand))
          continue;
        if (cand->succ[0] != other && cand->succ[1] != other)
          continue;
        x = cand, s = other, xi = i;
        n = cand->succ[0] == other ? cand->succ[1] : cand->succ[0];
      }
      if (!x || n == b || n == x || n == s || s == x)
        break;
      // S の phi は、B から来る値と X から来る値が同じこと
      int pb = -1, px = -1;
      for (int i = 0; i < s->npreds; i++) {
        if (s->preds[i] == b)
          pb = i;
        if (s->preds[i] == x)
          px = i;
      }
      if (pb < 0 || px < 0)
        break;
      bool ok = true;
      for (IrInst *in = s->first; in && ok; in = in->next)
        if (in->op == IR_PHI && !in->repl && in->nargs == s->npreds)
          ok = ir_resolve(in->args[pb]) == ir_resolve(in->args[px]);
      if (!ok)
        break;
      // 変える
      IrInst *xbr = x->last, *c2 = xbr->a;
      c2->ccond = xi == 0 ? c1->cond : cc_inverse[c1->cond];   // B から X へ行く条件
      if (x->succ[0] != n)
        c2->cond = cc_inverse[c2->cond];                      // 成り立てば N
      IrInst *head = c1;      // 続いた ccmp の頭（間にほかの命令を入れない）
      while (head->c && head->c == head->prev)
        head = head->c;
      for (IrInst *in = x->first, *next; in != c2; in = next) {
        next = in->next;
        ir_remove(in);
        ir_insert_before(head, in);
      }
      ir_remove(c2);
      ir_insert_before(br, c2);
      c2->c = c1;
      br->a = c2;
      b->succ[0] = n;
      b->succ[1] = s;
      for (int i = 0; i < n->npreds; i++)
        if (n->preds[i] == x)
          n->preds[i] = b;
      ir_remove_edge(x, s);
      x->reachable = false;
      x->npreds = 0;
      x->first = x->last = NULL;
      changed = true;
    }
  }
  return changed;
}

static void ir_optimize(void) {
  // 3 回まで。1 回まわして中間表現が変わらなければ、次の回も同じ結果なので止める（2026-11-15。出力は
  // 変わらない。ほとんどの関数は 2 回目で変わらなくなる）
  uint64_t prev = 0;
  for (int round = 0; round < 3; round++) {
    ir_simplify_all();
    ir_mark_sqrt();
    ir_fold_branches();
    ir_dominators();
    ir_cse();
    ir_dce();
    uint64_t fp = ir_fingerprint();
    if (round > 0 && fp == prev)
      break;
    prev = fp;
  }
  if (ir_mem2reg()) {
    // 値にした読み出しの先をもう 1 回（ブロックの形は変わらないので、支配木はそのまま使える）
    ir_simplify_all();
    ir_dce();
  }
  ir_fold_addresses();
  ir_load_elim();
  ir_dce();
  ir_licm();
  ir_dce();
  if (ir_ccmp_chain()) {    // a && b の比べを ccmp で続ける（2026-11-27）
    ir_dominators();
    ir_dce();
  }
  if (ir_if_convert()) {    // 分岐を csel に（2026-11-26）
    ir_dominators();
    ir_dce();
  }
}

//
// ループの中で変わらない計算をループの外に出す（LICM。2026-11-15）
//
// 戻る辺（支配する頭への辺）から自然ループを見つけ、外から入る前任が 1 つでそこからの行き先が頭だけ
// （プリヘッダー）のとき、ループの中の副作用がなく止まらない計算のうち、入力がすべてループの外で
// 決まるものを、プリヘッダーの最後（分岐の前）に移す。内側のループから順に行うので、外のループの
// 外まで出ていく。割り算（0 で割ると止まる）・浮動小数点の演算（丸めモード）・ロード・比較（分岐と
// まとめる）は動かさない。レジスタが足りなくならないよう、1 つのループで動かすのは IR_LICM_MAX 個まで
// （即値に入る定数は数えない）。実機で、配列の和のループが 2 倍速くなった（大域の配列の番地の計算が
// ループの中で毎回くり返されていた）。

#define IR_LICM_MAX 6

static bool ir_licm_op(IrInst *in) {
  switch (in->op) {
  case IR_CONST: case IR_FCONST: case IR_GADDR: case IR_LADDR: case IR_TP: case IR_TLSADDR:
  case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
  case IR_SHL: case IR_SHR: case IR_SAR: case IR_NEG: case IR_NOT: case IR_EXT:
    return !in->fp;
  default:
    return false;
  }
}

typedef struct {
  IrBlock *head;
  bool *in;        // ブロックの番号 → ループの中か
  int size;
} IrLoopInfo;

static bool ir_licm_outside(IrInst *v, bool *in_loop) {
  return !v || !in_loop[v->bb->id];
}

// ループの中の読み出し ld を外に出せるか（2026-11-16）: 番地がループの中で変わらず、ループの中に重なり
// うる書き込みがなく、外で読んでも止まらない（必ず読める番地: 局所変数・大域変数・TLS、またはループを出る
// までに必ず通るブロックの読み出し）。前は読み出しを動かさず、ループの中で大域変数・ポインタのメンバーを
// 毎回読み直していた
static bool ir_licm_load(IrInst *ld, IrLoopInfo *L, IrBlock *bb, IrInst **stores, int nst,
                         IrBlock **exits, int nexit) {
  if (ld->vol || ld->acq || !ir_licm_outside(ld->a, L->in) || !ir_licm_outside(ld->c, L->in))
    return false;
  for (int i = 0; i < nst; i++)
    if (ir_may_alias(stores[i], ld))
      return false;
  if (ld->fp_base)
    return true;
  IrInst *a = ld->a;
  if (!ld->c && a && (a->op == IR_LADDR || a->op == IR_TLSADDR || (a->op == IR_GADDR && !a->sym->is_weak)))
    return true;
  for (int i = 0; i < nexit; i++)
    if (!ir_dominates(bb, exits[i]))
      return false;
  return nexit > 0;
}

static void ir_licm(void) {
  ir_dominators();
  IrLoopInfo *loops = ir_alloc(sizeof(IrLoopInfo) * (ir_nrpo + 1));
  int nloops = 0;
  IrBlock **work = ir_alloc(sizeof(IrBlock *) * (ir_nblocks + 1));
  for (int i = 0; i < ir_nrpo; i++) {
    IrBlock *u = ir_rpo[i];
    for (int k = 0; k < u->nsucc; k++) {
      IrBlock *h = u->succ[k];
      if (!h->idom || !ir_dominates(h, u))
        continue;
      // 同じ頭のループはまとめる
      IrLoopInfo *L = NULL;
      for (int j = 0; j < nloops; j++)
        if (loops[j].head == h)
          L = &loops[j];
      if (!L) {
        L = &loops[nloops++];
        L->head = h;
        L->in = ir_alloc(ir_nblocks + ir_nrpo + 1); // プリヘッダーを作ると番号が増える
        L->in[h->id] = true;
        L->size = 1;
      }
      int nw = 0;
      if (!L->in[u->id]) {
        L->in[u->id] = true;
        L->size++;
        work[nw++] = u;
      }
      while (nw) {
        IrBlock *b = work[--nw];
        for (int q = 0; q < b->npreds; q++) {
          IrBlock *pb = b->preds[q];
          if (!pb->idom || L->in[pb->id])
            continue;
          L->in[pb->id] = true;
          L->size++;
          work[nw++] = pb;
        }
      }
    }
  }
  // 内側（小さい）ループから
  for (int i = 1; i < nloops; i++)
    for (int j = i; j > 0 && loops[j].size < loops[j - 1].size; j--) {
      IrLoopInfo t = loops[j];
      loops[j] = loops[j - 1];
      loops[j - 1] = t;
    }
  for (int li = 0; li < nloops; li++) {
    IrLoopInfo *L = &loops[li];
    IrBlock *pre = NULL;
    int outside = 0;
    for (int q = 0; q < L->head->npreds; q++) {
      IrBlock *pb = L->head->preds[q];
      if (!L->in[pb->id]) {
        pre = pb;
        outside++;
      }
    }
    if (outside != 1 || !pre->idom || !pre->last)
      continue;
    if (pre->nsucc != 1 || pre->last->op != IR_BR) {
      // 前任が条件分岐（do-while の前の if など）: 辺の間にプリヘッダーを作る（phi の引数の並びは
      // 頭の前任の並びと同じなので、前任を差し替えるだけでよい）
      if (pre->last->op != IR_CBR)
        continue;
      IrBlock *p = ir_new_block();
      p->reachable = p->sealed = true;
      IrInst *br = ir_new(IR_BR);
      ir_append(p, br);
      p->succ[0] = L->head;
      p->nsucc = 1;
      ir_add_pred(p, pre);
      for (int k = 0; k < pre->nsucc; k++)
        if (pre->succ[k] == L->head)
          pre->succ[k] = p;
      for (int q = 0; q < L->head->npreds; q++)
        if (L->head->preds[q] == pre)
          L->head->preds[q] = p;
      p->idom = pre;
      pre = p;
    }
    // 関数の呼び出しを含むループは外に出さない（呼び出しをまたぐ値は退避が要るレジスタにしか置けず、
    // 足りなくなるとスタックに逃がして、かえって遅くなる。呼び出しの時間のほうが大きい）
    bool has_call = false;
    for (int i = 0; i < ir_nrpo && !has_call; i++)
      if (L->in[ir_rpo[i]->id])
        for (IrInst *in = ir_rpo[i]->first; in; in = in->next)
          if (in->op == IR_CALL || in->op == IR_MEMCPY || in->op == IR_MEMZERO || in->op == IR_ASM)
            has_call = true;
    if (has_call)
      continue;
    // 読み出しを外に出すための下調べ（2026-11-16）: ループの中の書き込みと、出口（外へ出る辺を持つブロック）。
    // 書き込みを伴うほかの命令（アトミック、errno を書く sqrt、インラインアセンブリの出力）があれば読み出しは
    // 動かさない
    int nst = 0, nexit = 0;
    bool loads_ok = true;
    for (int i = 0; i < ir_nrpo; i++) {
      IrBlock *b = ir_rpo[i];
      if (!L->in[b->id])
        continue;
      for (int k = 0; k < b->nsucc; k++)
        if (!L->in[b->succ[k]->id]) {
          nexit++;
          break;
        }
      for (IrInst *in = b->first; in; in = in->next) {
        if (in->op == IR_STORE)
          nst++;
        else if (in->op == IR_CAS || in->op == IR_XCHG || in->op == IR_ASMOUT ||
                 (in->op == IR_FSQRT && !in->sqrt_nonneg))
          loads_ok = false;
      }
    }
    IrInst **stores = ir_alloc(sizeof(IrInst *) * (nst + 1));
    IrBlock **exits = ir_alloc(sizeof(IrBlock *) * (nexit + 1));
    nst = nexit = 0;
    for (int i = 0; i < ir_nrpo; i++) {
      IrBlock *b = ir_rpo[i];
      if (!L->in[b->id])
        continue;
      for (int k = 0; k < b->nsucc; k++)
        if (!L->in[b->succ[k]->id]) {
          exits[nexit++] = b;
          break;
        }
      for (IrInst *in = b->first; in; in = in->next)
        if (in->op == IR_STORE)
          stores[nst++] = in;
    }
    int moved = 0;
    for (int i = 0; i < ir_nrpo && moved < IR_LICM_MAX; i++) {
      IrBlock *bb = ir_rpo[i];
      if (!L->in[bb->id])
        continue;
      for (IrInst *in = bb->first, *next; in && moved < IR_LICM_MAX; in = next) {
        next = in->next;
        if (in->repl)
          continue;
        if (in->op == IR_LOAD) {
          if (!loads_ok || !ir_licm_load(in, L, bb, stores, nst, exits, nexit))
            continue;
        } else if (!ir_licm_op(in) || !ir_licm_outside(in->a, L->in) || !ir_licm_outside(in->b, L->in)) {
          continue;
        }
        ir_remove(in);
        ir_insert_before(pre->last, in);
        if (in->op != IR_CONST)
          moved++;
      }
    }
  }
}

//
// SSA を抜ける: phi を、前任の最後のコピーにする
//

static IrInst *ir_copy_to(IrInst *dst, IrInst *src) {
  IrInst *in = ir_new(IR_COPY);
  in->a = src;
  in->b = dst; // b は書き込む先（コピーは自分の値を作らない）
  return in;
}

static void ir_out_of_ssa(void) {
  int nblocks = ir_nblocks;
  for (int b = 0; b < nblocks; b++) {
    IrBlock *bb = ir_blocks[b];
    if (!bb->reachable || !bb->first || bb->first->op != IR_PHI)
      continue;
    for (int k = 0; k < bb->npreds; k++) {
      IrBlock *pred = bb->preds[k];
      // 重要な辺（前任が分岐する）は、間にブロックを入れる
      if (pred->last->op == IR_INDBR || pred->last->op == IR_SWTAB)
        ir_unsupported();   // 番地で飛ぶので間に入れられない（入口のブロックは前任が 1 つなので来ないはず）
      if (pred->nsucc > 1) {
        IrBlock *mid = ir_new_block();
        mid->reachable = true;
        for (int i = 0; i < pred->nsucc; i++)
          if (pred->succ[i] == bb)
            pred->succ[i] = mid;
        IrInst *br = ir_new(IR_BR);
        ir_append(mid, br);
        mid->succ[0] = bb;
        mid->nsucc = 1;
        ir_add_pred(mid, pred);
        bb->preds[k] = mid;
        pred = mid;
      }
      // 並列コピー（すべての phi に同時に代入する）を、順番のあるコピーに並べる。
      // ほかのコピーの元になっている phi には、そのコピーが終わるまで書かない。輪になったら一時的な値に移す
      IrInst *term = pred->last;
      IrInst *dst[256], *src[256];
      int n = 0;
      for (IrInst *phi = bb->first; phi && phi->op == IR_PHI; phi = phi->next) {
        if (phi->args[k] == phi)
          continue;
        if (n == 256)
          ir_unsupported();
        dst[n] = phi;
        src[n] = phi->args[k];
        n++;
      }
      bool done[256] = {0};
      for (int left = n; left > 0;) {
        bool progress = false;
        for (int i = 0; i < n; i++) {
          if (done[i])
            continue;
          bool blocked = false;
          for (int j = 0; j < n && !blocked; j++)
            if (!done[j] && j != i && src[j] == dst[i])
              blocked = true;
          if (blocked)
            continue;
          ir_insert_before(term, ir_copy_to(dst[i], src[i]));
          if (!dst[i]->hint)
            dst[i]->hint = src[i];
          if (!src[i]->hint)
            src[i]->hint = dst[i];
          done[i] = true;
          left--;
          progress = true;
        }
        if (!progress) {
          int i = 0;
          while (done[i])
            i++;
          IrInst *t = ir_new(IR_COPY);
          t->a = dst[i];
          ir_insert_before(term, t);
          for (int j = 0; j < n; j++)
            if (!done[j] && src[j] == dst[i])
              src[j] = t;
        }
      }
    }
  }
}

//
// 生存区間と線形走査
//

static bool ir_defines(IrInst *in) {
  switch (in->op) {
  case IR_STORE:
  case IR_BR:
  case IR_CBR:
  case IR_INDBR:
  case IR_SWTAB:
  case IR_RET:
  case IR_UNREACH:
  case IR_PHI:
  case IR_MEMZERO:
  case IR_MEMCPY:
  case IR_PARAM_STRUCT:
  case IR_ASM:
    return false;
  case IR_COPY:
    return !in->b; // 書き込む先があるコピーは、その先を定義する
  case IR_CALL:
    return in->size != 0;
  default:
    return true;
  }
}

#define IR_SET_WORDS(n) (((n) + 63) / 64)

static void ir_set_bit(uint64_t *s, int i) { s[i / 64] |= 1ULL << (i % 64); }
static bool ir_get_bit(uint64_t *s, int i) { return (s[i / 64] >> (i % 64)) & 1; }

TLS static IrInst **ir_vals; // 番号 → 値
TLS static IrBlock **ir_order;
TLS static int ir_norder;
TLS static bool *ir_fused; // 番号 → 条件分岐とまとめた比較

// AArch64 のターゲットの記述（レジスタの組・呼び出し規約・即値。2026-11-27 に分けた）
#include "ir_a64.inc"

// 値にレジスタが要るか（使われない値、条件分岐とまとめた比較、即値として埋め込んだ定数、レジスタなしで使える
// 定数（ir_target_free_const）は要らない）
static bool ir_needs_reg(IrInst *v) {
  return v && v->uses > 0 && !ir_fused[v->id] && !ir_target_free_const(v);
}

static void ir_dfs_order(IrBlock *bb, bool *seen) {
  seen[bb->id] = true;
  for (int i = bb->nsucc - 1; i >= 0; i--)
    if (!seen[bb->succ[i]->id])
      ir_dfs_order(bb->succ[i], seen);
  ir_order[ir_norder++] = bb; // 帰りがけ順（あとで逆にする）
}

// 値が使われる・定義される場所を、ブロックの集合 use と def に記録する
static void ir_block_use_def(IrBlock *bb, uint64_t *use, uint64_t *def) {
  for (IrInst *in = bb->first; in; in = in->next) {
    IrInst *ops[3] = {in->a, in->op == IR_COPY ? NULL : in->b, NULL};
    for (int i = 0; i < 2; i++) {
      IrInst *v = ops[i];
      if (ir_needs_reg(v) && !ir_get_bit(def, v->id))
        ir_set_bit(use, v->id);
    }
    if (in->c && ir_needs_reg(in->c) && !ir_get_bit(def, in->c->id))
      ir_set_bit(use, in->c->id);
    if (in->op != IR_PHI)
      for (int i = 0; i < in->nargs; i++)
        if (ir_needs_reg(in->args[i]) && !ir_get_bit(def, in->args[i]->id))
          ir_set_bit(use, in->args[i]->id);
    if (in->op == IR_COPY && in->b)
      ir_set_bit(def, in->b->id);
    else if (ir_defines(in))
      ir_set_bit(def, in->id);
  }
}

// いちばん下の 1 のビットの位置（m は 0 でない。de Bruijn の数を掛けて表を引く。slimcc には __builtin_ctzll がない）
static inline int ir_ctz(uint64_t m) {
  static const uint8_t tab[64] = {0, 1, 48, 2, 57, 49, 28, 3, 61, 58, 50, 42, 38, 29, 17, 4, 62, 55, 59, 36, 53, 51, 43, 22, 45, 39, 33, 30, 24, 18, 12, 5, 63, 47, 56, 27, 60, 41, 37, 16, 54, 35, 52, 21, 44, 32, 23, 11, 46, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19, 9, 13, 8, 7, 6};
  return tab[((m & -m) * 0x03f79d71b4cb0a89ULL) >> 58];
}

// ブロックをまたいで生きうる値（どこかのブロックの use に入るもの）だけに、値の番号の順に詰めた番号を付け、
// 生存の集合はその幅で持つ（ir_live_ids[詰めた番号] = 値の番号）。集合に入るのは use に入る値だけなので
// （live_in = use ∪ (live_out − def)、live_out = 後ろの live_in の和）、結果は値の番号の幅で持つときと同じ。
// 前はブロックごとに 4 つの集合を値の数の幅で持ち、vim の nfa_emit_equi_class（10,540 ブロック、47,278 個の値）で
// 240MB を使い、Monocs のヒープが尽きた（2026-11-20）
TLS static int *ir_live_ids;
TLS static int ir_nlive;

static void ir_liveness(void) {
  int words = IR_SET_WORDS(ir_nvals);
  uint64_t *u = ir_alloc(8 * words), *d = ir_alloc(8 * words);
  int *map = ir_alloc(sizeof(int) * (ir_nvals + 1));
  // 1 回目: use に入る値を選ぶ
  for (int i = 0; i < ir_norder; i++) {
    memset(u, 0, 8 * words);
    memset(d, 0, 8 * words);
    ir_block_use_def(ir_order[i], u, d);
    for (int w = 0; w < words; w++)
      for (uint64_t m = u[w]; m; m &= m - 1)
        map[w * 64 + ir_ctz(m)] = 1;
  }
  ir_live_ids = ir_alloc(sizeof(int) * (ir_nvals + 1));
  ir_nlive = 0;
  for (int v = 0; v < ir_nvals; v++)
    map[v] = map[v] ? (ir_live_ids[ir_nlive] = v, ir_nlive++) : -1;
  // 2 回目: 詰めた番号の集合を作る
  int cw = IR_SET_WORDS(ir_nlive);
  uint64_t **use = ir_alloc(sizeof(uint64_t *) * ir_nblocks);
  uint64_t **def = ir_alloc(sizeof(uint64_t *) * ir_nblocks);
  for (int i = 0; i < ir_norder; i++) {
    IrBlock *bb = ir_order[i];
    use[bb->id] = ir_alloc(8 * cw);
    def[bb->id] = ir_alloc(8 * cw);
    bb->live_in = ir_alloc(8 * cw);
    bb->live_out = ir_alloc(8 * cw);
    memset(u, 0, 8 * words);
    memset(d, 0, 8 * words);
    ir_block_use_def(bb, u, d);
    for (int w = 0; w < words; w++) {
      for (uint64_t m = u[w]; m; m &= m - 1)
        ir_set_bit(use[bb->id], map[w * 64 + ir_ctz(m)]);
      for (uint64_t m = d[w]; m; m &= m - 1) {
        int c = map[w * 64 + ir_ctz(m)];
        if (c >= 0)
          ir_set_bit(def[bb->id], c);
      }
    }
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (int i = ir_norder - 1; i >= 0; i--) {
      IrBlock *bb = ir_order[i];
      for (int w = 0; w < cw; w++) {
        uint64_t out = 0;
        for (int s = 0; s < bb->nsucc; s++)
          out |= bb->succ[s]->live_in[w];
        uint64_t in = use[bb->id][w] | (out & ~def[bb->id][w]);
        if (out != bb->live_out[w] || in != bb->live_in[w]) {
          bb->live_out[w] = out;
          bb->live_in[w] = in;
          changed = true;
        }
      }
    }
  }
}


// 作る間（ir_intervals）は区間を逆の順（いちばん前の区間が最後）に持ち、最後に並べ直す（2026-11-16。
// 前は先頭に足すたびに全体をずらしていた）
static void ir_add_range(IrInst *v, int from, int to) {
  // 後ろから作るので、先頭に足すか、先頭とつなげる
  if (v->nrng) {
    int *f = v->rng + 2 * (v->nrng - 1);
    if (to >= f[0]) {
      if (from < f[0])
        f[0] = from;
      if (to > f[1])
        f[1] = to;
      return;
    }
  }
  v->rng = ir_grow(v->rng, &v->caprng, (v->nrng + 1) * 2, sizeof(int));
  v->rng[2 * v->nrng] = from;
  v->rng[2 * v->nrng + 1] = to;
  v->nrng++;
}

static void ir_set_from(IrInst *v, int pos) {
  if (!v->nrng) {
    ir_add_range(v, pos, pos + 1); // 定義だけで使われない
    return;
  }
  v->rng[2 * (v->nrng - 1)] = pos;
}

// 線形化した位置と生存区間（Wimmer and Franz, "Linear Scan Register Allocation on SSA Form" の BuildIntervals）
static void ir_intervals(void) {
  for (int i = 0; i < ir_nvals; i++)
    if (ir_vals[i]) {
      ir_vals[i]->nrng = 0;
      ir_vals[i]->cross_call = false;
    }
  int pos = 0;
  for (int i = 0; i < ir_norder; i++) {
    IrBlock *bb = ir_order[i];
    bb->start = pos;
    for (IrInst *in = bb->first; in; in = in->next) {
      pos += 2;
      in->pos = pos;
    }
    pos += 2;
    bb->end = pos;
  }
  int words = IR_SET_WORDS(ir_nlive);    // live_out は詰めた番号（ir_liveness）
  for (int i = ir_norder - 1; i >= 0; i--) {
    IrBlock *bb = ir_order[i];
    for (int w = 0; w < words; w++)
      for (uint64_t m = bb->live_out[w]; m; m &= m - 1) {
        int id = ir_live_ids[w * 64 + ir_ctz(m)];
        if (id < ir_nvals && ir_vals[id])
          ir_add_range(ir_vals[id], bb->start, bb->end);
      }
    for (IrInst *in = bb->last; in; in = in->prev) {
      if (in->op == IR_COPY && in->b) {
        if (ir_needs_reg(in->b) || in->b->op == IR_PHI)
          ir_set_from(in->b, in->pos);
      } else if (ir_defines(in) && ir_needs_reg(in)) {
        ir_set_from(in, in->pos);
      }
      if (in->op == IR_PHI)
        continue;
      IrInst *ops[3] = {in->a, in->op == IR_COPY ? NULL : in->b, in->c};
      for (int j = 0; j < 3; j++)
        if (ir_needs_reg(ops[j]))
          ir_add_range(ops[j], bb->start, in->pos);
      for (int j = 0; j < in->nargs; j++)
        if (ir_needs_reg(in->args[j]))
          ir_add_range(in->args[j], bb->start, in->pos);
    }
  }
  // 区間を前から順に並べ直す
  for (int i = 0; i < ir_nvals; i++) {
    IrInst *v = ir_vals[i];
    if (!v)
      continue;
    for (int a = 0, b = v->nrng - 1; a < b; a++, b--) {
      int f = v->rng[2 * a], t = v->rng[2 * a + 1];
      v->rng[2 * a] = v->rng[2 * b];
      v->rng[2 * a + 1] = v->rng[2 * b + 1];
      v->rng[2 * b] = f;
      v->rng[2 * b + 1] = t;
    }
  }
  // 関数の呼び出しをまたぐか（呼び出しの前から後まで生きている）。呼び出しの位置を並べて二分探索する
  int ncalls = 0;
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      if (in->op == IR_CALL || (in->op == IR_FSQRT && !in->sqrt_nonneg))
        ncalls++;
  if (ncalls) {
    int *calls = ir_alloc(sizeof(int) * ncalls);
    int k = 0;
    for (int i = 0; i < ir_norder; i++)
      for (IrInst *in = ir_order[i]->first; in; in = in->next)
        if (in->op == IR_CALL || (in->op == IR_FSQRT && !in->sqrt_nonneg))
          calls[k++] = in->pos; // 位置は昇順に並んでいる
    for (int v = 0; v < ir_nvals; v++) {
      IrInst *x = ir_vals[v];
      if (!x)
        continue;
      for (int r = 0; r < x->nrng && !x->cross_call; r++) {
        int from = x->rng[2 * r], to = x->rng[2 * r + 1];
        // from より大きい最初の呼び出しが to より前にあるか
        int lo = 0, hi = ncalls;
        while (lo < hi) {
          int mid = (lo + hi) / 2;
          if (calls[mid] <= from)
            lo = mid + 1;
          else
            hi = mid;
        }
        if (lo < ncalls && calls[lo] < to)
          x->cross_call = true;
      }
    }
  }
  for (int i = 0; i < ir_nvals; i++) {
    IrInst *v = ir_vals[i];
    if (v && v->nrng) {
      v->start = v->rng[0];
      v->end = v->rng[2 * v->nrng - 1];
    }
  }
}

// pos は呼ぶたびに同じか後ろ（ir_linear_scan の位置）。終わった区間は rcur で飛ばす（2026-11-16）
static bool ir_covers(IrInst *v, int pos) {
  int r = v->rcur;
  while (r < v->nrng && v->rng[2 * r + 1] <= pos)
    r++;
  v->rcur = r;
  for (; r < v->nrng; r++)
    if (v->rng[2 * r] <= pos && pos < v->rng[2 * r + 1])
      return true;
  return false;
}

// cur と v が重なる最初の位置（重ならなければ -1）。v の終わった区間（cur の始まりより前）は飛ばす
static int ir_next_intersection(IrInst *v, IrInst *cur) {
  int i = v->rcur, j = 0;
  while (i < v->nrng && j < cur->nrng) {
    int a0 = v->rng[2 * i], a1 = v->rng[2 * i + 1];
    int b0 = cur->rng[2 * j], b1 = cur->rng[2 * j + 1];
    int lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
    if (lo < hi)
      return lo;
    if (a1 <= b1)
      i++;
    else
      j++;
  }
  return -1;
}

static int ir_cmp_start(const void *x, const void *y) {
  IrInst *a = *(IrInst **)x, *b = *(IrInst **)y;
  if (a->start != b->start)
    return a->start < b->start ? -1 : 1;
  return a->id < b->id ? -1 : a->id > b->id;
}

// 線形走査（Wimmer and Franz。区間を分けず、逃がすときは区間全体）
static bool *ir_hot_blocks(void);

#define IR_REUSE_GAP 24  // 空いてからこれだけの位置（およそ 12 命令）たったレジスタは、すぐ使ってよい

// a を逃がすほうが b より安いか: 重み ÷ 生きる長さ が小さいほう（同じなら遠くまで生きるほう）
static bool ir_spill_cheaper(IrInst *a, IrInst *b, int64_t *wt) {
  int64_t la = a->end - a->start + 1, lb = b->end - b->start + 1;
  int64_t x = wt[a->id] * lb, y = wt[b->id] * la;
  if (x != y)
    return x < y;
  return a->end > b->end;
}

static void ir_linear_scan(void) {
  IrInst **list = ir_alloc(sizeof(IrInst *) * (ir_nvals + 1));
  int n = 0;
  for (int i = 0; i < ir_nvals; i++) {
    IrInst *v = ir_vals[i];
    if (v && v->nrng && (ir_needs_reg(v) || v->op == IR_PHI)) {
      v->rcur = 0;
      list[n++] = v;
    }
  }
  qsort(list, n, sizeof(IrInst *), ir_cmp_start);

  IrInst **active = ir_alloc(sizeof(IrInst *) * (n + 1));
  IrInst **inactive = ir_alloc(sizeof(IrInst *) * (n + 1));
  int nactive = 0, ninactive = 0;
  int last_end[64];   // レジスタごとの、最後に割り当てた値の終わり
  for (int r = 0; r < 64; r++)
    last_end[r] = -1;
  bool has_loop = opt_sched && ir_hot_blocks();
  // 逃がす値を選ぶ重み（2026-11-21）: 定義と使う所の数（ループの中は 8 倍）。重みが生きる長さのわりに小さい値
  // から逃がす（前は「いちばん遠くまで生きる値」で、QuickJS の JS_CallInternal では関数全体で使う pc などの、
  // いちばんよく使う値が先に逃がされていた）
  int64_t *wt = ir_alloc(sizeof(int64_t) * (ir_nvals + 1));
  {
    bool *hotb = ir_hot_blocks();
    for (int i = 0; i < ir_norder; i++) {
      int64_t f = hotb && hotb[i] ? 8 : 1;
      for (IrInst *in = ir_order[i]->first; in; in = in->next) {
        IrInst *ops[3] = {in->a, in->b, in->c};
        if (in->id <= ir_nvals)
          wt[in->id] += f;
        for (int k = 0; k < 3; k++)
          if (ops[k] && ops[k]->id <= ir_nvals)
            wt[ops[k]->id] += f;
        for (int k = 0; k < in->nargs; k++)
          if (in->args[k] && in->args[k]->id <= ir_nvals)
            wt[in->args[k]->id] += f;
      }
    }
  }
  // x1〜x7 を使うとき: 引数（PARAM）は自分の入ってくるレジスタにだけ置ける。ほかの値は、引数を全部
  // 取り出した後に始まるものだけ（まだ読んでいない引数のレジスタを壊さない）
  bool no67;
  bool low = ir_low_ok(&no67);
  uint8_t *preg = low ? ir_fixed_hints() : NULL;
  int param_end = -1;
  if (low)
    for (int i = 0; i < n; i++)
      if (list[i]->op == IR_PARAM && list[i]->start > param_end)
        param_end = list[i]->start;
  // 下位 32 ビットだけを使う拡張（mov になる）は、元と同じレジスタなら何も出さないので、元をヒントにする
  // （2026-11-16。int の引数・値を別のレジスタに写す mov が多かった）
  for (int i = 0; i < n; i++)
    if (list[i]->op == IR_EXT && list[i]->low32 && list[i]->size == 4 && !list[i]->hint && !list[i]->a->fp)
      list[i]->hint = list[i]->a;
  bool *hinted = ir_alloc(ir_nvals + 1);  // ほかの値のヒント（コピーで同じレジスタにまとめたい相手）になっている
  for (int i = 0; i < n; i++) {
    IrInst *h = ir_resolve(list[i]->hint);
    if (h && h->id <= ir_nvals)
      hinted[h->id] = true;
  }
  for (int i = 0; i < n; i++) {
    IrInst *cur = list[i];
    int p = cur->start;
    for (int j = 0; j < nactive;) {
      IrInst *it = active[j];
      if (it->end <= p) {
        active[j] = active[--nactive];
      } else if (!ir_covers(it, p)) {
        inactive[ninactive++] = it;
        active[j] = active[--nactive];
      } else {
        j++;
      }
    }
    for (int j = 0; j < ninactive;) {
      IrInst *it = inactive[j];
      if (it->end <= p) {
        inactive[j] = inactive[--ninactive];
      } else if (ir_covers(it, p)) {
        active[nactive++] = it;
        inactive[j] = inactive[--ninactive];
      } else {
        j++;
      }
    }

    // 読むのは order のレジスタだけなので、その分だけ決める（2026-11-16。前は 64 個を 0 にしていた）
    int free_until[64];
    const int *order = cur->fp ? ir_falloc_order : low ? ir_alloc_order_low : ir_alloc_order;
    int norder = cur->fp ? IR_NFREGS : low ? IR_NREGS_LOW : IR_NREGS;
    for (int k = 0; k < norder; k++) {
      int r = order[k];
      free_until[r] = !cur->cross_call || ir_callee_tab[r] ? INT32_MAX : 0;
    }
    if (low && !cur->fp && no67)
      free_until[6] = free_until[7] = 0;
    if (low && !cur->fp) {
      if (cur->op == IR_PARAM) {
        for (int k = 0; k <= 7; k++)
          if (k != cur->imm)
            free_until[ir_low_reg(k)] = 0;
      } else if (cur->start <= param_end) {
        for (int k = 0; k <= 7; k++)
          free_until[ir_low_reg(k)] = 0;
      }
    }
    if (ir_has_alloca && !cur->fp)
      free_until[28] = 0;   // フレームの土台（ir_fmem。alloca で sp が動くので）
    for (int j = 0; j < nactive; j++)
      free_until[active[j]->reg] = 0;
    for (int j = 0; j < ninactive; j++) {
      int q = ir_next_intersection(inactive[j], cur);
      if (q >= 0 && q < free_until[inactive[j]->reg])
        free_until[inactive[j]->reg] = q;
    }

    int reg = 0;
    // 引数は、空いていれば入ってくるレジスタのまま使う（写す mov が要らない）
    if (low && cur->op == IR_PARAM && !cur->fp && cur->imm >= 0 && cur->imm <= 7 &&
        free_until[ir_low_reg((int)cur->imm)] >= cur->end)
      reg = ir_low_reg((int)cur->imm);
    // 呼び出しの引数・戻り値・return する値は、そのレジスタ（ir_fixed_hints。2026-11-27）
    if (!reg && low && !cur->fp && preg[cur->id] && free_until[preg[cur->id]] >= cur->end)
      reg = preg[cur->id];
    IrInst *h = reg ? NULL : ir_resolve(cur->hint);
    if (h && h->reg && ir_order_tab[h->reg] == (cur->fp ? 2 : 1) && (!ir_is_low_reg(h->reg) || low) &&
        free_until[h->reg] >= cur->end)
      reg = h->reg;
    // 空いているもののうち、最後に使い終わったのがいちばん前のもの（空いたばかりのレジスタをすぐ使うと、
    // 関係のない命令どうしが同じレジスタの書き換えの順で縛られ、命令の並べ替え（sched.inc）が効かない）。
    // 退避が要るレジスタ（x19〜x28・d8〜d15）をまだ使っていなければ、ループのある関数で、ほかに空いたばかり
    // のものしかないときだけ新しく使う（入口と出口の退避が 1 組増える）
    // コピーでまとめる値・広げる命令（元と同じレジスタなら何も出さない）は今までどおり
    if (!reg && opt_sched && !cur->hint && !hinted[cur->id] && cur->op != IR_EXT && cur->op != IR_COPY &&
        cur->op != IR_PHI) {
      int best = 0, fresh = 0;
      for (int k = 0; k < norder; k++) {
        int r = order[k];
        if (free_until[r] < cur->end)
          continue;
        if (ir_callee_tab[r] && !ir_reg_used[r]) {
          if (!fresh)
            fresh = r;
          continue;
        }
        if (!best || last_end[r] < last_end[best])
          best = r;
      }
      if (best && (last_end[best] < 0 || cur->start - last_end[best] >= IR_REUSE_GAP || !fresh || !has_loop))
        reg = best;
      else if (fresh && has_loop)
        reg = fresh;
    }
    for (int k = 0; k < norder && !reg; k++)
      if (free_until[order[k]] >= cur->end)
        reg = order[k];

    if (!reg) {
      // 空いているレジスタがない: 一番遅くまで使われる値（それが cur なら cur）を逃がす
      int victim = -1;
      for (int j = 0; j < nactive; j++) {
        int r = active[j]->reg;
        if (active[j]->fp != cur->fp || (cur->cross_call && !ir_is_callee_reg(r)))
          continue;
        bool blocked = false;
        for (int k = 0; k < ninactive && !blocked; k++)
          if (inactive[k]->reg == r && ir_next_intersection(inactive[k], cur) >= 0)
            blocked = true;
        if (blocked)
          continue;
        if (victim < 0 || ir_spill_cheaper(active[j], active[victim], wt))
          victim = j;
      }
      // 整数の定数は逃がさず、使う所で作り直す（ir_srcA が mov する。2026-11-21。前は QuickJS の JS_CallInternal で
      // 関数全体にまとめた定数がスタックに逃がされ、使うたびに読んでいた）。逃がす相手も定数を先に選ぶ
      if (cur->op == IR_CONST && !cur->fp)
        continue;
      int cvict = -1;
      for (int j = 0; j < nactive; j++)
        if (active[j]->op == IR_CONST && !active[j]->fp && active[j]->fp == cur->fp &&
            !(cur->cross_call && !ir_is_callee_reg(active[j]->reg))) {
          bool blocked = false;
          for (int k = 0; k < ninactive && !blocked; k++)
            if (inactive[k]->reg == active[j]->reg && ir_next_intersection(inactive[k], cur) >= 0)
              blocked = true;
          if (!blocked && (cvict < 0 || active[j]->end > active[cvict]->end))
            cvict = j;
        }
      if (cvict >= 0) {
        IrInst *v = active[cvict];
        reg = v->reg;
        v->reg = 0;             // 逃がす場所もなし（作り直す）
        active[cvict] = active[--nactive];
      } else if (victim >= 0 && ir_spill_cheaper(active[victim], cur, wt)) {
        IrInst *v = active[victim];
        reg = v->reg;
        v->reg = 0;
        v->spill = ir_new_spill();
        active[victim] = active[--nactive];
      } else {
        cur->spill = ir_new_spill();
        continue;
      }
    }
    cur->reg = reg;
    ir_reg_used[reg] = true;
    if (cur->end > last_end[reg])
      last_end[reg] = cur->end;
    active[nactive++] = cur;
  }
}

// 命令の並べ替え（sched.inc。-fno-schedule-insns2 でしない）は、ループの中のブロックだけにかける。
// 1 回しか通らないところは並べ替えても速くならず、コンパイルの時間だけかかる（全部にかけると、
// 実機でコンパイルが 1 割遅くなった）。ループは並び（逆後順）で前に戻る辺で見分ける（目安なので、
// 既約でない形を見誤っても、並べ替えるかどうかが変わるだけ）
static bool *ir_hot_blocks(void) {
  int *pos = ir_alloc(sizeof(int) * ir_nblocks);
  for (int i = 0; i < ir_norder; i++)
    pos[ir_order[i]->id] = i;
  int *diff = ir_alloc(sizeof(int) * (ir_norder + 1));
  bool any = false;
  for (int i = 0; i < ir_norder; i++) {
    IrBlock *bb = ir_order[i];
    for (int k = 0; k < bb->nsucc; k++) {
      int j = pos[bb->succ[k]->id];
      if (j <= i) {
        diff[j]++;
        diff[i + 1]--;
        any = true;
      }
    }
  }
  if (!any)
    return NULL;
  bool *hot = ir_alloc(ir_norder);
  int run = 0;
  for (int i = 0; i < ir_norder; i++) {
    run += diff[i];
    hot[i] = run > 0;
  }
  return hot;
}

//
// 出力（AArch64。2026-11-27 に分けた）
//
#include "ir_a64_emit.inc"

// 関数を新しいコード生成で出力する。扱えなければ何も出力せずに false
static bool ir_gen_fn_body(Obj *fn);
static void ir_rec_abort(void);

static bool ir_gen_fn(Obj *fn) {
  ir_cur_fn = fn->name;
  if (!opt_ir || opt_g || fn->is_naked || fn->ty->is_variadic || fn->dealloc_vla)
    return false;
  ir_nblocks = 0;
  ir_nvals = 0;
  ir_ndup = 0;
  ir_dup_rng = NULL;
  ir_capdup = 0;
  ir_nloops = 0;
  ir_nslots = 0;
  ir_frame = 0;
  ir_fn = fn;
  ir_null_val = NULL;
  ir_blocks = NULL;
  ir_capblocks = 0;
  ir_loops = NULL;
  ir_caploops = 0;
  ir_slots = NULL;
  ir_capslots = 0;
  ir_labels = NULL;
  ir_nlabels = ir_caplabels = 0;
  ir_ninlines = 0;
  ir_inlined = NULL;
  ir_ninlined = ir_capinlined = 0;
  memset(ir_reg_used, 0, sizeof(ir_reg_used));

  if (setjmp(ir_bail)) {
    ir_rec_abort();
    if (getenv("SLIMCC_IR_WHY"))
      fprintf(stderr, "ir: %s: line %d\n", fn->name, ir_bail_line);
    ir_free_all();
    return false;
  }
  return ir_gen_fn_body(fn);
}

// ir_gen_fn の本体（setjmp を呼ぶ関数は中間表現にならず今までのコード生成になるので、setjmp の外に分けた）
static bool ir_gen_fn_body(Obj *fn) {
  Type *rty = fn->ty->return_ty;
  if (rty->kind != TY_VOID && !ir_value_ty(rty) && !ir_struct_ty(rty))
    ir_unsupported();
  int gp = 0, fp = 0;
  calling_convention(fn->ty->param_list, &gp, &fp);
  ir_stk_params = false;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next) {
    if (p->is_zero_sized_arg || (!ir_value_ty(p->ty) && !ir_struct_ty(p->ty)))
      ir_unsupported();
    // スタックで渡されたもの（2026-11-21。前は関数ごと今までのコード生成だった）: 呼び出し元の sp（= x29 + 16）
    // からの stack_offset にある。値・構造体はその場所を変数の場所にする（AAPCS64 では呼ばれた側が書き換えて
    // よい）。参照渡しのポインタがスタックにあるときは、入口で読んでから写す（下）。フレームは省かない
    if (p->pass_by_stack || p->arg_reg < 0) {
      ir_stk_params = true;
      if (p->pass_by_stack)
        ir_slot_at(p, 16 + p->stack_offset);
    }
  }

  IrSet pinned = {0};
  ir_has_asm = false;
  ir_has_alloca = false;
  ir_has_labelval = false;
  ir_dispatch = NULL;
  ir_lbl_lists = NULL;
  ir_nlbl_lists = ir_caplbl_lists = 0;
  ir_scan_addr(fn->body, &pinned);
  // &&label・goto *p があれば、ラベルに .L.jmp の番号を付ける（今までのコード生成と同じ name_labels。関数の
  // 中の静的な表（QuickJS の dispatch_table）の &&label も .L.jmp.N を指す）
  ir_lbl_lo = ir_lbl_hi = 0;
  if (ir_has_labelval) {
    ir_lbl_lo = count();
    for (int i = 0; i < ir_nlbl_lists; i++)
      name_labels(ir_lbl_lists[i]);
    ir_lbl_hi = count();
  }
  if (ir_has_asm)
    ir_frame = 16; // [x29 - 16] に x19、[x29 - 8] に x20
  for (Obj *p = fn->ty->param_list; p; p = p->param_next)
    if (ir_struct_ty(p->ty))
      ir_set_add(&pinned, p);
  ir_collect_vars(fn->ty->scopes, &pinned);

  IrBlock *entry = ir_new_block();
  entry->sealed = true;
  ir_cur = entry;
  // まず引数のレジスタ（x0〜x7、v0〜v7、x8）をすべて受け取る（後のコピーなどで壊さないように）
  ir_rtn_ptr = NULL;
  if (ir_struct_ty(rty) && is_mem_rtn(rty)) {
    ir_rtn_ptr = ir_new(IR_PARAM);
    ir_rtn_ptr->imm = 8;
    ir_emit(ir_rtn_ptr);
  }
  int nparams = 0;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next)
    nparams++;
  IrInst **pv = ir_alloc(sizeof(IrInst *) * (nparams + 1));
  IrInst **pv2 = ir_alloc(sizeof(IrInst *) * (nparams + 1));   // 語に分けた構造体の 2 語目
  int i = 0;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next, i++) {
    if (p->pass_by_stack || p->arg_reg < 0)
      continue;     // スタックのもの（下の 2 つ目の繰り返しで）
    if (ir_word_reg_struct(p->ty) && !p->pass_by_ref) {
      // 語に分けて受け取り、2 つ目の繰り返しで局所変数の場所に書く（SROA。2026-11-21）
      for (int k = 0; k * 8 < p->ty->size; k++) {
        IrInst *in = ir_new(IR_PARAM);
        in->imm = p->arg_reg + k;
        ir_set_class(in, ty_ulong);
        ir_emit(in);
        if (k == 0)
          pv[i] = in;
        else
          pv2[i] = in;
      }
      continue;
    }
    if (ir_struct_ty(p->ty) && !p->pass_by_ref) {
      // レジスタで渡された構造体は、その場でメモリに書く（使うのは x16・x17 だけ）
      ArgInfo ai = classify(p->ty);
      if (ai.cls != AC_GP && ai.cls != AC_HFA)
        ir_unsupported();
      IrInst *in = ir_new(IR_PARAM_STRUCT);
      in->imm = p->arg_reg;
      in->ty = p->ty;
      in->rtn_slot = ir_slot(p);
      ir_emit(in);
      continue;
    }
    IrInst *in = ir_new(IR_PARAM);
    in->imm = p->arg_reg;
    if (!p->pass_by_ref)
      ir_set_class(in, p->ty);
    ir_emit(in);
    pv[i] = in;
  }
  i = 0;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next, i++) {
    if (p->pass_by_ref && p->arg_reg < 0)
      pv[i] = ir_load(ir_laddr(16 + p->stack_offset), ty_ulong);   // スタックにある、コピーのアドレス
    if (p->pass_by_stack) {
      // スタックの場所がそのまま変数の場所（ir_slot_at）。SSA の変数なら読んで値にする
      if (!ir_struct_ty(p->ty) && ir_is_ssa_var(p))
        ir_write_var(p, ir_cur, ir_normalize(ir_load(ir_laddr(ir_slot(p)), p->ty), p->ty));
      continue;
    }
    if (ir_word_reg_struct(p->ty) && !p->pass_by_ref) {
      IrInst *addr = ir_laddr(ir_slot(p));
      ir_store_word(addr, 0, pv[i]);
      if (pv2[i])
        ir_store_word(addr, 8, pv2[i]);
      continue;
    }
    if (ir_struct_ty(p->ty)) {
      if (p->pass_by_ref)
        ir_memcpy(ir_laddr(ir_slot(p)), pv[i], p->ty->size); // 呼び出し元のコピーから写す
      continue;
    }
    IrInst *v = ir_normalize(pv[i], p->ty);
    if (ir_is_ssa_var(p)) {
      ir_write_var(p, ir_cur, v);
    } else {
      IrInst *addr = ir_new(IR_LADDR);
      addr->imm = ir_slot(p);
      ir_emit(addr);
      ir_store(addr, v, p->ty);
    }
  }
  ir_stmt(fn->body);
  if (ir_has_labelval)
    ir_finish_dispatch();
  for (int i = 0; i < ir_nlabels; i++)
    ir_seal(ir_labels[i].bb);
  if (!ir_is_term(ir_cur->last)) {
    IrInst *ret = ir_new(IR_RET);
    if (!strcmp(fn->name, "main") && rty->kind != TY_VOID)
      ret->a = ir_const(0);
    ir_emit(ret);
  }

  ir_cleanup();
  ir_optimize();
  ir_mark_low32();
  ir_out_of_ssa();
  ir_count_uses();

  // ブロックの並び（深さ優先の帰りがけ順の逆）
  ir_order = ir_alloc(sizeof(IrBlock *) * ir_nblocks);
  ir_norder = 0;
  bool *seen = ir_alloc(ir_nblocks);
  ir_dfs_order(ir_blocks[0], seen);
  for (int i = 0; i < ir_norder / 2; i++) {
    IrBlock *t = ir_order[i];
    ir_order[i] = ir_order[ir_norder - 1 - i];
    ir_order[ir_norder - 1 - i] = t;
  }

  ir_vals = ir_alloc(sizeof(IrInst *) * (ir_nvals + 1));
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      ir_vals[in->id] = in;
  ir_fused = ir_alloc(ir_nvals + 1);
  ir_mark_fused();
  // 即値としてだけ使う定数は、値を作らない
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      if (in->b && ir_imm_operand(in, in->b))
        in->b->uses--;
  // 呼び出しの引数の整数の定数は、x0〜x7（スタックなら x16）に直接作る（ir_arg_const。2026-11-27。前は
  // ほかのレジスタに作ってから mov していた）。引数にだけ使う定数には、レジスタを割り当てない
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      if (in->op == IR_CALL)
        for (int k = 0; k < in->nargs; k++)
          if (ir_arg_const(in, k))
            in->args[k]->uses--;

  ir_liveness();
  ir_intervals();
  ir_linear_scan();

  // インライン展開してよいか、大きさ（最適化した後の命令の数。ループの反転・展開で 2 か所目以降に出した
  // ものは数えない（ir_is_dup））
  int size = 0;
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      if (in->op != IR_PHI && in->op != IR_PARAM && in->op != IR_COPY && !ir_is_dup(in->id))
        size++;
  fn->ir_size = size;
  // alloca は呼び出し元に残るので展開しない。構造体の引数・戻り値（16 バイトまでのレジスタで返すもの）は
  // 写して展開する（2026-11-21。QuickJS の JSValue を受け渡しする JS_DupValue など）
  bool inl = !ir_has_asm && !ir_has_alloca && !ir_has_labelval && !ir_rtn_ptr;
  for (Obj *p = fn->ty->param_list; p; p = p->param_next)
    if (!ir_value_ty(p->ty) && !ir_struct_ty(p->ty))
      inl = false;
  fn->ir_inlinable = inl;
  if (inl && size <= IR_INLINE_SIZE_ALWAYS)
    ast_arena_keep = true; // この関数の AST を残す（後の関数で展開する）

  ir_label_base = count();
  codegen_fn = fn; // get_symbol が参照した記号を記録する（static の関数や文字列リテラルを出力するため）
  // 文字列リテラルなどの static の局所変数を持つ関数を展開したときは、その関数も参照したことにする
  // （それらはその関数と一緒に出力されるため。持たない関数は、ほかから呼ばれなければ出力しない）
  for (int i = 0; i < ir_ninlined; i++)
    if (ir_inlined[i]->static_lvars)
      get_symbol(ir_inlined[i]);
  ir_emit_fn(fn);
  ir_free_all();
  return true;
}
