# slimcc の出すコードを良くする（続きの手引き）

標準のカーネルは、自作のコンパイラ slimcc-a64 で作った slimcc 版（2026-11-16 から。方針「Monocs の中で完結」）。
GCC 版より遅い分は、slimcc の出すコードを良くして縮める。この文書は、その作業を後から続けるための
手引き（今の位置、測り方、確かめ方、候補、試して見送ったもの）。経緯の細かい記録は `CLAUDE.md` の
「slimcc 版のカーネル（中のコンパイラ）を速くする」から下。

コンパイラ自身を速くする方針は `docs/slimcc-compile-speed.md`（2026-11-21）。

## 0. 再開するとき（2026-11-27 時点。まずここを読む）

### 今の位置

実機（Zero 2 W、1000MHz）のカーネルの中のコンパイラ（`sh tools/slimcc/devbench.sh`、`cc -c -o /dev/null`、3 回の最小）:

| | parse.c | codegen.c |
|---|---|---|
| 2026-11-26（if 変換の前） | 565M | 1,200M |
| 2026-11-27（8b4c519） | 523M | 1,147M |

- 2 世代目の slimcc の .text（verify.sh の最後）: 575,380 → 514,932 バイト。
- Pi 5 の bench.sh: nbody 402ms（GCC -O2 301ms）、primes 1,025ms（1,007ms）、strhash 399ms（402ms。GCC に並んだ）。
- 2026-11-27 に入れたもの（下の 9〜16）: 引数の定数を x0〜x7 に直接、tbz・tbnz、ccmp・cset、呼び出しのある関数でも
  x0〜x7 を値に（いちばん効いた。3〜4%）、退避の stp・ldp、memcpy のある関数でも x0〜x5、定数の計算の直し。
  どれも main にコミット済み（GitHub には未 push）。実機は 8b4c519 のカーネルで動いている。

### いちばん確かな探し方（14）

同じ仕事を GCC 版と slimcc 版のカーネルで動かし、関数ごとの命令の数を比べる。差の大きい関数の -S を読む。

```sh
make && make kernel-gcc
export NOS_HOST=192.168.3.96
N="python3 tools/uart/nos.py"
$N cmd ' perf record -e inst -p 20000 -n 400 cc -c -o /dev/null /usr/src/kernel/cc/codegen.c' > slim.txt
$N kernel build/kernel8.img; sleep 25
$N cmd ' perf record -e inst -p 20000 -n 400 cc -c -o /dev/null /usr/src/kernel/cc/codegen.c' > gcc.txt
$N kernel build/kslim/kernel8.img          # 標準に戻す
join <(awk '$3=="kernel"{print $4, $2}' slim.txt | sort) <(awk '$3=="kernel"{print $4, $2}' gcc.txt | sort) |
  awk '{print $2-$3, $2, $3, $1}' | sort -rn | head -30
```

- `-p 10000` だと slimcc 版は記録があふれる（65,536 個）。`-p 20000` で 3.5 万個ほど。
- 関数の -S: `cc/slimcc-monocs -std=c23 -nostdinc -isystem cc/slimcc_headers/include -isystem build/picolibc-install/include -Icc -DSLIMCC_LIBRARY -S -o - cc/FILE.c`
  （`make -C cc slimcc-monocs` で作り直してから）。
- 命令の種類ごとに GCC と数を比べるのは、GCC のインライン展開の差が混ざるので、手がかり程度。
- 2026-11-27 の比べ（8b4c519 の前。GCC 版 24,771、slimcc 版 35,099）で差の大きかったもの: get_entry_h（直した）、
  ir_linear_scan（1,012 対 785）、ir_emit_inst1（431 対 223）、ir_mem2reg（711 対 505）、ir_alloc_nz・zone_malloc（確保）、
  ir_mark_low32、new_token、vfprintf（long double のため今までのコード生成）。

### 次の候補（効きそうな順）

1. （済み、2026-11-27。下の 15・16）**switch の表引き**とビットの表。コンパイラ自身への効き目は小さかった（0.4%・0%）。
2. 呼び出しをまたぐ値の退避（12 の「まだ」）、構造体を渡す呼び出しのある関数でも x0〜x7。
3. 小さなもの: csinc（`c ? x + 1 : x`）、範囲の確かめ（`x >= A && x < B` を sub と符号なしの比べ 1 つに）、
   論理即値（`and x, x, #0xff` を定数を作らずに）、`x & mask`（2^k でない）の `tst`、合流の写しの定数（`movz` + `mov`、
   約 9 万命令で 160 か所）、末尾の呼び出し（707 関数のうち 216 か所。perf の -g・落ちたときの呼び出し元が 1 つ減る）。

### 2026-11-27 に学んだこと（同じことでつまずかない）

- 新しい命令を出すようにしたら、内蔵アセンブラのかけらの道（cc/assembler.c の frag_insn）でも直接組み立てる。
  でないと文字列の道を通り、命令が減ってもコンパイラが遅くなる（tbz・ccmp で 0.5% 遅くなった）。cmp_o.sh で .o が同じか確かめる。
- 命令が減っても実機で遅くなることがある: ループの反転を広げたもの（命令 +0.8%・1.5〜2% 遅い。戻した）、ccmp
  （命令・分岐・読み違いは減ったが命令キャッシュのミスが増えた）。必ず前後のカーネルを交互に測る。命令の数・分岐・
  キャッシュは `perf -e inst,br,br-miss,l1i-miss cc -c -o /dev/null ...`。
- GCC と比べる試験に未定義動作（符号つきのあふれ）を入れない。GCC の -O0・-O2・-fwrapv で出力が同じか、
  `gcc -fsanitize=undefined` で確かめる（run.sh は `gcc -w`（-O0）と比べる）。
- `git stash` で前のコンパイラと比べた後は、作り直しが漏れることがある（古いコンパイラの結果で「誤り」に見えた）。
  `touch cc/ir.inc` してから `make -C cc slimcc slimcc-monocs`。
- verify.sh はコンパイラを作り直すので、流している間に cc/ を書き換えない（直しは別のファイルに用意しておく）。
- 実機は Wi-Fi だけ（NOS_HOST=192.168.3.96）。前のカーネルは build の外（作業用のフォルダ）に写してから比べる。

## 0.1 ターゲットに依存するところ（2026-11-27。別の CPU を足すときの手がかり）

ユーザーの「将来違う CPU を使う場合に備えたい」から。今すぐ全部を作り替えず（使わない仕組みを先に作らない）、
(1) 中間表現のコード生成を「ターゲットに依存しない部分」と「AArch64」のファイルに分け、(2) 残る AArch64 の前提を
ここにまとめた。別の CPU を使うと決まったら、その CPU に合わせてここから分けていく。

### ファイルの分け方

| ファイル | ターゲット | 中身 |
|---|---|---|
| cc/ir.inc | 依存しない（下の「ir.inc に残る前提」を除く） | 中間表現を作る・最適化・SSA を抜ける・生存区間・線形走査の手順・全体の流れ（ir_gen_fn） |
| cc/ir_a64.inc | AArch64 | レジスタの組と順・呼び出し規約で決まったレジスタ（ir_low_ok・ir_fixed_hints）・即値（ir_imm_operand・ir_mul_shape）・レジスタなしで使える定数（ir_target_free_const） |
| cc/ir_a64_emit.inc | AArch64 | 命令の記録とかけら、tbz の書き直し、命令ごとの出力（ir_emit_inst1）、比較と分岐をまとめる印（ir_mark_fused）、フレームの入口と出口 |
| cc/sched.inc | Cortex-A53 | 命令の並べ替えの模型（遅延・同時に出せる組。tools/perf/a53lat.c で測った値） |
| cc/codegen.c | AArch64 | 今までのコード生成（中間表現で扱えない関数）と、呼び出し規約の分類（classify・calling_convention） |
| cc/assembler.c | AArch64 | 内蔵アセンブラ（命令の組み立て、かけらの道 frag_insn） |
| cc/ld.c・cc/memlink.c | AArch64 | ELF のリロケーション、範囲外の分岐の中継（AAPCS64 の IP0）、TLS の配置 |
| cc/rt/ | AArch64 | long double（binary128）の演算（AAPCS64 の long double）、__clear_cache（dc・ic） |
| cc/platform/*.c、preprocess.c の init_macros | AArch64 | ターゲットの名前・パス・予定義マクロ（`__aarch64__`・`__ARM_*`） |
| cc/type.c・parse.c | AArch64 の ABI | char は符号なし、`_BitInt`・`__int128` の大きさと境界、名前のないビットフィールド、va_list の形（preprocess.c） |
| ほかの前処理・字句・構文解析・型・omp.inc | 依存しない | |

2026-11-27 の分け方は、中身を動かしただけ（出すコードは 1 バイトも変わらない。cmp_s.sh・cmp_o.sh で確かめた）。

### ir.inc に残る前提（ターゲットに聞く形にしていないもの）

- 値の表し方: 整数を 64 ビットのレジスタに型に合わせて拡張して持つ。32 ビットの計算を w レジスタでする印
  （wform・nsw の w の命令・low32・ir_cmp_w・ir_use_low32・ir_nsw_w）。RISC-V（64 ビット）も *W の命令があるので
  近いが、上位ビットの扱い（RISC-V は 32 ビットの結果を符号拡張、AArch64 の w はゼロ拡張）が違う。
- 番地の形（ir_fold_addresses）: `[base + imm]`（-256〜4096 × 大きさ）、`[base + idx << log2(大きさ)]`。ターゲットごとに
  使える形と範囲が違う（RISC-V は `[base + imm12]` だけ）。
- csel（IR_SELECT・ir_if_convert）、ccmp（ir_ccmp_chain・CMP の c と ccond）、tbz（ir_tbz_bit）、cset、積和
  （ir_fp_contract の IR_FMADD など）、fsqrt・fabs を前提にした変形。条件の番号（CC_*・cc_name・cc_inverse）は
  AArch64 の条件の並び。ない CPU では、変形を止める印（例: ir_target_has_ccmp）をターゲットの記述に足す。
- スレッドポインタ（IR_TP = tpidr_el0）と TLS の番地（local-exec・initial-exec）。
- フレーム: x29 から下に退避・局所変数・逃がした値（ir_frame・spill・ofs が x29 からの負のオフセット）。
  スタックで渡された引数は x29 + 16 から。インラインアセンブリのある関数は x19・x20 を [x29 - 16] に。
- 呼び出し規約で決まる中間表現の形: 構造体を x0・x1（HFA は v0〜v3）で受け渡す（IR_PARAM_STRUCT・SROA の語・
  IR_X1）、大きな構造体の戻り値の領域 x8、va_arg の手順（ir_va_arg。AAPCS64 の va_list）、引数のレジスタの番号
  （argregs が x0〜x7・v0〜v7）、_Bool の戻り値は下位 8 ビット（rbool）。
- インラインアセンブリのオペランドを x0〜x7・v0〜v7 に置く、アトミック操作（IR_CAS・IR_XCHG）が x0〜x5 を使う。
- 即値の目安が散らばっているところ（LICM の「即値に入る定数は数えない」、ir_alloc の 4096 は別もの）。

### 別の CPU のとき（見込み）

- **同じ AArch64 の別のコア**（Pi 4 の Cortex-A72、Pi 5 の A76 など）: 命令は同じ。要るのは sched.inc の模型をコアごとに
  選べるようにする（-mcpu。表を測り直す）、使える命令の切り替え（ARMv8.1 の LSE のアトミック命令、ARMv8.2 など）、
  cc/platform と予定義マクロ。if 変換の数（IR_IFCVT_OPS）・展開の大きさなどの目安も、順不同に実行するコアでは測り直す。
- **別の命令セット**（RISC-V・x86-64 など）: ir_a64.inc・ir_a64_emit.inc に当たる 2 つのファイル、アセンブラ、リロケーション、
  rt/、platform を新しく作る。ir.inc は、上の「残る前提」をターゲットの記述に聞く形に直してから共有する。
  codegen.c（今までのコード生成）は中間表現で扱えない関数のためのものなので、新しいターゲットでは、まず中間表現で
  扱えない関数をなくす（long double・va_start のある可変長引数の関数・setjmp）か、そのターゲットの分も作る。
- OS（Monocs）の側の CPU に依存するところ（boot.S・vectors.S・MMU・例外・SMP のスピンテーブル・PMU など）は、この文書の外。

## 1. 今の位置（2026-11-16。今の数字は 0. にある）

実機（Zero 2 W、1000MHz）で、カーネルの中のコンパイラの `cc -c -o /dev/null`（CPU サイクル、3 回の最小。
`sh tools/slimcc/devbench.sh`）:

| | parse.c | codegen.c |
|---|---|---|
| slimcc 版（標準） | 554M | 1059M |
| GCC 版 | 373M | 735M |
| 比（slimcc ÷ GCC） | 1.49 倍 | 1.44 倍 |

この表は、実機の /src が da19952 のソース（`make sdimage-self && nos.py pushdir build/selfsrc /src`）のときのもの。
今後はこれを基準にする。コンパイルするソースが変わると数字も変わる（2026-11-16 の作業中は、朝のままの /src で
slimcc 版 parse.c 655M → 486M、codegen.c 1022M → 764M と測っていた。GCC 版は 411M → 327M、652M → 523M）。

- 実機の上のセルフホストのビルド（3 コア）: slimcc 版の上でカーネル 8 秒・C ライブラリ 43 秒、GCC 版の上で 5.5〜5.9 秒・31〜36 秒。
- Pi 5 のベンチマーク（`cc/test-a64/bench.sh`、verify.sh の最後）: nbody 399ms（GCC -O2 299ms）、primes 1058ms（1007ms）、strhash 416ms（399ms）、自分のコンパイル 629ms。
- 命令あたりのサイクルは両方ほぼ同じ（0.74 前後）。差はほとんど命令の数（slimcc 版が多い）。

測った数字は、比べる前に必ず測り直す（実機の /src の中身、ファームウェア、温度で変わる）。

## 2. 測り方

```sh
make && make kernel-gcc                              # 標準（slimcc 版）と GCC 版
sh tools/slimcc/devbench.sh                          # 標準のカーネルを送って parse.c・codegen.c を測る
sh tools/slimcc/devbench.sh build/kernel8.img build/kslim/kernel8.img   # 両方（最後に標準が残る）
```

- 前後を比べるときは、同じ /src（`nos.py pushdir build/selfsrc /src` で変わる）で、前のカーネルと交互に測る。
- `clock` の measured が 1000 MHz であること（devbench.sh が表示する）。起動して 1 回目は少し遅い（3 回の最小を使う）。
- どこが重いか（実機）: `nos.py cmd ' perf record -e cycles -n 40 cc -c -o /dev/null /src/cc/parse.c'`。
  呼び出し元も見るなら `-g`。命令の数は `-e inst`。slimcc 版はインライン展開が少ないので、関数ごとに細かく見える。
- 出したコードを見る: `objdump -d --no-show-raw-insn build/kslim/slimcc/codegen.o | awk '/<関数名>:/,/^$/'`。
  カーネルと同じオプションの -S:
  `cc/slimcc-monocs -std=c23 -nostdinc -isystem cc/slimcc_headers/include -isystem build/picolibc-install/include -Icc -DSLIMCC_LIBRARY -S -o - cc/codegen.c`
  （`make -C cc slimcc-monocs` で作り直してから。make kernel-slimcc は自動で作り直すが、直接使うときは古いことがある）。
  同じ関数を GCC 版の `objdump -d build/kernel.elf` と並べると、何が多いかがわかる。
- Pi 5 は命令を順不同に実行するので、差が小さく見える（分岐やレジスタの退避が隠れる）。判断は実機で。
  Pi 5 で手早く見るなら、2 世代目（slimcc でコンパイルした slimcc）を前後で作って、同じファイルのコンパイル時間を比べる。

## 3. 確かめ方（コードの出し方を変えたら全部）

1. `make cc-test`: test/ の全部、GCC との呼び出し規約、2・3 世代目の一致、
   8b（`cc/test-a64/loop_gen.py`: ループの形 300 個 × 種 4 つを GCC と比べる）、
   8c（`cc/test-a64/gccmatch/*.c`: va_arg・あふれ・別名（alias.c）・文字列リテラル（strlit.c）を GCC と比べる）、
   8d（`cc/test-a64/switch_gen.py`: switch を GCC と比べる）。新しい最適化には、ここに GCC と比べる試験を足す。
2. 出力を変えないはずの変更なら `sh tools/slimcc/cmp_s.sh`（HEAD の slimcc と -S が同じ）と
   `sh tools/slimcc/cmp_o.sh`（かけらの道と文字列の道の .o が同じ）。
3. `sh tools/test/verify.sh`（quick なし）: セルフホストの一致（Monocs の中の slimcc はかけらの道、Linux 上の
   カーネルのビルドは -MMD なので文字列の道。両方の一致を確かめることになる）、gen2、libctest。
4. QEMU の試験（既定は標準の slimcc 版）: `make sdimage` の後に `tools/test/{shell,pthread,openmp,linuxapi}.sh`。
5. 実機: `sh tools/test/hwcheck.sh`（両方のカーネル）。アプリの速さ（primes 44ms・primes_mc 495ms）も見る。
6. 検査付きのコンパイラで、コンパイラ自身の未定義動作を探す: `make -C cc slimcc-san` の後、
   `cc/slimcc-san -S`・`-c` でいろいろなファイルを通す（2026-11-16 に、-S でだけ落ちる誤りをこれで見つけた）。

## 4. 候補（効きそうな順の見込み。どれも測ってから決める）

- **呼び出しのある関数のレジスタ**: 呼び出しをまたぐ値は x19〜x28 に置き、入口と出口で保存・復元する。
  GCC はもっと少ない保存で済ませている関数が多い（実機の perf で、小さな関数の入口・出口が目立つ）。
  逃がす値の選び方（ir_linear_scan の victim）、呼び出しの前後だけ逃がす（区間を分ける）など。
- （済み、2026-11-27。下の 12）**x0 も割り当てる**。
- **浮動小数点の v0〜v7**: 葉に近い関数でも使っていない（整数の x1〜x7 と同じ考え方で足せる）。
- **定数の掛け算**: 2 のべき乗以外（×24 など。構造体の配列の添字）は mul。`add x, x, x, lsl #1` と `lsl` に。
- **ループの番地の計算**: 添字から毎回番地を計算している（誘導変数の強度の削減、ポインタを進める形）、
  ldp・stp にまとめる、後置インクリメントの番地。
- **ループをまたぐ読み出しの除去**: ir_load_elim は戻る辺を引き継がない（ループの外に出せない読み出しは毎回読む）。
- **密集した switch の表引き**: 今は二分探索（ir_switch_tree）。中間表現のブロックは後続が 2 つまでなので、
  表引きには多分岐の命令が要る。
- **末尾の呼び出し**（`bl f; ret` → `b f`）。
- **関数をまたいだ文字列リテラルのまとめ**: 今は同じ関数の中と、関数の外どうしだけ（parse.c の
  string_literal_var。関数の中の文字列はその関数と一緒に出力されるので、またぐには出力の仕組みが要る）。
- （済み、2026-11-21）スタックで渡す引数・`__builtin_alloca`・計算型の goto を中間表現で（下の 5.1）。
- （済み、2026-11-21）逃がした値の番地・逃がす値の選び方・構造体を受け渡しする関数のインライン展開（下の 5.2）。
- （済み、2026-11-21）構造体をレジスタに置く（SROA。下の 5.3）。
- **SROA の広げ方**: HFA（浮動小数点の構造体）・8 の倍数でない構造体・32 バイトより大きい構造体・浮動小数点の
  メンバーを読み書きする場所は、今もメモリ。浮動小数点は、整数のレジスタとの間で写す命令（fmov）が要る。
- **生存区間を分ける**（本当の形）: 5. の「ブロックの中で読み直さない」が効かなかったので、残る逃がした値は、
  ブロックをまたいで 1 回ずつ使うもの。ループの単位で分けても、QuickJS の振り分けのように関数全体が 1 つの
  ループのときは効かない。
- **今までのコード生成のままの関数**: 中間表現で扱えない関数（long double、va_start のある可変長引数の
  関数、setjmp など）。`SLIMCC_IR_WHY=1 make kernel-slimcc` で理由と数がわかる（2026-11-15 に 76 個）。
- コンパイラの中の残り（実機の perf、slimcc 版）: ir_linear_scan・ir_intervals（レジスタの割り当て）、
  名前の表（fnv_hash・get_entry_h）、字句解析（tokenize・new_token・read_ident）、中間表現の各段
  （ir_count_uses・ir_resolve_operands・ir_cleanup・ir_liveness・ir_fingerprint）。どれも 1〜4%。

## 4.1 AArch64 の命令で、まだ使っていないもの（2026-11-22 に cc/ir.inc を調べた）

使っているもの: 番地の形（`[x, #imm]`・`[x, y, lsl #k]`・`[x, w, sxtw #k]`）、w レジスタの 32 ビットの計算、`madd`・`msub`、
`fmadd`・`fnmsub`（GNU の規格のとき）、`cbz`・`cbnz`・`cset`、ずらす命令をまとめた計算、`fsqrt`・`fabs`、A53 向けの並べ替え、葉の関数のフレームの省略。

使っていないもの（効きそうな順の見込み。どれも測ってから決める）:

| 機能 | 中身 | 効きそうな場面 |
|---|---|---|
| ベクトル化（NEON） | 1 命令で 2〜4 個を一度に計算 | 配列・浮動小数点のループ（nbody の GCC との差の大きな理由） |
| 条件付きの選択（`csel`・`csinc`・`csneg`・`ccmp`、`fcsel`） | 分岐せずに条件で値を選ぶ・比べを続ける | `a > b ? a : b`、小さな if。分岐の読み違いが減る。内蔵アセンブラに `fcsel` がない（足す） |
| `ldp`・`stp` のまとめ | 関数の入口・出口だけで使っている。ふつうの並んだ読み書きはまとめていない | 構造体のコピー、並んだ変数（4. の「ループの番地の計算」と同じ候補） |
| 後置・前置の番地の形 | `ldr x0, [x1], #8` | ポインタを進めるループ |
| 定数の割り算を掛け算に（`umulh`・`smulh`） | `/ 10` などを掛け算とずらしで | 割り算の多い計算（A53 の割り算は w で約 11、x で約 19 サイクル） |
| ビットの取り出し（`ubfx`・`sbfx`・`bfi`・`bfxil`） | 今までのコード生成（codegen.c）だけが使い、中間表現は使っていない | ビットフィールド |
| `tbz`・`tbnz` | 1 ビットを見て飛ぶ | フラグの判定 |
| `clz`・`rbit`・`rev`・`cnt` | 先頭の 0 の数、ビット・バイトを逆に、1 の数 | `__builtin_clz` など。`__builtin_bswap` は slimcc にない（`<endian.h>` の be32toh などがアプリで使えない理由） |
| `frintm`・`frintp`・`frintz`・`frinta` | `floor`・`ceil`・`trunc`・`round` を命令で | 浮動小数点の丸め |

この CPU にないもの: LSE のアトミック命令（ARMv8.1 から）、暗号の命令（Zero 2 W の Cortex-A53 にはない）。

## 5.1 中間表現で扱うようにしたもの（2026-11-21）

- スタックで渡す引数: 呼ばれる側は、その場所（x29 + 16 + stack_offset）を変数の場所にする（ir_slot_at。SSA の
  変数なら入口で読む。フレームは省かない: ir_stk_params）。呼ぶ側は呼ぶ直前に sp を下げて [sp + オフセット] に
  書き、戻ったら上げる（IR_CALL の stkargs。argregs が負ならスタック）。
- `__builtin_alloca`（IR_ALLOCA。codegen.c の builtin_alloca と同じ命令。VLA は今までどおり）。sp は出口で x29
  から戻る。alloca のある関数はインライン展開しない。
- 計算型の goto（IR_INDBR・IR_LABELADDR）: goto *p を 1 つの振り分けのブロックに集め、後続はラベルごとの入口の
  ブロック（.L.jmp.N を出してラベルへ飛ぶ）。ラベルの番号は name_labels（関数の中の静的な表の &&label も同じ番号）。
  ブロックの後続は配列（succ。ふつうは succ_buf の 2 つ）。
- 効果: QuickJS（Pi 5、-g なし）fib(27) 173 → 143ms、100 万回のループ 302 → 236ms。実機 1028 → 1001ms、
  1753 → 1517ms（逃がした値の 2 命令が残る。上の候補）。カーネルの中の今までのコード生成の関数 76 → 55 個。
- 気をつけること: slimcc は `-g` のとき中間表現を使わない（ir_gen_fn の opt_g）。Linux で Makefile の
  `-g` 付きで作ると最適化なしの版を測ることになる（2026-11-21 に一度これで見誤った）。
- 試験: `cc/test-a64/gccmatch/stack_args.c`・`alloca.c`・`computed_goto.c`（GCC と比べる）。

## 5.2 逃がした値（2026-11-21。ユーザーの「この 3 つ全部お願いします」から）

- フレームの番地（ir_fmem）: 局所変数・逃がした値を、フレームの底を土台にした正のオフセットで 1 命令で読み書きする。
  土台は sp（関数の中で動かないとき。スタックの引数を書く間は ir_sp_adj を足す）か、alloca のある関数は x28
  （入口で mov x28, sp。割り当てない）。x29 から -256 より遠いと sub と ldr の 2 命令だった。
- 定数は逃がさず、使う所で作り直す（mov）。逃がす相手も定数を先に選ぶ。シフトの量が定数なら即値の形（lsl #n）。
  即値の形にできる定数は、レジスタを持っていても即値で。
- 逃がす値の選び方（ir_spill_cheaper）: 重み（定義と使う所の数。ループの中は 8 倍）÷ 生きる長さ が小さい値から。
  前は「いちばん遠くまで生きる値」で、関数全体で使う値（QuickJS の pc など）が先に逃がされていた。
- 構造体（JSValue など）を受け渡しする小さな関数もインライン展開する（引数は引数の場所に、戻り値は呼び出しの
  rtn_buf に、8 バイトずつ・型の組 0 で写す。ir_copy_struct）。
- 効果（実機）: QuickJS fib(27) 1000 → 590ms、100 万回のループ 1513 → 722ms。カーネルの中のコンパイラ parse.c
  575 → 566M サイクル。primes 44ms・primes_mc 495ms は同じ。Pi 5 の bench.sh は同じ（.text は 2% 小さい）。
  Pi 5 の QuickJS（-g なし）fib 143 → 99ms、ループ 236 → 141ms（GCC 版 34・39ms）。
- 試験: `cc/test-a64/gccmatch/inline_struct.c`。

## 5.3 SROA（2026-11-21。ユーザーの「お願いします！」から）

- 1 段目（構造体の受け渡しを 8 バイトずつの読み書きに）: 大きさが 8 の倍数で 32 バイトまで・境界 8 以上の構造体
  （ir_word_struct）の代入と 0 での初期化、x レジスタで渡す 16 バイトまでのもの（ir_word_reg_struct）の呼び出しの
  引数（語に分けて x レジスタの値として）、戻り値（IR_RET の args）、入口の引数（IR_PARAM を語ごとに）、呼び出しの
  戻り値（x0 は IR_CALL の値、x1 は直後の IR_X1）。前は IR_MEMCPY・IR_PARAM_STRUCT など中間表現の外の命令の中に
  隠れていて、読み出しの除去・ループの外への移動を止めていた。型の組は 0。
- 2 段目（mem2reg。ir_mem2reg）: アドレスが定数のオフセットの読み書き（と定数を足す計算）にしか使われない場所
  （32 バイトまで。スタックで渡された引数を除く）を、8 バイトの塊ごとの変数として SSA にする。ブロックを逆後順に
  たどり、前任がすべて済んだブロックから封じる（Braun らの方法のやり直し。元の変数の「ブロックの最新の値」の表は
  空にしてから）。1・2・4 バイトの読み書きはシフトと切り詰め・合成。浮動小数点・volatile・アトミック・添字の
  ある読み書き、ほかへ渡すアドレス、命令が直接書く場所（rtn_slot）があれば値にしない。その後に簡約と不要な
  コードの除去を 1 回。
- 効果（実機）: QuickJS fib(27) 590 → 527ms、100 万回のループ 722 → 510ms（Pi 5: 99 → 75ms、141 → 73ms。GCC 版
  34・39ms）。primes 44ms・primes_mc 495ms は同じ。
- コンパイルの時間: 中間表現の命令が増え、Pi 5 のコンパイラの命令数が parse.c +1%、codegen.c +3.4%（構造体の AOp を
  値で渡す所が多い）、QuickJS の quickjs.c +6.6%。実機のカーネルの中のコンパイラで parse.c 566 → 577M サイクル
  （+2%）、codegen.c 1102 → 1173M（+6.4%）。出来たコンパイラのコードの質は同じ（同じソースを新旧の slimcc で
  作って比べた。命令数は新しいほうがわずかに少ない）。2 段目の後の簡約を外しても、命令数は変わらなかった。
- 試験: `cc/test-a64/gccmatch/sroa.c`。

## 5. 試して見送ったもの（同じことをくり返さない）

- （2026-11-21）逃がした値を、同じブロックの中で 2 回以上使うなら最初に使う所でレジスタに読んで使い回す
  （生存区間を分けることの手始め。空いているレジスタを区間で調べて、IR_RELOAD を足す形）。QuickJS の
  JS_CallInternal で逃がした値の読み書きが 7,492 → 7,465 か所にしか減らず、速さも変わらなかった（同じブロックで
  2 回使う逃がした値が少ない）。足すコードに見合わないので戻した。
- （2026-11-21）構造体を受け渡しする関数だけ、インライン展開の上限を 12 → 24（JS_DupValue 18・JS_FreeValue 24 が
  入る）。QuickJS（Pi 5）fib 99 → 97ms、ループ 141 → 136ms と 2〜4% 速いが、QuickJS のコンパイルが 1.31 → 1.55 秒
  （17% 遅い）、qjs が 5% 大きい。Monocs はコンパイルの速さも大事なので戻した。

- （2026-11-27）ループの反転を、メモリを読む条件（`i < in->nargs` の `p->n`・`*p`。volatile・アトミックは除く）にも
  広げる（ir_dup_ok）。ir_count_uses の内側のループは分岐 1 つになり、`in->args` の読み出しもループの外に出たが、
  条件を 2 か所に出す分で命令の数が 0.8% 増え、実機のカーネルの中のコンパイラが parse.c 526 → 534M、codegen.c
  1,153 → 1,177M と遅くなった（展開には使わない形でも命令の数は同じだった）。戻した。
- インライン展開の大きさ（IR_INLINE_SIZE 12）を 20〜40 に: カーネルの中のコードは約 3% 速いが、アプリと
  自分のコンパイルの仕事が増え、全体では遅くなった（2026-11-15）。カーネルを作るときだけ変えるには、
  セルフホストのビルドにも渡す仕組みが要る。
- 葉の関数では退避の要るレジスタを新しく使わない: 配列の和のループが 20.6 → 28.4ms に遅くなった（x1〜x7 を
  使えるようにした今なら、結果が違うかもしれない）。
- fnv_hash を 8 バイトずつ混ぜる: 名前はほとんど 8 バイトより短く、わずかに遅くなった。
- 最適化の回の打ち切り方を変える（各段の「変えた」の印で止める）: まれに出力が変わるおそれがあるので触っていない
  （今は中間表現の指紋 ir_fingerprint が変わらなければ止める）。

## 6. 気をつけること

- 出すコードを変えると、実機の上のセルフホストのビルドの結果も変わる（Linux 上のビルドと一致すればよい）。
- 「GCC では動くが slimcc では動かない」コードがありうる（2026-11-16: 同じ文字列リテラルを 2 回書いて範囲に
  していたシェル）。QEMU の試験は標準の slimcc 版で流す（既定）。
- 最適化で型の規則（strict aliasing）を使っている。カーネル・picolibc は GCC の -O2・-O3 で作っていたので、
  この規則を前提にしている。破るコードを見つけたら、コードを直す（または `-fno-strict-aliasing`）。

## 7. ほかのコンパイラを検討した（2026-11-21。見送り）

ユーザーの「GCC の移植は難しいの？」「もっと速いコンパイラないの？」から相談して、**slimcc をこのまま一緒に
改良して、いつか GCC 並みにする**と決めた（ユーザー「このままで slimcc を一緒に改良していつか gcc 並みにしましょう！」）。

- **GCC を Monocs に移す**: できなくはないが大きい。
  - GCC は 4.8 から C++ で書かれていて、slimcc では作れない（Pi 5 の g++ で Monocs 向けに作り、C++ の実行時の部品
    （new・delete、大域のオブジェクトの初期化）を用意する）。
  - `gcc` は cc1・as・collect2・ld を別のプロセスとして動かす（libiberty の pex）。fork・exec がないので、同じ
    アドレス空間で順に呼ぶ形に書き換え、cc1 は大域の状態を持つので毎回読み込み直す。
  - 数十 MB の ELF のプログラムを読み込んでつなぐ仕組みが要る（slimld が ELF の .o を読めるのが土台）。as・ld は
    binutils を持ち込むか、GCC の出すアセンブリ（.cfi・.loc など）を内蔵アセンブラで読む。
  - RAM 512MB では大きなファイルの -O2 が足りないおそれ、コンパイルは slimcc より数倍遅い見込み（測っていない）。
    「速さが命」とぶつかる。GPLv3（third_party に足すときは相談、配るならソースも）。
  - 得られるもの: 速いコード、C++。速いコードが要るときは、今も Pi 5 の GCC で作れる。
- **ほかの候補**:
  - TinyCC: コンパイルは速いが最適化がほとんどない（2026-11-04 に slimcc に置き換えた）。
  - cproc + QBE: C で書かれ arm64 もある。QBE の目標は「GCC・LLVM の 7 割」で、今の slimcc（約 67%）と変わらない見込み。
  - MIR（c2m）: C で書かれた JIT で、メモリ上でコンパイルして動かす作りが Monocs に合う。ただ c2m は C11 で
    （README「small C11 ... compiler」。C17・C23 の記述なし）、VLA・`_Complex`・アトミック・`_Thread_local` がない。
    Monocs のカーネル（C23、アトミック、コアごとの TLS）は作れず、アプリの既定（gnu23）や SQLite（`__atomic_*`）
    でも引っかかる。slimcc の代わりにはならない。
  - Clang・LLVM: C++ で GCC より大きい。
- 小さくて GCC 並みのコードを出すものはない（ベクトル化・レジスタの割り当てなどの重い仕組みが要るため）。

### 次にやるなら

2026-11-27 から、上の「0. 再開するとき」の「次の候補」を見る（ここにあった 3 つのうち、定数の掛け算・ldp・stp・x0 の
割り当ては済んだ）。

## 8. if 変換（csel。2026-11-26）

- cc/ir.inc の IR_SELECT と ir_if_convert（ir_optimize の終わり）: ブロックの終わりの IR_CBR の先が、ほかから入らない
  小さな腕（安い整数の計算 IR_IFCVT_OPS = 2 個まで）で同じ合流先へ飛ぶ形（ひし形・三角形。合流先の前任はちょうど 2 つ）で、
  合流先の phi が整数だけ（IR_IFCVT_PHIS = 2 個まで）なら、腕の計算を分岐の前（直前の比較のさらに前）に移し、phi を
  IR_SELECT（csel）にして、まっすぐ飛ぶようにする。直前の比較とまとめる（ir_mark_fused）。下位 32 ビットの印は phi と
  同じ扱い（条件は IR_CBR と同じ）。CSE の「同じ計算か」に c も比べるようにした（IR_SELECT の偽のときの値）。
- 試験: cc/test-a64/select_gen.py（run.sh の 8e。三項演算子・片方だけの if・if/else・腕の計算・2 つ同時・ループの中を GCC と比べる）。
- 効果（実機、前後のカーネルを交互に 2 回ずつ）: 予測できない分岐のループ（乱数で足すか引くか・最大・最小）396 → 186ms
  （2.1 倍）。primes 44ms は同じ。カーネルの中のコンパイラは parse.c 565 → 566M、codegen.c 1,200 → 1,207M（0.6% 遅い。
  新しい段の時間と、読める分岐を csel にした分）。Pi 5 の bench.sh は変わらない（順不同に実行するので）。
  分岐の読めないデータの処理が大きく速くなるので入れる。コンパイラが遅くならない形（読める分岐は
  変えないなど）は、見分ける手がかりがないので今は見送り。
- 確かめ: make cc-test、verify.sh（quick なし。libc・カーネル・gen2・libctest がすべて一致）。

## 9. 引数の定数を x0〜x7 に直接作る（2026-11-27）

- 呼び出しの引数の整数の定数（0 以外）は、x0〜x7（スタックで渡すなら x16）に直接作る（cc/ir.inc の ir_arg_const）。
  引数にだけ使う定数にはレジスタを割り当てない（即値として埋め込む定数と同じく、使う数から引く）。前は
  `movz x27, #1` → `mov x0, x27` のように、ほかのレジスタ（呼び出しをまたげば退避の要るもの）に作ってから写していた。
- 見つけ方: parse.c・tokenize.c の -S を GCC と命令の種類ごとに比べた（GCC はインライン展開が多いので総数は比べられない。
  slimcc にだけ多いものを見る）。退避の要るレジスタの数は関数あたり GCC と同じくらいだった。
- 効果: 命令の数 parse.c 29,565 → 28,888、codegen.c 60,011 → 59,454（約 2% 減）。実機のカーネルの中のコンパイラ
  parse.c 566 → 560M、codegen.c 1,208 → 1,197M（約 1%。前後のカーネルを交互に 2 回ずつ）。primes 44ms は同じ。
- 確かめ: make cc-test、verify.sh（quick なし。libc・カーネル・gen2・libctest がすべて一致）。
- 同じ比べで見えた残り（次の候補）:
  - （済み。下の 12）`mov` が命令の 18%（`mov x0, xN` だけで 6,683 個 / 94,332）。
  - （済み。下の 10）bool を返す関数の結果を `uxtb` で広げてから `cbz`（200 か所）。GCC は `tbz w0, #0`。
  - （済み。下の 11）`a && b`・`a || b` の 1・0 を選ぶだけの形（合流先の前任が 3 つ）は if 変換にかからない。
  - long double を返す・比べる定数の評価（parse.c の eval2・eval_cmp・eval_double など）は今までのコード生成のまま
    （GCC の 1.8 倍の命令。long double を中間表現で扱うのは 2026-11-07 にユーザーと合意して見送り）。

## 10. tbz・tbnz（2026-11-27）

- 1 ビットを見る分岐を tbz・tbnz にする（cc/ir.inc の ir_mark_fused・ir_tbz_bit、IR_CBR の出力）:
  - _Bool を返す呼び出しの結果を、すぐ後の分岐だけに使うとき: `tbz w0, #0`（前は `uxtb w10, w0` + `cbz w10`。
    AAPCS64 で _Bool は下位 8 ビットに 0 か 1。IrInst の rbool）。
  - `x & 2^k`（定数）を、すぐ後の分岐だけに使うとき: `tbz x, #k`（前は定数を作る `movz` + `and` + `cbz`）。
- tbz・tbnz の飛べる距離は ±32KB。関数の記録の数 × 4 が 28KB 以上なら、出力の前に全部を「逆の条件で次の
  命令を飛び越し、b で飛ぶ」形にする（ir_relax_tbz。アセンブラは届かないと誤りにする）。
- 内蔵アセンブラのかけらの道（frag_insn）でも tbz・tbnz を直接組み立てる（最初は文字列の道を通っていて、
  命令が減ったのにコンパイラが 0.5% 遅くなった）。
- 効果: 命令の数 parse.c 28,888 → 28,475（1.4% 減）。実機のカーネルの中のコンパイラ parse.c 561 → 558M、
  codegen.c 1,198 → 1,197M（前後のカーネルを交互に 2 回ずつ）。
- 試験: cc/test-a64/gccmatch/tbz.c（いろいろな型・ビットの位置、_Bool を返す関数、32KB を超える関数を GCC と比べる）。
  確かめ: make cc-test、cmp_o.sh、verify.sh（quick なし。すべて一致）。
- まだ: `x & mask`（2^k でない、論理即値にできるもの）の `tst x, #mask` + `b.cond`、`and`・`orr`・`eor` の論理即値
  （今は定数を movz で作る）。

## 11. 比べを続ける（ccmp）と cset（2026-11-27）

- cc/ir.inc の ir_ccmp_chain（ir_optimize の if 変換の前）: 比べの分岐の先 X が「（定数と）比べて分岐するだけ」で、
  X の前任が B だけ、B と X の分岐が同じ先 S を持つ形（`a && b`・`a || b`。どちらの向きでも）を、
  `cmp`・`ccmp`・1 つの分岐にする（「B から X へ行く条件」が成り立つときだけ比べ、成り立たなければ
  X の条件が偽になるフラグ）。S の phi は B と X から来る値が同じときだけ。3 つ以上も続けてつなぐ。
  IR_CMP の c（すぐ前の比較）と ccond。前の比較はフラグだけを作る（ir_mark_fused）。if 変換が計算を移すときは、
  続いた比べの頭の前に（間にフラグを変える命令を入れない）。ccmp の即値は 0〜31（ほかはレジスタに作る）。
- 1・0（-1・0）を選ぶだけの IR_SELECT は cset（csetm）に（ir_select_cset。定数を作らない）。
- 内蔵アセンブラのかけらの道で、ccmp・cset・csetm・csel を直接組み立てる（前は文字列の道）。
- 例: parse.c の is_type_kw（`kind >= 91 && kind < 124`）が `cmp`・`ccmp`・`cset` の分岐なしに（前は分岐 2 つと
  1・0 の写し）。
- 効果（実機、前後のカーネルを交互に 2 回ずつ）: カーネルの中のコンパイラ parse.c 558 → 556M、codegen.c
  1,197 → 1,201M。codegen.c は、命令 729.2 → 727.6M、分岐 152.5 → 148.2M、分岐の読み違い 11.14 → 10.95M と
  どれも減ったが、L1 の命令キャッシュのミスが 4.73 → 5.09M に増えた（コードの並びが変わった分。中身の
  仕事は減っている）。
- 試験: cc/test-a64/ccmp_gen.py（run.sh の 8f。型・比べ方・定数（ccmp の即値に入る・入らない）・&&・||・混ぜたもの・
  !・2〜4 個・返す値・if・三項演算子・ループの条件の関数 300 個 × 種 4 つを GCC と比べる）。
  確かめ: make cc-test、cmp_o.sh、verify.sh（quick なし。すべて一致）。
- まだ: 範囲の確かめ（`x >= A && x < B` を `sub`・`cmp`・符号なしの比べ 1 つに。GCC はこうする）、
  csinc（`c ? x + 1 : x`）。

## 12. 呼び出しのある関数でも x0〜x7 を値に（2026-11-27）

- 前は、関数を呼ぶ関数では値を x0〜x7 に置かず、引数は x9〜 に作ってから `mov x0, x9` などで写し、戻り値も
  `mov x9, x0` で写していた（parse.c・tokenize.c の約 6,000 個の mov のうち、引数への写しが 3,672、戻り値・引数の
  受け取りが約 1,500）。
- cc/ir.inc:
  - x0 は値のレジスタの番号では IR_X0（63）で表し、出力で 0 にする（ir_phys。番号 0 は「なし」のため）。
  - ir_low_ok: 呼び出しのある関数でも x0〜x7 を割り当てる。除くのは、x0〜x7 を作業用に使う命令（memcpy・
    インラインアセンブリ・CAS・XCHG・構造体の引数を受ける PARAM_STRUCT）、構造体を渡す呼び出し、構造体の戻り値の
    2 語目（IR_X1）のある関数。呼び出しをまたぐ値は今までどおり x19〜（free_until が 0）。
  - ir_fixed_hints: 呼び出しの引数に使って終わる値（区間の終わり = 呼び出しの位置）はその引数のレジスタ、
    呼び出しの戻り値と return する値は x0 を、まず試す（引数（PARAM）が自分のレジスタのままの次）。
  - ir_par_move: 呼び出しの整数の引数と、2 語の構造体の return は並列に写す（写す先がまだ読んでいない元なら
    後回し、輪は x16 で切る）。関数のポインタが x0〜x7 にあれば、先に x17 へ。
- 効果: parse.c の命令の数 28,442 → 25,640（9.9% 減。mov 6,642 → 3,832）、tokenize.c 5,961 → 5,517。
  2 世代目の slimcc の .text 575,380 → 541,524 バイト。実機のカーネルの中のコンパイラ parse.c 556 → 534M
  （4.0%）、codegen.c 1,201 → 1,165M（3.1%）（前後のカーネルを交互に 2 回ずつ）。primes 44ms は同じ。
- 試験: cc/test-a64/gccmatch/parmove.c（引数の入れ替え（輪）・8 個を逆に・同じ値を 2 つに・関数のポインタ・
  戻り値を次の引数に・2 語の構造体・呼び出しをまたぐ値を GCC と比べる。QEMU の cortex-a53 でも同じ）。
  確かめ: make cc-test、verify.sh（quick なし。すべて一致）、QEMU の shell・pthread・openmp・linuxapi、
  実機の compat_dev.sh（Wi-Fi）と Lua の公式テスト（新しいコンパイラでコンパイルし直して final OK）。
- まだ: 構造体を渡す呼び出しのある関数（今は x0〜x7 を使わない）、呼び出しをまたぐ値の退避（x19〜 の数）。
- 続き（2026-11-27）: x0〜x7 を使えなかった関数の理由を数えると、parse.c・tokenize.c・codegen.c・preprocess.c で
  85 関数のうち 80 が構造体のコピー（IR_MEMCPY）だった。memcpy が使うのは x16・x17 と、逃がした土台を読む x6・x7
  だけなので、その関数では x6・x7 だけを避ける（ir_low_ok の no67）。2 世代目の .text 541,524 → 531,316 バイト。
  実機の速さは変わらない（0.2% 以内。memcpy のある関数は熱いところではない）。

## 13. 退避の要るレジスタを stp・ldp で 2 つずつ（2026-11-27）

- 関数の入口と出口で、x19〜x28・d8〜d15 を 1 つずつ str・ldr していた（parse.c・tokenize.c・codegen.c で
  保存 2,274・復元 2,324 命令、全体の 5.3%）。同じ種類の隣どうしを stp・ldp にする（ir_save_regs。オフセットが
  -512〜504 の間だけ。外なら今までどおり）。
- 効果: 命令の数 parse.c 25,326 → 24,500（3.3% 減）。12 の続き（memcpy の x6・x7）と合わせて、2 世代目の .text
  541,524 → 518,132 バイト、実機のカーネルの中のコンパイラ parse.c 534 → 526M、codegen.c 1,165 → 1,152M
  （前後のカーネルを交互に 2 回ずつ）。
- 確かめ: cmp_o.sh、make cc-test、verify.sh（quick なし。すべて一致）。

## 14. 同じ仕事の関数ごとの命令の数を GCC 版と比べる（2026-11-27）

- 実機で、GCC 版と slimcc 版のカーネルで同じ `cc -c -o /dev/null codegen.c` を `perf record -e inst -p 20000 -n 400`
  にかけ、関数ごとの記録の数を比べた（GCC 版 24,771、slimcc 版 35,099）。差のいちばんは名前の表を引く get_entry_h
  （873 対 247）。これがいちばん確かな探し方（命令の種類の比べは、GCC のインライン展開の差が混ざる）。
- get_entry_h で見つけて直したもの（cc/ir.inc）:
  - `(x ± c1) ± c2` → `x + (±c1 ± c2)`（0 なら x）。後置の増減 `hash++` は `(x += 1) - 1` の形で来るので、
    `add`・`movn`・`add` で元の値を作り直していた（同じ w の形・あふれの扱いのときだけ）。
  - 足し算・引き算・比べの負の即値（-4095〜-1）は、逆の命令（sub・add・cmn）の即値に（前は movn で作っていた）。
  - 定数の掛け算 (2^k ± 1) × 2^m（3・5・7・9・12・24・40 など）を `add x, a, a, lsl #k`（か `lsl`・`sub`）と `lsl #m` に
    （ir_mul_shape。前は movz と mul。構造体の配列の添字に多い）。
  - 4 バイトの読み出しと int の戻り値は、下位 32 ビットだけを使うなら符号拡張しない（ldrsw → ldr w、sxtw を
    出さない）。符号つきの計算に使うときは、その結果も下位 32 ビットだけのときだけ（でなければ広げ直しが要る）。
- 効果: get_entry_h のループの中 11 → 9 命令。2 世代目の .text 518,132 → 514,932 バイト。実機のカーネルの中の
  コンパイラ parse.c 525 → 523M、codegen.c 1,152 → 1,147M。Pi 5 の bench.sh で strhash 407 → 399ms（GCC -O2 402ms）、
  primes 1,051 → 1,025ms。
- 試験: cc/test-a64/gccmatch/arith_imm.c（いろいろな型の定数の掛け算・後置の増減・負の即値・int の戻り値と読み出しを
  64 ビットで使う形。GCC の -O0・-O2・-fwrapv で同じ出力、UBSan で未定義動作なし）。
  確かめ: cmp_o.sh、make cc-test、verify.sh（quick なし。すべて一致）。
- 次（この比べで差の大きかったもの）: ir_linear_scan（1,012 対 785）、ir_emit_inst1（431 対 223）、ir_mem2reg（711 対 505）、
  ir_alloc_nz・zone_malloc（確保）、ir_mark_low32、new_token。

## 15. switch の表引き（2026-11-27）

- cc/ir.inc の ir_switch_table（ir_switch から。二分探索 ir_switch_tree の前に試す）: case の範囲が IR_SWTAB_MIN（6）個以上で、
  下端から上端までの値の数が case の値の数の IR_SWTAB_DENSITY（3）倍まで・IR_SWTAB_MAX（4096）までなら、
  `idx = v - 下端` を符号なしで幅と比べて（CC_HI で default へ）、中なら IR_SWTAB で飛ぶ。
- IR_SWTAB（新しい終わりの命令）: a が表の番号、swtab[a] が後続の番号。後続は行き先ごとの入口のブロック（前任が 1 つ、
  最初の 1 つは default）。計算型の goto（IR_INDBR）と同じ形なので、SSA を抜けるときに辺を分けなくてよい。
- 出力（cc/ir_a64_emit.inc）: `adr x16, .L.swt.N`・`ldrsw x17, [x16, idx, lsl #2]`・`add x16, x16, x17`・`br x16`、続けて
  `.L.swt.N:` と行き先ごとの `.word .L.ir.F.B - .L.swt.N`（GCC と同じ相対の表。入口のブロックは ir_thread で飛ばした先）。
- 内蔵アセンブラ（cc/assembler.c）: `.word A - B` で A・B がまだ定義されていないラベルでもよいようにした（Expr の sub、
  FX_DIFF32。最後に同じセクションなら差を書く。dir_data の間だけ）。GNU as と同じバイト列。
- 効果: 2 世代目の .text 514,932 → 514,388 バイト。コンパイラの中で表引きになった switch 42 個（codegen.c 30・parse.c 6・
  assembler.c 5・tokenize.c 1。ir_emit_inst1・ir_simplify・ir_fold・ao_print など）。実機のカーネルの中のコンパイラ
  parse.c 523 → 523M、codegen.c 1,247 → 1,242M（0.4%。前後のカーネルを交互に 2 回ずつ。この日に実機の材料を
  送り直したので、codegen.c の数はそれまでの表より大きい）。小さかったのは、値が散らばった switch が表引きに
  ならないことと、Cortex-A53 が行き先の変わる br をあまり読み当てられないためと思われる（確かめていない）。
- 試験: cc/test-a64/switch_gen.py（make cc-test の 8d）に、型の端に接した密な switch（dense_lo・dense_hi）と
  case が 100・250 個のものを足した（種 8 つで表引き 61〜82 個、GCC と同じ。QEMU の cortex-a53 でも）。
  確かめ: cmp_o.sh（かけらの道と文字列の道の .o が同じ）、make cc-test、verify.sh（quick なし。すべて一致）。

## 16. switch のビットの表（2026-11-27。GCC の bit test と同じ考え方）

- cc/ir.inc の ir_switch_bits（ir_switch から、表引き ir_switch_table より先に試す）: 行き先が IR_SWBITS_TARGETS（3）個まで、
  いちばん小さい下端から大きい上端までが 64 個まで、case の値が IR_SWBITS_MIN（4）個以上なら、`idx = v - 下端` を符号なしで
  幅と比べて外れたら default へ、中なら行き先ごとに「その行き先になる値のビットを立てた定数」を `(定数 >> idx) & 1` で
  調べる。新しい命令は足さず、今の命令で組み立てる（`& 1` の分岐は 10 の tbz・tbnz になる: `movz`・`lsr`・`tbz`）。
  case が少ないときは並べ替えていないので、下端・上端は並びに頼らずに求める（ir_case_unsigned はいつも決める）。
- 例: `switch (op) { case B: case D: case G: case H: case M: case P: return 0; default: return 1; }` が
  `sub`・`cmp`・`b.hi`・`movz`・`lsr`・`tbz` の分岐 2 つに（前は二分探索）。
- 効果: コンパイラの中でビットの表になった switch 195 個（codegen.c 113・preprocess.c 47 など）。実機のカーネルの中の
  コンパイラは変わらない（parse.c 523〜524M、codegen.c 1,242〜1,243M。熱いところにない）。2 世代目の .text は
  514,388 → 515,284 バイト。文字の分類の switch（6 つずつ 2 つの行き先、幅 31）を乱数の文字 6.5 万字 × 200 回で
  回すと、実機で 344〜345 → 319〜321ms（前は表引き。7.5% 速い）。
- 試験: cc/test-a64/switch_gen.py に、いくつかの case が同じ本体を共有する形（shared）を足した（種 8 つでビットの表
  77〜107 個・表引き 60〜68 個、GCC と同じ。QEMU の cortex-a53 でも）。確かめ: cmp_o.sh、make cc-test、verify.sh
  （quick なし。すべて一致）。

## 17. 書き込みの前の拡張を省く（2026-11-27）と、残りの差

- cc/ir.inc の ir_simplify の IR_STORE: size バイトの書き込みの値が size バイト以上への拡張（IR_EXT）なら、拡張の前の値を
  書く（`uxtb w12, w13` → `strb w12` の uxtb がなくなる）。コンパイラの中の拡張 194 → 158 個、2 世代目の .text
  515,284 → 515,220 バイト。実機の速さは変わらない。確かめ: cmp_o.sh、make cc-test、verify.sh（すべて一致）。
- 14 の比べのやり直し（2026-11-27 の終わり。GCC 版 26,452・slimcc 版 36,211 の記録）で差の大きいもの:
  get_entry_h（835 対 270）、ir_linear_scan（1,022 対 780）、ir_mem2reg（858 対 623）、ir_emit_inst1（430 対 217）、
  ir_alloc_nz（592 対 394）、zone_malloc（418 対 227）、new_token（486 対 318）、ir_mark_low32（516 対 353）。
  - get_entry_h は、Pi 5 の perf annotate で時間の 6 割がバケツを読んだ直後（キャッシュに当たらない）。ループの形は
    GCC とほぼ同じで、差は GCC が get_entry_h・fnv_hash を呼び出し元にインライン展開していること（slimcc は
    ループのある関数を IR_INLINE_SIZE 12 では展開しない）。記録の数の 3 倍の差は、まだ説明できていない。
  - emit_le（129 対 34）は、ループの番地とずらす量を毎回 i から計算している（GCC は `+= 8` と、書いた後に番地を
    進める `strb w4, [x3], #1`）。これが「誘導変数の強度の削減」と「後置の番地の形」（4. の候補）。
