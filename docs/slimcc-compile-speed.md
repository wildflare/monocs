# slimcc のコンパイルを速くする（2026-11-21 の方針）

出すコードを良くする続きは `docs/slimcc-codegen.md`。この文書は、コンパイラ自身の速さ（コンパイルにかかる時間）。

## 1. 決めたこと（ユーザーと話して）

- 目的は **コンパイルを速くすること**（ユーザーの答え「速くしたい」、2026-11-21）。
- **ゼロから作り直さない**。前の半分（前処理・字句・構文解析・型。parse.c・preprocess.c・tokenize.c・type.c）は
  元の slimcc のまま残し、後ろの半分（中間表現から機械語まで）を段階的に作り替える。
  - 理由: 前の半分は C23 と GCC の拡張にほぼ完全に対応していて、SQLite・Lua・vim・QuickJS がそのまま動くのは
    このおかげ。作り直すと互換を試験で積み上げ直すことになり、危なく、しかもコンパイルの時間の 3 分の 1 ほど
    なので速さの得がいちばん小さい。
  - 著作権: slimcc は MIT（Rui Ueyama、Hsiang-Ying Fu）。求められるのは表示とライセンスの文を残すことだけなので、
    作り直す理由にはしない（ユーザーとの話で、作り直しの動機は「速くしたい」と確かめた）。
- **コンパイラをアセンブラで書かない**。時間を決めているのはアルゴリズムとメモリの使い方で、手で命令を選んで
  縮むのはせいぜい 1〜3 割。セルフホストと「Monocs の中で完結」の方針も崩れる。
- 2026-11-21 の時点で `cc/` は約 3.7 万行。元の slimcc と同じ行が約 1.6 万行（44%。ほとんど前の半分）、Monocs で
  書いた部分が約 1.6 万行（ir.inc・sched.inc・omp.inc・assembler.c・ld.c・memlink.c・libslimcc.c など）。

## 2. 今どこに時間を使っているか（2026-11-21、実機、カーネルの中のコンパイラ）

測り方: `cd /usr/src/kernel; perf record -e cycles -o /tmp/prof.txt cc -std=c23 -Icc -DSLIMCC_LIBRARY -c -o /dev/null cc/codegen.c`
（/usr/src/kernel は `nos.py pushdir build/selfsrc /usr/src/kernel` で新しくしてから）。関数名で大まかに分けた。

| 部分 | codegen.c | parse.c |
|---|---|---|
| 中間表現（最適化・レジスタの割り当て・出力の準備。ir_*） | 50% | 40% |
| 字句・前処理 | 19% | 16% |
| 構文解析・型・今までのコード生成 | 18% | 24% |
| C ライブラリ・確保・出力 | 10% | 12% |
| 命令の並べ替え（sch_*） | 3% | 3% |
| 内蔵アセンブラ | 0% | 5% |

- 中間表現の中は平らで、約 170 の関数に広がっている。上位（codegen.c）: ir_linear_scan 2.9%、ir_cleanup 2.6%、
  ir_resolve_operands 2.2%、ir_intervals 2.1%、ir_count_uses 1.9%、ir_alloc_nz 1.7%、ir_fingerprint 1.5%、
  ir_emit_inst1 1.5%、ir_mark_low32 1.4%、ir_block_use_def 1.2%、ir_dce 1.2%、ir_fixup 1.2%。
- 原因は作りそのもの: 命令をポインタでつないだリストで持ち、関数ごとに全体を何十回もたどり直す（置き換えた値の
  指し直し ir_resolve_operands、使う数の数え直し ir_count_uses、変化の確かめ ir_fingerprint、片付け ir_cleanup
  など）。1 つを直しても数 % ずつしか縮まない。
- 内蔵アセンブラはもうほとんど時間を使わない（中間表現の関数は文字列を経ないかけらの道になったため）。
- 速さの数字（実機、`perf -e cycles cc -c -o /dev/null`、9b6873b のカーネル）: parse.c 577M、codegen.c 1173M サイクル。
- カーネルの中のコンパイラは slimcc が作ったもので、GCC 版より約 1.5 倍遅い。出すコードを良くすると、コンパイラ
  自身も速くなる（セルフホストなので 2 回効く）。

## 3. 進め方

1. **段階 1（小さく、すぐできる。ユーザーの確認は要らない）**: 全体のたどり直しを減らす。
   - 最適化の繰り返しの止め方を、指紋（ir_fingerprint）から「変えたら印を付ける」に。
   - 使う数（ir_count_uses）を、変えた所だけ直す・要るときだけ数える。
   - 置き換えの指し直し（ir_resolve_operands）と片付け（ir_cleanup）の回数を減らす。
   - 確保（ir_alloc_nz・memset）を減らす。
   - 見込み: 中間表現の部分で 1〜2 割、全体で 5〜10%。
2. **段階 2（大きい。始める前にユーザーに確認する。CLAUDE.md の「大きな設計変更は実施前に確認」）**:
   中間表現の持ち方を、配列と番号の形に作り替える（命令を連続した配列に置き、オペランドは番号で持つ。置き換えは
   その場で書き換え、指し直しの段をなくす）。キャッシュに乗りやすくなる。ir.inc（約 7,000 行）のほぼ全部に
   手が入る。見込み: 中間表現の部分で 2〜3 倍、全体で 2〜3 割以上。
3. **並行して**: slimcc が出すコードを良くし続ける（`docs/slimcc-codegen.md`）。

段階 1 の効き目を見てから、段階 2 に進むかを決める（2026-11-21、ユーザーに提案したところ）。

## 4. 確かめ方

- 出力が変わらないはずの変更（段階 1 のほとんど・段階 2）は、`sh tools/slimcc/cmp_s.sh`（HEAD の slimcc と -S が
  同じ）と `sh tools/slimcc/cmp_o.sh`（かけらの道と文字列の道の .o が同じ）で、1 バイトも変わらないこと。
- そのうえで `make cc-test`、`sh tools/test/verify.sh`（quick なし）、QEMU の試験、実機の hwcheck
  （`docs/slimcc-codegen.md` の 3. と同じ）。
- 速さは、Pi 5 ではコンパイラの命令数（`perf stat -e instructions cc/slimcc -c ...`。揺れが小さい）、実機では
  上の 2. の cycles を前後のカーネルで交互に何回か（起動から 60 秒の内は避ける。`clock` の measured が 1000MHz）。
  大きいファイル（QuickJS の quickjs.c、約 6 万行）も見る。
