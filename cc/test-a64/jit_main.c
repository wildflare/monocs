// Linux 上で libslimcc（メモリ上でのコンパイル・リンク・実行）を確かめるプログラム
//
//   slimcc-jit [-I dir]... [-D def]... [-std=c17 など] [-repeat N] [-c] file.c... [--] [args...]
//   .c で終わる引数が続く間はソース（複数なら slimcc_link_many で 1 つのプログラムにする）
//   -cache: ファイルの中身と字句解析の結果を覚えて使い回す（slimcc_cache_files。Monocs の run DIR と同じ）
//   -roundtrip: コンパイルした結果をいったん書いて読み戻したもの（slimcc_obj_save・load）をリンクする
//   -odir DIR: コンパイルしたものを DIR/名前.o（ELF）に書く（-c と使う。-cache のありなしで比べる）
//   -c: コンパイルだけ行う
//   -o out.o: ファイルを ELF の .o にする（slimcc_compile_file。.c、.S、.s）
//   slimcc-jit-monocs（Monocs のプラットフォーム）では --sysroot=DIR で DIR/lib/slimcc/include と
//   DIR/include をシステムのヘッダーにする
//
// file.c をコンパイルしてメモリに配置し、main(argc, argv) を呼んで、その戻り値で終了する。
// 外部のシンボルは dlsym で探す（このプログラムは -rdynamic でリンクし、test/host/common.c を含む）。
// -repeat N: 同じファイルを N 回コンパイルし、毎回 ELF の出力が同じであることを確かめる
//            （ライブラリの状態が正しく初期化されているかの確認）。メモリ使用量も表示する

#include "../slimcc.h"
#include "../libslimcc.h"
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/resource.h>

// long double（binary128）の演算に使う libgcc の関数。動的リンクでは見えないことがあるので、
// このプログラムにリンクして表から渡す（Monocs の OS API 表と同じ）
#define LIBGCC_TF(X) \
  X(__addtf3) X(__subtf3) X(__multf3) X(__divtf3) X(__extenddftf2) X(__extendsftf2) \
  X(__trunctfdf2) X(__trunctfsf2) X(__fixtfsi) X(__fixtfdi) X(__fixunstfsi) X(__fixunstfdi) \
  X(__floatsitf) X(__floatditf) X(__floatunsitf) X(__floatunditf) X(__eqtf2) X(__netf2) \
  X(__lttf2) X(__letf2) X(__gttf2) X(__getf2)
#define DECL(f) extern char f[];
LIBGCC_TF(DECL)
#define ENTRY(f) {#f, f},
static const struct {
  const char *name;
  void *addr;
} libgcc_syms[] = {LIBGCC_TF(ENTRY)};

static void *resolve(const char *name, void *ctx) {
  (void)ctx;
  for (size_t i = 0; i < sizeof(libgcc_syms) / sizeof(*libgcc_syms); i++)
    if (!strcmp(name, libgcc_syms[i].name))
      return libgcc_syms[i].addr;
  return dlsym(RTLD_DEFAULT, name);
}

static void *alloc_exec(size_t n) {
  void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static void free_exec(void *p, size_t n) {
  munmap(p, n);
}

static char *elf_bytes(SlimccObj *obj, size_t *len) {
  char *buf;
  FILE *f = open_memstream(&buf, len);
  asm_write_elf((AsmObj *)obj, f);
  fclose(f);
  return buf;
}

// -odir: DIR/名前.o に ELF を書く
static void write_odir(const char *odir, const char *name, SlimccObj *obj) {
  if (!odir)
    return;
  const char *b = strrchr(name, '/');
  char out[4096];
  snprintf(out, sizeof(out), "%s/%s.o", odir, b ? b + 1 : name);
  size_t len;
  char *e = elf_bytes(obj, &len);
  FILE *f = fopen(out, "wb");
  fwrite(e, 1, len, f);
  fclose(f);
  free(e);
}

// 書いて読み戻す（-roundtrip）
static SlimccObj *round_trip(SlimccObj *obj) {
  char *buf;
  size_t len;
  FILE *f = open_memstream(&buf, &len);
  if (!slimcc_obj_save(obj, f)) {
    fprintf(stderr, "slimcc-jit: save failed\n");
    exit(1);
  }
  fclose(f);
  slimcc_free_obj(obj);
  f = fmemopen(buf, len, "r");
  SlimccObj *o = slimcc_obj_load(f);
  fclose(f);
  free(buf);
  if (!o) {
    fprintf(stderr, "slimcc-jit: load failed\n");
    exit(1);
  }
  return o;
}

static long max_rss_kb(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

int main(int argc, char **argv) {
  // ヘッダー（slimcc_headers）は、このプログラムと同じディレクトリから探す
  argv0 = argv[0];

  const char *incs[64];
  const char *defs[64];
  int ninc = 0, ndef = 0, repeat = 1;
  bool compile_only = false;
  const char *output = NULL;
  const char *std = NULL;
  const char *odir = NULL;
  bool roundtrip = false;
  int i = 1;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "-I") && i + 1 < argc)
      incs[ninc++] = argv[++i];
    else if (!strncmp(argv[i], "-I", 2))
      incs[ninc++] = argv[i] + 2;
    else if (!strcmp(argv[i], "-D") && i + 1 < argc)
      defs[ndef++] = argv[++i];
    else if (!strncmp(argv[i], "-D", 2))
      defs[ndef++] = argv[i] + 2;
    else if (!strncmp(argv[i], "-std=", 5))
      std = argv[i] + 5;
    else if (!strcmp(argv[i], "--tmpflagstdgnu2y"))
      std = "gnu2y";
    else if (!strcmp(argv[i], "-c"))
      compile_only = true;
    else if (!strcmp(argv[i], "-o") && i + 1 < argc)
      output = argv[++i];
#ifdef SLIMCC_JIT_MONOCS
    else if (!strncmp(argv[i], "--sysroot=", 10)) {
      extern TLS const char *slimcc_sysroot;
      slimcc_sysroot = argv[i] + 10;
    }
#endif
    else if (!strcmp(argv[i], "-roundtrip"))
      roundtrip = true;
    else if (!strcmp(argv[i], "-cache"))
      slimcc_cache_files(true);
    else if (!strcmp(argv[i], "-odir") && i + 1 < argc)
      odir = argv[++i];
    else if (!strcmp(argv[i], "-repeat") && i + 1 < argc)
      repeat = atoi(argv[++i]);
    else
      break;
  }
  if (i >= argc) {
    fprintf(stderr, "usage: slimcc-jit [-I dir] [-D def] [-repeat N] file.c [args...]\n");
    return 2;
  }
  incs[ninc] = NULL;
  defs[ndef] = NULL;
  SlimccOptions opt = {.include_paths = incs, .defines = defs, .std = std};
  const char *path = argv[i];
  if (output)
    return slimcc_compile_file(path, output, &opt);
  // 2 つ目以降のソース
  SlimccObj *more[64];
  int nmore = 0;
  int j = i + 1;
  for (; j < argc && nmore < 64; j++) {
    size_t n = strlen(argv[j]);
    if (!strcmp(argv[j], "--")) {
      j++;
      break;
    }
    if (n < 2 || strcmp(argv[j] + n - 2, ".c"))
      break;
    more[nmore] = slimcc_compile(argv[j], NULL, &opt);
    if (!more[nmore])
      return 1;
    write_odir(odir, argv[j], more[nmore]);   // asm_write_elf は次のコンパイルの前に（アセンブラの状態を使う）
    nmore++;
  }

  SlimccObj *obj = NULL;
  char *first = NULL;
  size_t first_len = 0;
  for (int r = 0; r < repeat; r++) {
    if (obj)
      slimcc_free_obj(obj);
    obj = slimcc_compile(path, NULL, &opt);
    if (!obj)
      return 1;
    if (repeat > 1) {
      size_t len;
      char *b = elf_bytes(obj, &len);
      if (!first) {
        first = b;
        first_len = len;
      } else {
        if (len != first_len || memcmp(b, first, len)) {
          fprintf(stderr, "slimcc-jit: compile #%d differs from the first\n", r + 1);
          return 1;
        }
        free(b);
      }
      if (r == 0 || r == repeat - 1)
        fprintf(stderr, "slimcc-jit: compile #%d, max RSS %ld KB\n", r + 1, max_rss_kb());
    }
  }

  if (compile_only) {
    write_odir(odir, path, obj);
    slimcc_free_obj(obj);
    return 0;
  }

  if (roundtrip) {
    obj = round_trip(obj);
    for (int k = 0; k < nmore; k++)
      more[k] = round_trip(more[k]);
  }
  SlimccImage *img;
  if (nmore) {
    SlimccObj *all[65] = {obj};
    memcpy(all + 1, more, sizeof(SlimccObj *) * nmore);
    img = slimcc_link_many(all, nmore + 1, resolve, NULL, alloc_exec, free_exec);
    for (int k = 0; k < nmore; k++)
      slimcc_free_obj(more[k]);
  } else {
    img = slimcc_link(obj, resolve, NULL, alloc_exec, free_exec);
  }
  slimcc_free_obj(obj);
  if (!img)
    return 1;
  int (*entry)(int, char **) = (int (*)(int, char **))slimcc_symbol(img, "main");
  if (!entry) {
    fprintf(stderr, "slimcc-jit: no main()\n");
    return 1;
  }
  fflush(stdout);
  // プログラムの引数: argv[0] は最初のソース、その後に残りの引数
  if (nmore) {
    argv[j - 1] = argv[i];
    i = j - 1;
  }
  int ret = entry(argc - i, argv + i);
  fflush(stdout);
  slimcc_free_image(img);
  return ret;
}
