// slimcc-a64 をライブラリとして使う入口（ドライバ main.c の代わりにリンクする）
//
// main.c の cc1 と同じ順（予定義マクロ → 前処理 → 構文解析 → コード生成）でコンパイルし、
// コード生成の結果（アセンブリのテキスト）をそのまま内蔵アセンブラにかける。
// 各ファイルの状態は、コンパイルの前に *_reset で初期化し直す。
// エラーは、error() → cleanup_exit() から longjmp で slimcc_compile に戻す。

#define SLIMCC_NO_ZONE
#include <stdatomic.h>
#include "slimcc.h"
#include "libslimcc.h"
#include <setjmp.h>
#include <sys/stat.h>

TLS static jmp_buf error_jb;
TLS static bool in_compile;
TLS static StringArray sysincl_paths;
TLS static int user_incl_cnt;

//
// ドライバ（main.c）が提供していた関数
//

void cleanup_exit(int status) {
  if (in_compile)
    longjmp(error_jb, 1);
  exit(status);
}

// ファイルがあるかの覚え（slimcc_cache_files の間。どのコアからも使い、exist_lock で守る）。
// インクルードの検索は -I のディレクトリを順に調べるので、同じヘッダーを読む .c をまとめてコンパイルすると、
// 同じパスを何度も調べる
typedef struct ExistEntry ExistEntry;
struct ExistEntry {
  ExistEntry *next;
  bool exists;
  char path[];
};
#define EXIST_BUCKETS 1024
static ExistEntry *exist_cache[EXIST_BUCKETS];

static unsigned path_hash(const char *s) {
  unsigned h = 2166136261u; // FNV-1a
  for (; *s; s++)
    h = (h ^ (unsigned char)*s) * 16777619u;
  return h % EXIST_BUCKETS;
}
static int exist_users;
static atomic_flag exist_lock = ATOMIC_FLAG_INIT;
TLS static bool caching;

static void exist_lock_take(void) {
  while (atomic_flag_test_and_set_explicit(&exist_lock, memory_order_acquire))
    ;
}

static void exist_lock_give(void) {
  atomic_flag_clear_explicit(&exist_lock, memory_order_release);
}

// コンパイルで調べて、なかったパス（ヘッダーの検索・__has_include。absent_reset から次まで。コアごと）。
// キャッシュは、これらが今もないことも確かめる（前はなかった同じ名前のヘッダーが、先に探す場所に増えたとき）
TLS static char **absent_paths;
TLS static int nabsent, capabsent;

void absent_reset(void) {
  for (int i = 0; i < nabsent; i++)
    free(absent_paths[i]);
  nabsent = 0;
}

static void absent_add(const char *path) {
  for (int i = 0; i < nabsent; i++)
    if (!strcmp(absent_paths[i], path))
      return;
  if (nabsent == capabsent) {
    capabsent = capabsent ? capabsent * 2 : 64;
    absent_paths = realloc(absent_paths, sizeof(char *) * capabsent);
  }
  absent_paths[nabsent++] = strdup(path);
}

static bool file_exists2(const char *path);

bool file_exists(const char *path) {
  bool e = file_exists2(path);
  if (!e && in_compile)
    absent_add(path);
  return e;
}

static bool file_exists2(const char *path) {
  unsigned h = path_hash(path);
  if (caching) {
    exist_lock_take();
    ExistEntry *e = exist_cache[h];
    for (; e; e = e->next)
      if (!strcmp(e->path, path))
        break;
    exist_lock_give();
    if (e)
      return e->exists;
  }
  struct stat st;
  bool exists = !stat(path, &st);
  if (caching) {
    size_t n = strlen(path) + 1;
    ExistEntry *e = malloc(sizeof(ExistEntry) + n);
    if (e) {
      memcpy(e->path, path, n);
      e->exists = exists;
      exist_lock_take();
      e->next = exist_cache[h];
      exist_cache[h] = e;
      exist_lock_give();
    }
  }
  return exists;
}

void slimcc_cache_files(bool on) {
  if (on == caching)
    return;
  caching = on;
  exist_lock_take();
  exist_users += on ? 1 : -1;
  if (exist_users == 0) {
    for (int i = 0; i < EXIST_BUCKETS; i++) {
      while (exist_cache[i]) {
        ExistEntry *next = exist_cache[i]->next;
        free(exist_cache[i]);
        exist_cache[i] = next;
      }
    }
  }
  exist_lock_give();
  file_cache_set(on);
}

void add_include_path(StringArray *arr, const char *path) {
  for (int i = 0; i < arr->len; i++)
    if (!strcmp(arr->data[i], path))
      return;
  strarray_push(arr, path);
}

bool in_sysincl_path(int idx) {
  return idx >= user_incl_cnt;
}

bool ignore_missing_dep(const char *path, const char *filename, Token *tok) {
  if (!path) {
    // 直し方の案内（libslimcc.h の slimcc_include_hint。Monocs では -l名前 の付け忘れ）
    const char *hint = slimcc_include_hint ? slimcc_include_hint(filename) : NULL;
    if (tok) {
      if (hint)
        error_tok(tok, "file not found\n%s", hint);
      error_tok(tok, "file not found");
    }
    if (hint)
      error("`%s` file not found\n%s", filename, hint);
    error("`%s` file not found", filename);
  }
  return false;
}

void add_dep_file(const char *path, bool is_sys) {
  (void)path;
  (void)is_sys;
}

char *find_dir_w_file(const char *pattern) {
  (void)pattern;
  return NULL;
}

void run_assembler_gnustyle(StringArray *args, const char *input, const char *output) {
  (void)args;
  (void)input;
  (void)output;
  error("libslimcc: an external assembler is not available");
}

void run_linker_gnustyle(StringArray *paths, StringArray *inputs, const char *output,
                         const char *ldso_path, const char *libpath, const char *gcclibpath) {
  (void)paths;
  (void)inputs;
  (void)output;
  (void)ldso_path;
  (void)libpath;
  (void)gcclibpath;
  error("libslimcc: an external linker is not available");
}

//
// コンパイル
//

// "c17"、"gnu2y" などを opt_std と is_iso_std にする（main.c の set_std と同じ値）
static void set_std(const char *s) {
  is_iso_std = false;
  opt_std = STD_C17;
  if (!s)
    return;
  if (!strncmp(s, "gnu", 3)) {
    s += 3;
  } else if (s[0] == 'c') {
    is_iso_std = true;
    s += 1;
  } else {
    error("unknown c standard '%s'", s);
  }
  if (!strcmp(s, "89") || !strcmp(s, "90"))
    opt_std = STD_C89;
  else if (!strcmp(s, "99"))
    opt_std = STD_C99;
  else if (!strcmp(s, "11"))
    opt_std = STD_C11;
  else if (!strcmp(s, "17") || !strcmp(s, "18"))
    opt_std = STD_C17;
  else if (!strcmp(s, "23"))
    opt_std = STD_C23;
  else if (!strcmp(s, "2y") || !strcmp(s, "29"))
    opt_std = STD_C2Y;
  else
    error("unknown c standard");
}

static void reset_all(void) {
  tokenize_reset();
  preprocess_reset();
  parse_reset();
  type_reset();
  codegen_reset();
}

typedef enum { SRC_C, SRC_PP_ASM, SRC_ASM } SrcKind;

static char *read_text_file(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "%s: cannot open: %s\n", path, strerror(errno));
    return NULL;
  }
  char *buf = NULL;
  size_t len = 0;
  FILE *out = open_memstream(&buf, &len);
  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0)
    fwrite(tmp, 1, n, out);
  fclose(fp);
  fclose(out);
  return buf;
}

// コンパイルして AsmObj にする。object なら slimcc -c と同じ扱い（位置独立にしない）。
// そうでなければメモリ上の実行用（定義していない変数は GOT 経由。memlink.c が GOT を作る。
// 外部の変数がどれだけ離れていても届くようにするため。__PIE__ などのマクロは定義しない）
static AsmObj *compile_body(const char *path, const char *source, const SlimccOptions *opt, bool object,
                            SrcKind kind) {
  // main.c のオプションの既定値と後処理に合わせる
  opt_fpic = 0;
  opt_fpie = object ? 0 : 2;
  opt_no_pic_macros = !object;
  opt_pie = false;
  opt_g = false;
  opt_E = opt_dM = opt_P = opt_cc1_asm_pp = false;

  if (kind == SRC_ASM) {
    char *text = source ? NULL : read_text_file(path);
    if (!source && !text)
      return NULL;
    in_compile = true;
    if (setjmp(error_jb)) {
      in_compile = false;
      free(text);
      return NULL;
    }
    AsmObj *obj = asm_assemble(source ? source : text, path);
    in_compile = false;
    free(text);
    return obj;
  }

  in_compile = true;
  if (setjmp(error_jb)) {
    in_compile = false;
    return NULL;
  }
  set_std(opt ? opt->std : NULL);
  in_compile = false;
  opt_gnu_keywords = !is_iso_std;

  reset_all();

  // -D（コマンドラインの順に、define_cli_macros が定義する）
  macrodefs.len = 0;
  if (opt && opt->defines)
    for (const char *const *d = opt->defines; *d; d++)
      macrochange_push(&macrodefs, *d, true);
  if (opt && opt->undefines)
    for (const char *const *u = opt->undefines; *u; u++)
      macrochange_push(&macrodefs, *u, false);
  opt_func_sections = opt && opt->function_sections;
  opt_openmp = opt && opt->openmp;
  opt_sched = !(opt && opt->no_schedule);
  opt_unroll = !(opt && opt->no_unroll);
  opt_strict_aliasing = !(opt && opt->no_strict_aliasing);

  // インクルードパス: 追加のパス、続いてシステムのパス（-isystem、プラットフォームのパス）。
  // Linux 用のドライバ（main.c の build_incl_paths）と同じく、システムのパスと同じ -I は除く
  include_paths.len = 0;
  iquote_paths.len = 0;
  if (opt && opt->include_paths)
    for (const char *const *p = opt->include_paths; *p; p++)
      add_include_path(&include_paths, *p);
  sysincl_paths.len = 0;
  if (opt && opt->system_include_paths)
    for (const char *const *p = opt->system_include_paths; *p; p++)
      add_include_path(&sysincl_paths, *p);
  if (!(opt && opt->nostdinc)) {
    TLS static StringArray platform_paths;
    if (!platform_paths.len)
      platform_stdinc_paths(&platform_paths);
    for (int i = 0; i < platform_paths.len; i++)
      add_include_path(&sysincl_paths, platform_paths.data[i]);
  }
  int n = 0;
  for (int i = 0; i < include_paths.len; i++) {
    bool match = false;
    for (int j = 0; j < sysincl_paths.len && !match; j++)
      match = !strcmp(sysincl_paths.data[j], include_paths.data[i]);
    if (!match)
      include_paths.data[n++] = include_paths.data[i];
  }
  include_paths.len = n;
  user_incl_cnt = include_paths.len;
  for (int i = 0; i < sysincl_paths.len; i++)
    strarray_push(&include_paths, sysincl_paths.data[i]);

  set_memory_file(source ? path : NULL, source);

  // コンパイル中の確保はゾーンから行い、最後にまとめて捨てる（zone.c）
  TLS static char *asm_text;
  asm_text = NULL;
  TLS static FILE *out;
  out = NULL;

  in_compile = true;
  if (setjmp(error_jb)) {
    in_compile = false;
    if (out)
      fclose(out);
    free(asm_text);
    set_memory_file(NULL, NULL);
    reset_all();
    alloc_reset();
    zone_end();
    opt_E = opt_cc1_asm_pp = false;
    return NULL;
  }
  zone_begin();
  ir_mem_reset();
  slimcc_frag_free_all();
  opt_direct = true;        // 同じ関数の中で内蔵アセンブラにかけるので、中間表現の関数はかけらで渡す

  bool is_asm_pp = kind == SRC_PP_ASM;
  if (is_asm_pp)
    opt_E = opt_cc1_asm_pp = true;

  arena_on(&cc1_arena);
  arena_on(&pp_arena);

  init_macros();
  platform_init_cc1();
  define_cli_macros(is_asm_pp);

  StringArray none = {0}, forced = {0};
  if (opt && opt->forced_includes)
    for (const char *const *p = opt->forced_includes; *p; p++)
      strarray_push(&forced, (char *)*p);
  deps_reset();
  absent_reset();
  Token *tok = preprocess(path, &forced, &none);

  // 出力（アセンブリのテキスト）は通常のヒープ（open_memstream）に置く
  size_t asm_len;
  out = open_memstream(&asm_text, &asm_len);
  if (is_asm_pp) {
    // .S: 前処理の結果をそのまま内蔵アセンブラにかける（main.c の cc1 の -E と同じ）
    print_tokens(tok, out);
    arena_off(&pp_arena);
  } else {
    tok = prepare_parse(tok);
    arena_off(&pp_arena);
    Obj *prog = parse(tok);
    codegen(prog, out);
  }
  fclose(out);
  out = NULL;
  arena_off(&cc1_arena);
  opt_E = opt_cc1_asm_pp = false;

  // 前処理・構文解析・コード生成の状態を捨ててから、内蔵アセンブラにかける
  size_t zone_bytes = zone_total();
  set_memory_file(NULL, NULL);
  reset_all();
  alloc_reset();
  zone_end();
  if (slimcc_mem_report && zone_bytes >= SLIMCC_MEM_REPORT) {
    const char *big_fn;
    size_t big_bytes;
    ir_mem_biggest(&big_fn, &big_bytes);
    slimcc_mem_report(path, zone_bytes, big_fn, big_bytes);
  }

  AsmObj *obj = asm_assemble(asm_text, path);
  slimcc_frag_free_all();   // かけら（cc/ir.inc）は内蔵アセンブラが読んだら要らない
  in_compile = false;

  free(asm_text);
  asm_text = NULL;
  return obj;
}

// slimcc_mem_report は弱いシンボル（libslimcc.h）: 使う側が定義していなければ NULL（変数を持たない: コンパイラの中の
// 書き換える状態はコアごと（TLS）にする決まり）

// 前のコンパイルが途中で止められた（コンパイラの中の例外。longjmp で compile_body の後始末を通らずに抜けた）
// ときは、残った状態をできるだけ捨ててから始める。ゾーンが動いたままだと、compile_body がゾーンを始める前の
// 確保（-I の表など、次のコンパイルまで使うもの）がゾーンに入り、終わりに捨てられて壊れる。
// どこで止められても元に戻るとは限らない（状態が多い）ので、Monocs はコンパイルの途中では Ctrl-C などで
// 止めない（os/slimcc/cmd.c の slimcc_cmd_app）。止められたコンパイルが確保したものの一部は解放されない
TLS static bool compile_busy;

static AsmObj *compile(const char *path, const char *source, const SlimccOptions *opt, bool object,
                       SrcKind kind) {
  if (compile_busy) {
    in_compile = false;
    set_memory_file(NULL, NULL);
    reset_all();
    alloc_reset();
    zone_end();
    opt_E = opt_cc1_asm_pp = false;
  }
  compile_busy = true;
  AsmObj *obj = compile_body(path, source, opt, object, kind);
  compile_busy = false;
  return obj;
}

const char *const *slimcc_deps(int *n) {
  return deps_get(n);
}

const char *const *slimcc_absent(int *n) {
  *n = nabsent;
  return (const char *const *)absent_paths;
}

bool slimcc_cacheable(void) {
  return !deps_is_volatile();
}

bool slimcc_obj_save(SlimccObj *obj, FILE *out) {
  return asm_obj_save((AsmObj *)obj, out);
}

SlimccObj *slimcc_obj_load(FILE *in) {
  return (SlimccObj *)asm_obj_load(in);
}

SlimccObj *slimcc_obj_load_mem(const void *p, size_t n) {
  return (SlimccObj *)asm_obj_load_mem(p, n);
}

SlimccObj *slimcc_compile(const char *path, const char *source, const SlimccOptions *opt) {
  return (SlimccObj *)compile(path, source, opt, false, SRC_C);
}

static bool has_suffix(const char *s, const char *suffix) {
  size_t n = strlen(s), m = strlen(suffix);
  return n >= m && !strcmp(s + n - m, suffix);
}

int slimcc_compile_file(const char *path, const char *output, const SlimccOptions *opt) {
  SrcKind kind = has_suffix(path, ".S") ? SRC_PP_ASM : has_suffix(path, ".s") ? SRC_ASM : SRC_C;
  AsmObj *obj = compile(path, NULL, opt, true, kind);
  if (!obj)
    return 1;
  FILE *fp = fopen(output, "wb");
  if (!fp) {
    fprintf(stderr, "%s: cannot open: %s\n", output, strerror(errno));
    asm_free(obj);
    return 1;
  }
  asm_write_elf(obj, fp);
  asm_free(obj);
  if (fclose(fp)) {
    fprintf(stderr, "%s: write error\n", output);
    return 1;
  }
  return 0;
}

void slimcc_free_obj(SlimccObj *obj) {
  asm_free((AsmObj *)obj);
}
