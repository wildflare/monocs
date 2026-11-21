# memcpy などのメモリ操作について（メモ）

2026-11-05 の作業と質問のまとめ。

## Monocs の現状

- picolibc は slimcc-a64 でビルドしている（`make picolibc-slimcc`、`tools/picolibc/build.py`）。
- `memcpy`・`memset`・`strlen`・`strcmp` などは、picolibc のアセンブリ版（`libc/machine/aarch64/*.S`）を使う。
  - アセンブリ版は、`ldp`/`stp`（16 バイトずつ）や NEON の `ld1`、q レジスタの `ldp`/`stp`（1 命令で 32 バイト以上）で、まとめて読み書きする。
  - picolibc は `__ARM_NEON` と `__ARM_FEATURE_UNALIGNED` が定義されているときだけアセンブリ版を選ぶ。slimcc はこれらを定義しないので、picolibc のビルドにだけ `-D` で渡している。
  - アプリには定義しない。`__ARM_NEON` は「NEON の組み込み関数（`arm_neon.h`）が使える」という意味も持つが、slimcc はそれに対応していないため。
- 内蔵アセンブラは、picolibc の .S が使う SIMD 命令に対応している（GNU as と一致することを `cc/test-a64/asm/neon.asm` などで確認）。

### 速さ（QEMU raspi3b での測定。実機の値ではない）

| | memcpy 64MB | memset 64MB | strlen 64MB |
|---|---|---|---|
| GCC でビルドした picolibc | 498 ms | 38 ms | 157 ms |
| slimcc でビルドした picolibc（C 版） | 1121 ms | 112 ms | 167 ms |
| slimcc でビルドした picolibc（アセンブリ版、現在） | 505 ms | 37 ms | 158 ms |

C 版は、slimcc が最適化をしないこともあって遅い。アセンブリ版にすると、GCC 版と同じ速さになる。

## メモリコピー専用の命令（FEAT_MOPS）

- ARMv8.8-A（ARMv9.3-A）で、メモリのコピーと埋めをする命令が追加された（FEAT_MOPS）。
  - コピー: `CPYP`（前処理）・`CPYM`（本体）・`CPYE`（後処理）の 3 命令を順に並べる。どんな大きさでもこの 3 命令で終わる。
  - 重なりを正しく扱う版（`memmove` 相当）と、扱わない版（`CPYFP`/`CPYFM`/`CPYFE`）がある。
  - 埋め（`memset` 相当）: `SETP`・`SETM`・`SETE`。
  - 最適な方法（一度に動かす量など）は CPU が選ぶ。x86 の `rep movsb` に近い考え方。
- Zero 2 W の Cortex-A53（ARMv8.0-A）と、Pi 5 の Cortex-A76（ARMv8.2-A）にはない。どの製品のどのコアが対応しているかは確認していない。

### 対応している CPU で、自動的に速い方が使われるか

使われない。CPU が既存の命令列を置き換えることはないので、プログラムの側で MOPS の命令を使う必要がある。方法は 2 通り。

1. コンパイル時に決める: `-march=armv8.8-a+mops` などでビルドすると、コンパイラが `memcpy` を MOPS の命令にできる。その実行ファイルは、MOPS のない CPU では未定義命令の例外になる。
2. 実行時に判定して切り替える: 起動時に CPU の機能を調べ、`memcpy` の実装を選ぶ。
   - Linux の glibc は ifunc でこれを行っている。MOPS 版も含まれていると記憶しているが、どの版からかは確認していない。
   - picolibc に同じ仕組みがあるかは確認していない（ベアメタル向けなので、ビルド時に決める作りだと思われる）。

Monocs で行うなら、起動時に `ID_AA64ISAR2_EL1` の MOPS の欄を読み、関数ポインタで実装を切り替える形になる。Zero 2 W には MOPS がないので、今は必要ない。
