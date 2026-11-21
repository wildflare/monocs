// slimcc-a64 をライブラリとして使うための API（Monocs のカーネルや、Linux 上の確認用プログラムから使う）
//
// C のソースをメモリ上でコンパイルし（slimcc_compile）、メモリに配置してリロケーションを解決し
// （slimcc_link）、関数のアドレスを得る（slimcc_symbol）。ドライバ（main.c）のように
// プロセスの起動や一時ファイルは使わない。何度呼んでもよい。
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct SlimccObj SlimccObj;
typedef struct SlimccImage SlimccImage;

typedef struct {
  const char *const *include_paths; // -I に相当する追加のパス（NULL で終わる。NULL 可）
  const char *const *defines;       // -D に相当する "NAME" か "NAME=VALUE"（NULL で終わる。NULL 可）
  const char *std;                  // -std= に相当する "c11"、"gnu17"、"c23"、"gnu2y" など（NULL なら gnu17）
  const char *const *undefines;     // -U に相当する名前（NULL で終わる。NULL 可。-D の後に反映する）
  const char *const *system_include_paths; // -isystem に相当するパス（NULL で終わる。NULL 可）
  bool nostdinc;                    // -nostdinc（プラットフォームのシステムのパスを使わない）
  bool function_sections;           // -ffunction-sections
  bool openmp;                      // -fopenmp（#pragma omp を使う。ランタイムは Monocs の os/libc/omp.c）
  bool no_schedule;                 // -fno-schedule-insns2（命令を並べ替えない。cc/sched.inc）
  bool no_strict_aliasing;          // -fno-strict-aliasing（型による別名の判定をしない。cc/ir.inc）
  bool no_unroll;                   // -fno-unroll-loops（ループを展開しない。cc/ir.inc）
  const char *const *forced_includes; // -include FILE（ソースの頭で #include "FILE" したのと同じ。NULL で終わる。NULL 可）
} SlimccOptions;

// コンパイルがメモリを多く使った（ゾーン（1 回のコンパイルの確保）の合計が SLIMCC_MEM_REPORT バイト以上）ときに呼ぶ
// 関数。使う側（Monocs のカーネル。os/slimcc/run.c が dmesg に書く）が定義すれば呼ばれる（弱いシンボル。2026-11-20）。
// ファイル、合計、中間表現でいちばん多く使った関数とその量（バイト）
#define SLIMCC_MEM_REPORT (64u << 20)
__attribute__((weak)) void slimcc_mem_report(const char *path, size_t zone_bytes, const char *big_fn,
                                             size_t big_fn_bytes);

// コンパイルが関数を 1 つ出し終えるたびに呼ぶ関数（弱いシンボル。使う側が定義すれば呼ばれる。2026-11-21。Monocs の
// カーネルが、時間のかかるコンパイルの進み具合を画面に出す: os/slimcc/run.c）。どのコアからも呼ばれる
__attribute__((weak)) void slimcc_progress(void);

// #include のファイルが見つからなかったときに呼ぶ関数（弱いシンボル。2026-11-21）。使う側（Monocs のカーネル。
// os/slimcc/run.c）が、直し方の案内（「note: … add -lpwm」など。改行なしの 1 行）を返せば、エラーの文に添える。
// 案内がなければ NULL。どのコアからも呼ばれる（返す文字列は、そのコアが次に呼ぶまで使える）
__attribute__((weak)) const char *slimcc_include_hint(const char *filename);

// path のファイル、または source（NULL でなければ、path はファイル名の表示だけに使う）をコンパイルする。
// エラーなら、メッセージを stderr に出して NULL を返す
SlimccObj *slimcc_compile(const char *path, const char *source, const SlimccOptions *opt);
void slimcc_free_obj(SlimccObj *obj);

// path（.c、.S、.s）をコンパイルし、ELF の .o を output に書く（slimcc -c と同じ結果）。
// 成功なら 0。エラーならメッセージを stderr に出して 1
int slimcc_compile_file(const char *path, const char *output, const SlimccOptions *opt);

// 外部のシンボル（関数・変数）のアドレスを返す。見つからなければ NULL。
// TLS の変数（initial-exec の参照）は "tls:名前" で呼ばれ、TP（TPIDR_EL0）からのオフセットを返す
typedef void *SlimccResolver(const char *name, void *ctx);

// 実行できるメモリを確保・解放する関数
typedef void *SlimccAlloc(size_t size);
typedef void SlimccFree(void *p, size_t size);

// obj を alloc で確保したメモリに配置し、リロケーションを解決する。エラーなら NULL
SlimccImage *slimcc_link(SlimccObj *obj, SlimccResolver *resolve, void *ctx,
                         SlimccAlloc *alloc, SlimccFree *free_mem);
// 最後の slimcc_compile（呼んだコアで）が読んだファイル（ソースとヘッダー。開いたときのパス）。
// 次のコンパイルまで有効
const char *const *slimcc_deps(int *n);
// 最後の slimcc_compile が調べて、なかったパス（ヘッダーの検索など）。次のコンパイルまで有効
const char *const *slimcc_absent(int *n);
// 最後の slimcc_compile の結果が、読んだファイル（とその有無）だけで決まるか（__DATE__・__TIME__・
// __TIMESTAMP__ を使ったら false）
bool slimcc_cacheable(void);

// コンパイルした結果をファイルに書く・読む（キャッシュ用。形式は assembler.c の asm_obj_save）。
// 読み込みは、壊れていれば NULL
bool slimcc_obj_save(SlimccObj *obj, FILE *out);
SlimccObj *slimcc_obj_load(FILE *in);
// メモリの上の [p, p + n) から読む（FILE より速い: fmemopen の fread は 1 バイトずつ読む。2026-11-20）
SlimccObj *slimcc_obj_load_mem(const void *p, size_t n);

// on の間（呼んだコアで）、読んだファイルの中身と、ファイルがあるかを覚えておき、次のコンパイルで使う
// （同じヘッダーを読む .c をまとめてコンパイルするとき用。間にファイルが変わらないこと）。off で捨てる
void slimcc_cache_files(bool on);

// 複数のオブジェクトを 1 つのプログラムとしてつなぐ（STB_LOCAL のシンボルはそれぞれのオブジェクトの中だけで引く）
SlimccImage *slimcc_link_many(SlimccObj *const *objs, int nobj, SlimccResolver *resolve, void *ctx,
                              SlimccAlloc *alloc, SlimccFree *free_mem);
void *slimcc_symbol(SlimccImage *img, const char *name);
// addr を含む（addr 以下でいちばん近い）シンボルの名前（img の外なら NULL）。*off に addr との差（perf record）
const char *slimcc_symbol_at(SlimccImage *img, const void *addr, size_t *off);
void slimcc_free_image(SlimccImage *img);
// 配置したメモリ（コード・データ・BSS がひと続き）の場所と大きさ（Monocs がつないだ結果を覚えて使い回すため）
void slimcc_image_range(SlimccImage *img, void **mem, size_t *size);
