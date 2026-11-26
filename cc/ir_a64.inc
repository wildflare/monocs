// slimcc-a64: 中間表現のコード生成の、AArch64 のターゲットの記述（ir.inc が取り込む。2026-11-27 に ir.inc から分けた）
//
// ここにあるもの: 値に割り当てるレジスタの組と順、呼び出し規約（AAPCS64）で決まったレジスタで受け渡す値、
// 命令に埋め込める即値、レジスタなしで使える定数。命令を出すところは ir_a64_emit.inc、命令の並べ替えの
// Cortex-A53 の模型は sched.inc。中間表現を作る・最適化する部分に残っている AArch64 の前提は、
// docs/slimcc-codegen.md の「ターゲットに依存するところ」にまとめてある（別の CPU を足すときの手がかり）。
//
// レジスタ（値のレジスタの番号）
//   x9〜x15    値（関数の呼び出しをまたがないもの）
//   x19〜x28   値（呼び出しをまたぐもの、またがないもの）。使ったものは入口で退避する（ir_save_regs）
//   x0〜x7     引数・戻り値。x0〜x7 を作業用に使う命令のない関数では値にも（ir_low_ok。呼び出しをまたがないもの）。
//              x0 は番号 0 が「なし」なので IR_X0（63）で表し、出力のときに 0 にする（ir_phys）
//   x16, x17   作業用（スタックに逃がした値の出し入れ、アドレス計算、並列の写しの輪を切る）
//   x8         構造体の戻り値の領域・割り算の余りの作業用
//   x29, x30   フレームポインタ、リンクレジスタ
//   浮動小数点は 32 + 番号（v16〜v29 は呼び出し元保存、v8〜v15 は呼び出し先保存（下位 64 ビット）、
//   v0〜v7 は引数・戻り値、v30・v31 は作業用）

// レジスタなしで使える定数（AArch64 では 0 = xzr・wzr）
static bool ir_target_free_const(IrInst *v) { return v->op == IR_CONST && v->imm == 0; }

// 割り当てるレジスタ（呼び出し元保存を先に試す）
// 浮動小数点のレジスタは 32 + 番号（v16〜v29 は呼び出し元保存、v8〜v15 は呼び出し先保存（下位 64 ビット）。
// v0〜v7 は引数・戻り値、v30・v31 は作業用）
static const int ir_alloc_order[] = {9, 10, 11, 12, 13, 14, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
#define IR_NREGS 17
// x0〜x7 を作業用に使う命令のない関数では、x0〜x7 も使う（ir_low_ok）。x0 は番号 0 が「なし」なので、
// 値のレジスタの番号では IR_X0（63）で表し、出力のときに 0 にする（ir_phys。2026-11-27）
#define IR_X0 63
static const int ir_alloc_order_low[] = {9, 10, 11, 12, 13, 14, 15, 1, 2, 3, 4, 5, 6, 7, IR_X0,
                                         19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
#define IR_NREGS_LOW 25
// v29 も作業用（積和演算の 3 つ目の引数を読むため）
static const int ir_falloc_order[] = {48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
                                      40, 41, 42, 43, 44, 45, 46, 47};
#define IR_NFREGS 21
TLS static bool ir_reg_used[64];

static int ir_new_spill(void) {
  ir_frame = align_to(ir_frame + 8, 8);
  return -ir_frame;
}

// 割り当てるレジスタの組（1: 整数 ir_alloc_order、2: 浮動小数点 ir_falloc_order）
static const uint8_t ir_order_tab[64] = {
  [1] = 1, [2] = 1, [3] = 1, [4] = 1, [5] = 1, [6] = 1, [7] = 1,
  [9] = 1, [10] = 1, [11] = 1, [12] = 1, [13] = 1, [14] = 1, [15] = 1,
  [19] = 1, [20] = 1, [21] = 1, [22] = 1, [23] = 1, [24] = 1, [25] = 1, [26] = 1, [27] = 1, [28] = 1,
  [40] = 2, [41] = 2, [42] = 2, [43] = 2, [44] = 2, [45] = 2, [46] = 2, [47] = 2,
  [48] = 2, [49] = 2, [50] = 2, [51] = 2, [52] = 2, [53] = 2, [54] = 2, [55] = 2, [56] = 2, [57] = 2,
  [58] = 2, [59] = 2, [60] = 2, [IR_X0] = 1,
};

// 引数のレジスタ x0〜x7 か（値のレジスタの番号で）
static bool ir_is_low_reg(int r) { return (r >= 1 && r <= 7) || r == IR_X0; }
// 引数の番号 k（0〜7）の値のレジスタの番号
static int ir_low_reg(int k) { return k ? k : IR_X0; }

// ir_is_callee_reg の表（x19〜x28・v8〜v15）
static const bool ir_callee_tab[64] = {
  [19] = 1, [20] = 1, [21] = 1, [22] = 1, [23] = 1, [24] = 1, [25] = 1, [26] = 1, [27] = 1, [28] = 1,
  [40] = 1, [41] = 1, [42] = 1, [43] = 1, [44] = 1, [45] = 1, [46] = 1, [47] = 1,
};

static bool ir_is_callee_reg(int r) {
  return (r >= 19 && r <= 28) || (r >= 40 && r <= 47);
}

// 決まったレジスタで受け渡す値（2026-11-27）: 呼び出しの引数に使って終わる値はその引数のレジスタ、
// 呼び出しの戻り値と return する値は x0。値の番号 → 値のレジスタの番号（0 はなし）
static uint8_t *ir_fixed_hints(void) {
  uint8_t *preg = ir_alloc(ir_nvals + 1);
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next) {
      if (in->op == IR_CALL) {
        if (in->size && !in->fp && !in->ty && in->id <= ir_nvals)
          preg[in->id] = IR_X0;
        for (int k = 0; k < in->nargs; k++) {
          IrInst *a = in->args[k];
          int r = in->argregs[k];
          if (r < 0 || r > 7 || a->fp || (in->argtys && in->argtys[k]) || a->id > ir_nvals)
            continue;
          if (a->end == in->pos && !preg[a->id])
            preg[a->id] = ir_low_reg(r);
        }
      } else if (in->op == IR_RET) {
        for (int k = 0; k < in->nargs && k < 8; k++)
          if (!in->args[k]->fp && in->args[k]->id <= ir_nvals && in->args[k]->end == in->pos &&
              !preg[in->args[k]->id])
            preg[in->args[k]->id] = ir_low_reg(k);
        if (!in->nargs && in->a && !in->ty && !in->a->fp && in->a->id <= ir_nvals && in->a->end == in->pos &&
            !preg[in->a->id])
          preg[in->a->id] = IR_X0;
      }
    }
  return preg;
}

// x0〜x7 を値に割り当ててよいか（2026-11-16）: x0〜x7 を作業用に使う命令がない。前は x0〜x7 をいつも割り当てず、
// 小さな葉の関数（fnv_hash など）でも退避の要る x19〜 を使い、入口と出口で保存・復元していた（GCC は x0〜x7 も使う）。
// 2026-11-27 から関数を呼ぶ関数でも（呼び出しをまたぐ値は x19〜 のまま。呼び出しの引数は並列に写す ir_par_move）。
// 構造体の引数（x0〜x7 に読み込みながら写す）・構造体の戻り値の 2 語目（IR_X1）のある関数は使わない。
// 構造体のコピー（IR_MEMCPY。逃がした土台を x6・x7 に読む）のある関数では x6・x7 だけ使わない（*no67。2026-11-27）
static bool ir_low_ok(bool *no67) {
  *no67 = false;
  for (int i = 0; i < ir_norder; i++)
    for (IrInst *in = ir_order[i]->first; in; in = in->next)
      switch (in->op) {
      case IR_MEMCPY:
        *no67 = true;
        break;
      case IR_ASM: case IR_ASMOUT: case IR_PARAM_STRUCT:
      case IR_CAS: case IR_XCHG: case IR_X1:
        return false;
      case IR_CALL:
        for (int k = 0; in->argtys && k < in->nargs; k++)
          if (in->argtys[k])
            return false;
        break;
      default:
        break;
      }
  return true;
}

// 定数の掛け算 c が (2^k + 1) × 2^m なら 1、(2^k - 1) × 2^m なら -1（k ≥ 1、2^k - 1 は 3 以上）、ほかは 0。
// 2 のべき乗は IR_SHL に簡約される
static int ir_mul_shape(int64_t c, int *k, int *m, bool *dummy) {
  (void)dummy;
  if (c <= 2)
    return 0;
  int mm = 0;
  uint64_t u = (uint64_t)c;
  while (!(u & 1))
    u >>= 1, mm++;
  if (u < 3)
    return 0;
  for (int kk = 1; kk < 32; kk++) {
    if (u == (1ULL << kk) + 1 || (u == (1ULL << kk) - 1 && kk >= 2)) {
      if (k)
        *k = kk;
      if (m)
        *m = mm;
      return u == (1ULL << kk) + 1 ? 1 : -1;
    }
  }
  return 0;
}

// 即値として命令に埋め込める定数（その命令の第 2 オペランド）
static bool ir_imm_operand(IrInst *user, IrInst *v) {
  if (user->op == IR_FCMP && user->b == v && v->op == IR_FCONST && v->imm == 0)
    return true; // fcmp d, #0.0
  if (v->op != IR_CONST || user->b != v)
    return false;
  switch (user->op) {
  case IR_ADD:
  case IR_SUB:
  case IR_CMP:
    // 負なら逆の命令（sub・add・cmn）の即値（2026-11-27）
    return v->imm > -4096 && v->imm < 4096;
  case IR_MUL:
    return ir_mul_shape(v->imm, NULL, NULL, NULL) != 0;   // ずらす命令と足し算・引き算（2026-11-27）
  case IR_SHL:
  case IR_SHR:
  case IR_SAR:
    return v->imm >= 0 && v->imm < 32;   // lsl・lsr・asr #n（w の形でも使える範囲。2026-11-21）
  default:
    return false;
  }
}

// 呼び出し in の k 番目の引数が、使う所で作る整数の定数か（0 は xzr のまま）
static bool ir_arg_const(IrInst *in, int k) {
  IrInst *a = in->args[k];
  return a->op == IR_CONST && !a->fp && a->imm != 0 && !(in->argtys && in->argtys[k]);
}

static int ir_tbz_bit(IrInst *c);

// IR_SELECT が定数を選ぶだけか: 1・0 なら 1、0・1 なら -1（cset）、-1・0 なら 2、0・-1 なら -2（csetm）、ほかは 0
static int ir_select_cset(IrInst *in) {
  IrInst *b = in->b, *c = in->c;
  if (b->op != IR_CONST || c->op != IR_CONST || b->fp || c->fp)
    return 0;
  if (b->imm == 1 && c->imm == 0)
    return 1;
  if (b->imm == 0 && c->imm == 1)
    return -1;
  if (b->imm == -1 && c->imm == 0)
    return 2;
  if (b->imm == 0 && c->imm == -1)
    return -2;
  return 0;
}

