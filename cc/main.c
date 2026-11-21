#include "slimcc.h"

typedef enum {
  FILE_NONE = 0,
  FILE_C,
  FILE_ASM,
  FILE_PP_ASM,
  FILE_LDARG,
} FileType;



static StringArray opt_imacros;
static StringArray opt_include;
static bool opt_M;
static bool opt_MM;
static bool opt_MD;
static bool opt_MG;
static bool opt_MP;
static bool opt_S;
// 構文解析と型検査までで止め、コードは生成しない（-S と同じ経路で、出力しない）
static bool opt_fsyntax_only;
// 内蔵アセンブラの確認用: .s を内蔵アセンブラにかけ、結果を比較用の形式で標準出力に出す
static bool opt_dump_internal_as;
// 内蔵アセンブラを使う（-fno-integrated-as で外部の as）。-g のときは外部の as を使う（行番号情報が未対応のため）
static bool opt_integrated_as = true;
static bool opt_c;
static bool opt_verbose;
static bool opt_verbose_noop;
static bool opt_pipe;
static bool nofork;

static const char *opt_use_ld;
static const char *opt_use_as;
static const char *opt_MF;
static const char *opt_MT;
static const char *opt_o;

static StringArray ld_paths;
static StringArray sysincl_paths;
static StringArray dep_files;
static StringArray tmpfiles;
static const char *tmp_folder;
static StringArray as_args;
static int incl_cnt;

static bool is_fork_child;

static void cc1(const char *input_file, const char *output, bool is_asm_pp);
// -c で内蔵アセンブラを使うとき、cc1（子のプロセス）が .o まで作る（中間表現の関数を、文字列を経ずに
// かけらで内蔵アセンブラに渡すため。cc/ir.inc）。-fno-slimcc-direct で今までどおり（文字列を一時ファイルに）
static bool opt_slimcc_direct = true;
static bool cc1_make_obj;

#if defined(USE_ASAN)
__attribute__((visibility("default"))) const char *__asan_default_options(void) {
  return "detect_leaks=0:exitcode=2";
}
#endif

#if defined(USE_UBSAN)
__attribute__((visibility("default"))) const char *__ubsan_default_options(void) {
  return "print_stacktrace=1:exitcode=3";
}
#endif

static void version(void) {
  puts("slimcc version 0.0");
}

static bool startswith(const char *arg, const char **p, const char *str) {
  size_t len = strlen(str);
  if (!strncmp(arg, str, len)) {
    *p = arg + len;
    return true;
  }
  return false;
}

static bool take_arg(char **argv, int *i, const char **arg, const char *opt) {
  if (strcmp(argv[*i], opt))
    return false;
  *i += 1;
  if (argv[*i]) {
    *arg = argv[*i];
    return true;
  }
  fprintf(stderr, "missing argument to %s\n", opt);
  exit(1);
}

static bool take_arg_s(char **argv, int *i, const char **p, const char *str) {
  return take_arg(argv, i, p, str) || startswith(argv[*i], p, str);
}

static void comma_arg(const char *arg, StringArray *arr) {
  arg = strtok(strdup(arg), ",");
  while (arg) {
    strarray_push(arr, arg);
    arg = strtok(NULL, ",");
  }
}

void add_include_path(StringArray *arr, const char *path) {
  size_t orig_len = strlen(path);
  size_t len = orig_len;

  while (len > 1 && path[len - 1] == '/')
    len--;

  for (int i = 0; i < arr->len; i++) {
    const char *s2 = arr->data[i];
    if (!strncmp(s2, path, len) && s2[len] == '\0')
      return;
  }

  if (len != orig_len)
    path = strndup(path, len);

  strarray_push(arr, path);
}

static FileType parse_opt_x(const char *s) {
  if (!strcmp(s, "c"))
    return FILE_C;
  if (!strcmp(s, "assembler"))
    return FILE_ASM;
  if (!strcmp(s, "assembler-with-cpp"))
    return FILE_PP_ASM;
  if (!strcmp(s, "none"))
    return FILE_NONE;
  error("<command line>: unknown argument for -x: %s", s);
}

static bool set_bool(const char *p, bool val, const char *str, bool *opt) {
  if (!strcmp(p, str)) {
    *opt = val;
    return true;
  }
  return false;
}

static bool set_true(const char *p, const char *str, bool *opt) {
  return set_bool(p, true, str, opt);
}

static void set_std(bool is_iso, const char *arg) {
  char *end;
  int val = strtoul(arg, &end, 10);

  if (end - arg == 2) {
    is_iso_std = is_iso;

    switch (val) {
    case 89:
    case 90: opt_std = STD_C89; return;
    case 99: opt_std = STD_C99; return;
    case 11: opt_std = STD_C11; return;
    case 17:
    case 18: opt_std = STD_C17; return;
    case 23: opt_std = STD_C23; return;
    case 29: opt_std = STD_C2Y; return;
    }
  }
  error("unknown c standard");
}

static void set_std_iso(const char *arg) {
  char *end;
  int val = strtoul(arg, &end, 10);

  if (end != arg) {
    is_iso_std = true;

    switch (val) {
    case 1990:   opt_std = STD_C89; return;
    case 199409: opt_std = STD_C94; return;
    case 1999:   opt_std = STD_C99; return;
    case 2011:   opt_std = STD_C11; return;
    case 2017:
    case 2018:   opt_std = STD_C17; return;
    case 2024:   opt_std = STD_C23; return;
    }
  }
  error("unknown c standard");
}



static char *quote_makefile(const char *s) {
  char *buf = calloc(1, strlen(s) * 2 + 1);

  for (int i = 0, j = 0; s[i]; i++) {
    switch (s[i]) {
    case '$':
      buf[j++] = '$';
      buf[j++] = '$';
      break;
    case '#':
      buf[j++] = '\\';
      buf[j++] = '#';
      break;
    case ' ':
    case '\t':
      for (int k = i - 1; k >= 0 && s[k] == '\\'; k--)
        buf[j++] = '\\';
      buf[j++] = '\\';
      buf[j++] = s[i];
      break;
    default: {
      buf[j++] = s[i];
      break;
    }
    }
  }
  return buf;
}

static void build_incl_paths(const char *opt_B, bool opt_nostdinc, StringArray *isystem,
                             StringArray *idirafter) {
  if (opt_B)
    add_include_path(&sysincl_paths, opt_B);

  for (int i = 0; i < isystem->len; i++)
    add_include_path(&sysincl_paths, isystem->data[i]);

  if (!opt_nostdinc)
    platform_stdinc_paths(&sysincl_paths);

  for (int i = 0; i < idirafter->len; i++)
    add_include_path(&sysincl_paths, idirafter->data[i]);

  // Filter system directories passed as -I
  for (int i = 0; i < include_paths.len; i++) {
    bool match = false;
    for (int j = 0; j < sysincl_paths.len; j++)
      if ((match = !strcmp(sysincl_paths.data[j], include_paths.data[i])))
        break;
    if (!match)
      include_paths.data[incl_cnt++] = include_paths.data[i];
  }
  include_paths.len = incl_cnt;

  for (int i = 0; i < sysincl_paths.len; i++)
    strarray_push(&include_paths, sysincl_paths.data[i]);
}

static void build_ld_paths(const char *opt_B, StringArray *paths) {
  if (opt_B)
    strarray_push(&ld_paths, opt_B);

  for (int i = 0; i < paths->len; i++)
    strarray_push(&ld_paths, paths->data[i]);

  platform_search_dirs(&ld_paths);
}

static int parse_args(int argc, char **argv, StringArray *input_args) {
  const char *opt_B = NULL;
  bool opt_nostdinc = false;
  bool has_gnu_keywords_option = false;
  int non_input_cnt = 0;

  StringArray libpaths = {0};
  StringArray isystem = {0};
  StringArray idirafter = {0};

  const char *arg;
  for (int i = 1; i < argc; i++) {
    if (*argv[i] == '\0')
      continue;

    if (*argv[i] != '-' || argv[i][1] == '\0') {
      strarray_push(input_args, argv[i]);
      continue;
    }

    if (!strcmp(argv[i], "-###")) {
      opt_verbose_noop = opt_verbose = true;
      continue;
    }

    if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
      opt_verbose = true;
      continue;
    }

    if (!strcmp(argv[i], "--help")) {
      puts("slimcc [ -o <path> ] <file>");
      exit(0);
    }

    if (!strcmp(argv[i], "--version")) {
      version();
      exit(0);
    }

    if (!strcmp(argv[i], "-dumpmachine")) {
      if (!dumpmachine_str)
        error("'-dumpmachine' not configured");
      puts(dumpmachine_str);
      exit(0);
    }

    if (!strcmp(argv[i], "-print-search-dirs") ||
        !strcmp(argv[i], "--print-search-dirs")) {
      StringArray dirs = {0};
      platform_search_dirs(&dirs);
      printf("libraries: =");
      for (int i = 0; i < dirs.len; i++)
        printf("%s%s", dirs.data[i], (i + 1 != dirs.len) ? ":" : "\n");
      exit(0);
    }

    if (!strcmp(argv[i], "-hashmap-test")) {
      hashmap_test();
      exit(0);
    }

    if (take_arg_s(argv, &i, &arg, "-o")) {
      opt_o = arg;
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }

    if (!strcmp(argv[i], "-fintegrated-as")) {
      opt_integrated_as = true;
      continue;
    }

    if (!strcmp(argv[i], "-fno-integrated-as")) {
      opt_integrated_as = false;
      continue;
    }

    if (!strcmp(argv[i], "-dump-internal-as")) {
      opt_dump_internal_as = true;
      continue;
    }

    if (!strcmp(argv[i], "-fsyntax-only")) {
      opt_S = opt_fsyntax_only = true;
      continue;
    }

    if (!strcmp(argv[i], "-c")) {
      opt_c = true;
      continue;
    }

    if (!strcmp(argv[i], "-E")) {
      opt_E = true;
      continue;
    }

    if (!strcmp(argv[i], "-P")) {
      opt_P = true;
      continue;
    }

    if (!strcmp(argv[i], "-dM")) {
      opt_dM = true;
      continue;
    }

    if (take_arg_s(argv, &i, &opt_B, "-B"))
      continue;

    if (take_arg_s(argv, &i, &arg, "-I")) {
      add_include_path(&include_paths, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-isystem")) {
      strarray_push(&isystem, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-idirafter")) {
      strarray_push(&idirafter, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-iquote")) {
      add_include_path(&iquote_paths, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-D")) {
      macrochange_push(&macrodefs, arg, true);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-U")) {
      macrochange_push(&macrodefs, arg, false);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-imacros")) {
      strarray_push(&opt_imacros, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-include")) {
      strarray_push(&opt_include, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-x")) {
      strarray_push(input_args, "-x");
      strarray_push(input_args, arg);
      non_input_cnt += 2;
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-L")) {
      strarray_push(&libpaths, arg);
      continue;
    }

    if (startswith(argv[i], &arg, "-Wa,")) {
      comma_arg(arg, &as_args);
      continue;
    }

    if (startswith(argv[i], &arg, "-Wl,")) {
      strarray_push(input_args, argv[i]);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-l")) {
      strarray_push(input_args, format("-Wl,-l%s", arg));
      continue;
    }

    if (take_arg(argv, &i, &arg, "-Xlinker")) {
      strarray_push(input_args, format("-Wl,%s", arg));
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-z")) {
      strarray_push(input_args, format("-Wl,-z,%s", arg));
      continue;
    }

    if (!strcmp(argv[i], "-s")) {
      opt_s = true;
      continue;
    }

    if (!strcmp(argv[i], "-M")) {
      opt_M = true;
      continue;
    }

    if (!strcmp(argv[i], "-MM")) {
      opt_M = opt_MM = true;
      continue;
    }

    if (!strcmp(argv[i], "-MD")) {
      opt_MD = true;
      continue;
    }

    if (!strcmp(argv[i], "-MMD")) {
      opt_MD = opt_MM = true;
      continue;
    }

    if (startswith(argv[i], &arg, "-Wp,-MD,")) {
      opt_MD = true;
      opt_MF = arg;
      continue;
    }

    if (startswith(argv[i], &arg, "-Wp,-MMD,")) {
      opt_MD = opt_MM = true;
      opt_MF = arg;
      continue;
    }

    if (!strcmp(argv[i], "-MG")) {
      opt_MG = true;
      continue;
    }

    if (!strcmp(argv[i], "-MP")) {
      opt_MP = true;
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-MF")) {
      opt_MF = arg;
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-MT") || startswith(argv[i], &arg, "-Wp,-MT,")) {
      if (opt_MT == NULL)
        opt_MT = arg;
      else
        opt_MT = format("%s %s", opt_MT, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-MQ") || startswith(argv[i], &arg, "-Wp,-MQ,")) {
      if (opt_MT == NULL)
        opt_MT = quote_makefile(arg);
      else
        opt_MT = format("%s %s", opt_MT, quote_makefile(arg));
      continue;
    }

    if (startswith(argv[i], &arg, "-g")) {
      opt_g = strcmp(arg, "0");
      continue;
    }

    if (startswith(argv[i], &arg, "-O")) {
      switch (*arg) {
      case 's':
      case 'z': {
        opt_fn_align = 1;
        break;
      }
      case '2':
      case '3':
        if (opt_fn_align == 1)
          opt_fn_align = 16;
        break;
      }
      continue;
    }

    if (!strcmp(argv[i], "-ansi")) {
      set_std(true, "89");
      continue;
    }

    if (startswith(argv[i], &arg, "-std=") ||
        startswith(argv[i], &arg, "--std=") ||
        take_arg(argv, &i, &arg, "--std")) {
      if (startswith(arg, &arg, "c"))
        set_std(true, arg);
      else if (startswith(arg, &arg, "gnu"))
        set_std(false, arg);
      else if (startswith(arg, &arg, "iso9899:"))
        set_std_iso(arg);
      else
        error("unknown c standard");
      continue;
    }

    if (!strcmp(argv[i], "--tmpflagstdgnu2y")) {
      set_std(false, "29");
      continue;
    }

    if (startswith(argv[i], &arg, "-ffp-contract=")) {
      if (!strcmp(arg, "off"))
        opt_fp_contract = 0;
      else if (!strcmp(arg, "on") || !strcmp(arg, "fast"))
        opt_fp_contract = 1;
      else
        error("unknown -ffp-contract: %s", arg);
      continue;
    }

    if (startswith(argv[i], &arg, "-f")) {
      bool b = !startswith(arg, &arg, "no-");

      if (set_bool(arg, b, "common", &opt_fcommon) ||
          set_bool(arg, b, "plt", &opt_use_plt) ||
          set_bool(arg, b, "function-sections", &opt_func_sections) ||
          set_bool(arg, b, "openmp", &opt_openmp) ||
          set_bool(arg, b, "data-sections", &opt_data_sections) ||
          set_bool(arg, b, "emulated-tls", &opt_femulated_tls) ||
          set_bool(arg, b, "short-enums", &opt_short_enums) ||
          set_bool(arg, b, "gnu89-inline", &opt_gnu89_inline))
        continue;

      if (set_bool(arg, b, "ms-anon-struct", &opt_ms_anon_struct) ||
          set_bool(arg, b, "ms-anonymous-structs", &opt_ms_anon_struct))
        continue;

      if (!strcmp(arg, "asm") || !strcmp(arg, "gnu-keywords")) {
        opt_gnu_keywords = b;
        has_gnu_keywords_option = true;
        continue;
      }

      if (!strncmp(arg, "align-functions", 15)) {
        arg += 15;
        if (*arg == '\0') {
          opt_fn_align = b ? 16 : 1;
          continue;
        }
        if (b && *arg == '=') {
          char *end;
          if (!(opt_fn_align = strtoul(arg + 1, &end, 10)))
            opt_fn_align = 16;
          if (*end == '\0' && is_pow_of_two(opt_fn_align))
            continue;
        }
        error("unsupported form of '-falign-functions'");
      }

      if (!strcmp(arg, "pic")) {
        opt_fpic = 1 * b, opt_fpie = 0;
        continue;
      }
      if (!strcmp(arg, "PIC")) {
        opt_fpic = 2 * b, opt_fpie = 0;
        continue;
      }
      if (!strcmp(arg, "pie")) {
        opt_fpic = 0, opt_fpie = 1 * b;
        continue;
      }
      if (!strcmp(arg, "PIE")) {
        opt_fpic = 0, opt_fpie = 2 * b;
        continue;
      }

      // These options are ignored for now.
      // -fstrict-aliasing（既定）・-fno-strict-aliasing: 型による別名の判定（ir.inc の ir_may_alias）
      if (!strcmp(arg, "strict-aliasing")) {
        opt_strict_aliasing = b;
        continue;
      }
      if (!strcmp(arg, "strict-overflow") ||
          !strcmp(arg, "omit-frame-pointer"))
        continue;

      // -fslimcc-direct（既定）・-fno-slimcc-direct: -c で、中間表現の関数を文字列を経ずに内蔵アセンブラへ
      if (!strcmp(arg, "slimcc-direct")) {
        opt_slimcc_direct = b;
        continue;
      }
      // -fslimcc-ir（既定）・-fno-slimcc-ir: 中間表現（SSA）を使うコード生成
      if (!strcmp(arg, "slimcc-ir")) {
        opt_ir = b;
        continue;
      }
      // -fschedule-insns2（既定）・-fno-schedule-insns2: 命令の並べ替え（GCC と同じ名前。-fschedule-insns は受けるだけ）
      if (!strcmp(arg, "schedule-insns2")) {
        opt_sched = b;
        continue;
      }
      if (!strcmp(arg, "schedule-insns"))
        continue;
      // -funroll-loops（既定）・-fno-unroll-loops: ループの展開（ir.inc の ir_unroll_for）
      if (!strcmp(arg, "unroll-loops")) {
        opt_unroll = b;
        continue;
      }

      // -f only options
      if (b) {
        if (set_true(arg, "defer-ts", &opt_fdefer_ts))
          continue;


        if (set_bool(arg, false, "signed-char", &ty_pchar->is_unsigned) ||
            set_bool(arg, true, "unsigned-char", &ty_pchar->is_unsigned))
          continue;

        if (startswith(arg, &arg, "stack-reuse=")) {
          opt_reuse_stack = !strcmp(arg, "all");
          continue;
        }

        if (set_true(arg, "disable-visibility", &opt_disable_visibility))
          continue;
        if (set_true(arg, "fake-always-inline", &opt_fake_always_inline))
          continue;

        if (startswith(arg, &opt_visibility, "visibility=") ||
            startswith(arg, &opt_use_as, "use-as="))
          continue;

        if (startswith(arg, &arg, "use-ld=")) {
          if (!strcmp(arg, "lld")) {
            opt_use_ld = "ld.lld";
            continue;
          }
          opt_use_ld = arg;
          continue;
        }
      }
    }

    if (argv[i][0] == '-') {
      arg = (argv[i][1] == '-') ? &argv[i][2] : &argv[i][1];

      if (set_true(arg, "r", &opt_r) ||
          set_true(arg, "rdynamic", &opt_rdynamic) ||
          set_true(arg, "static", &opt_static) ||
          set_true(arg, "static-pie", &opt_static_pie) ||
          set_true(arg, "static-libgcc", &opt_static_libgcc) ||
          set_true(arg, "shared", &opt_shared) ||
          set_true(arg, "pie", &opt_pie) ||
          set_true(arg, "nopie", &opt_nopie) ||
          set_true(arg, "pthread", &opt_pthread) ||
          set_true(arg, "pipe", &opt_pipe))
        continue;

      if (!strcmp(arg, "no-pie")) {
        opt_pie = false;
        opt_nopie = true;
        continue;
      }
    }

    if (!strcmp(argv[i], "-nostdinc")) {
      opt_nostdinc = true;
      continue;
    }

    if (set_true(argv[i], "-nostartfiles", &opt_nostartfiles) ||
        set_true(argv[i], "-nodefaultlibs", &opt_nodefaultlibs) ||
        set_true(argv[i], "-nolibc", &opt_nolibc))
      continue;

    if (!strcmp(argv[i], "-nostdlib")) {
      opt_nostartfiles = opt_nodefaultlibs = true;
      continue;
    }

    if (set_bool(argv[i], true, "-Werror", &opt_werror) ||
        set_bool(argv[i], false, "-Wno-error", &opt_werror))
      continue;

    if (startswith(argv[i], &arg, "-W")) {
      if (strchr(arg, ','))
        error("unknown argument: %s", argv[i]);
      continue;
    }

    // These options are ignored for now.
    if (startswith(argv[i], &arg, "-march=") ||
        !strcmp(argv[i], "-fdollars-in-identifiers") ||
        !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-ffp-contract=off") ||
        !strcmp(argv[i], "-fno-builtin") ||
        !strcmp(argv[i], "-fno-fast-math") ||
        !strcmp(argv[i], "-fno-lto") ||
        !strcmp(argv[i], "-fno-unwind-tables") ||
        !strcmp(argv[i], "-fno-asynchronous-unwind-tables") ||
        !strcmp(argv[i], "-fno-delete-null-pointer-checks") ||
        !strcmp(argv[i], "-fno-exceptions") ||
        !strcmp(argv[i], "-fno-stack-protector") ||
        !strcmp(argv[i], "-fwrapv") ||
        !strcmp(argv[i], "-m64") ||
        !strcmp(argv[i], "-malign-double") ||
        !strcmp(argv[i], "-mfpmath=sse") ||
        !strcmp(argv[i], "-mno-red-zone") ||
        !strcmp(argv[i], "-pedantic") ||
        !strcmp(argv[i], "--pedantic") ||
        !strcmp(argv[i], "-pedantic-errors") ||
        !strcmp(argv[i], "--pedantic-errors") ||
        !strcmp(argv[i], "-w"))
      continue;

    error("unknown argument: %s", argv[i]);
  }

  if (!opt_E && opt_dM)
    error("option -dM without -E not supported");

  if (opt_MG && !opt_M)
    error("option -MG must be used with -M or -MM");

  if (opt_disable_visibility && opt_visibility)
    error("-fvisibility disabled with -fdisable-visibility");

  if (!has_gnu_keywords_option)
    opt_gnu_keywords = !is_iso_std;

  if (opt_B) {
    char *as_b = format("%s/%s", opt_B, default_as);
    char *ld_b = format("%s/%s", opt_B, default_ld);
    if (file_exists(as_b))
      default_as = as_b;
    if (file_exists(ld_b))
      default_ld = ld_b;
  }

  build_incl_paths(opt_B, opt_nostdinc, &isystem, &idirafter);

  build_ld_paths(opt_B, &libpaths);

  return input_args->len - non_input_cnt;
}

static FILE *open_file(const char *path) {
  if (!strcmp(path, "-"))
    return stdout;

  FILE *out = fopen(path, "w");
  if (!out)
    error("cannot open output file: %s: %s", path, strerror(errno));
  return out;
}

static void close_file(FILE *file) {
  if (file == stdout)
    fflush(file);
  else
    fclose(file);
}

// Replace file extension
static const char *replace_extn(const char *tmpl, const char *extn) {
  char *filename = basename(strdup(tmpl));
  char *dot = strrchr(filename, '.');
  if (dot)
    *dot = '\0';
  return format("%s%s", filename, extn);
}

void cleanup_exit(int status) {
  if (is_fork_child)
    _exit(status);

  for (int i = 0; i < tmpfiles.len; i++)
    unlink(tmpfiles.data[i]);
  if (tmp_folder)
    rmdir(tmp_folder);

#if defined(CHECK_MEMUSE)
  {
    int64_t accum = 0;
    struct rusage stat;
    getrusage(RUSAGE_SELF, &stat);
    accum += stat.ru_maxrss;
    getrusage(RUSAGE_CHILDREN, &stat);
    accum += stat.ru_maxrss;
    if (accum > 5000 * 1024)
      status = 5;
  }
#endif

  exit(status);
}

static char *create_tmpfile(void) {
  if (!tmp_folder) {
    tmp_folder = mkdtemp(strdup("/tmp/slimcc-XXXXXX"));
    if (!tmp_folder)
      error("mkdtemp failed: %s", strerror(errno));
  }
  static int64_t i;
  char *path = format("%s/%" PRIi64, tmp_folder, i++);

  strarray_push(&tmpfiles, path);
  return path;
}

static char *create_pipefile(void) {
  char *path = create_tmpfile();
  if (mkfifo(path, (S_IRUSR | S_IWUSR)) == -1)
    error("mkfifo failed: %s", strerror(errno));
  return path;
}

void run_subprocess(const char **argv) {
  if (opt_verbose) {
    fprintf(stderr, "\"%s\"", argv[0]);
    for (int i = 1; argv[i]; i++)
      fprintf(stderr, " \"%s\"", argv[i]);
    fprintf(stderr, "\n");

    if (opt_verbose_noop)
      return;
  }

  extern char **environ;
  pid_t id;
  int status;
  if (posix_spawnp(&id, argv[0], NULL, NULL, (char *const *)argv, environ) ||
      waitpid(id, &status, 0) <= 0 ||
      status != 0) {
    fprintf(stderr, "exec failed: %s\n", argv[0]);
    cleanup_exit(1);
  }
}

static int fork_cc1(const char *input, const char *output, bool is_asm_pp) {
  pid_t id = fork();
  if (id == 0) {
    is_fork_child = true;
    cc1(input, output, is_asm_pp);
    _exit(0);
  }
  return id;
}

static void run_cc1(const char *input, const char *output, bool is_asm_pp) {
  if (opt_verbose_noop)
    return;

  if (nofork) {
    cc1(input, output, is_asm_pp);
    return;
  }

  int id = fork_cc1(input, output, is_asm_pp);

  int status;
  if (waitpid(id, &status, 0) <= 0 || status != 0)
    cleanup_exit(1);
}

static void run_cc1_as(const char *input, const char *output, bool is_asm_pp) {
  if (opt_slimcc_direct && opt_integrated_as && !opt_g && !opt_use_as && !opt_pipe && !opt_verbose_noop &&
      !is_asm_pp && !opt_M && !opt_MD) {
    cc1_make_obj = true;
    run_cc1(input, output, is_asm_pp);
    cc1_make_obj = false;
    return;
  }
  if (opt_verbose_noop) {
    char *tmp = create_tmpfile();
    run_assembler(&as_args, tmp, output);
    return;
  }

  if (opt_pipe) {
    char *tmp = create_pipefile();
    int id = fork_cc1(input, tmp, is_asm_pp);
    run_assembler(&as_args, tmp, output);

    int status;
    if (waitpid(id, &status, 0) <= 0 || status != 0)
      cleanup_exit(1);
    return;
  }

  char *tmp = create_tmpfile();
  run_cc1(input, tmp, is_asm_pp);
  run_assembler(&as_args, tmp, output);
}


bool in_sysincl_path(int idx) {
  return idx >= incl_cnt;
}

bool ignore_missing_dep(const char *path, const char *filename, Token *tok) {
  if (!path) {
    if (opt_MG) {
      add_dep_file(filename, false);
      return true;
    }
    if (tok)
      error_tok(tok, "file not found");
    error("`%s` file not found", filename);
  }
  return false;
}

void add_dep_file(const char *path, bool is_sys) {
  if (opt_M || opt_MD) {
    if (is_sys && opt_MM)
      return;

    static HashMap map;
    HashEntry *ent = hashmap_get_or_insert(&map, path, strlen(path));
    if (ent->val)
      return;
    ent->val = (void *)1;
    strarray_push(&dep_files, path);
  }
}

static const char *skip_dot_slash(const char *p) {
  if (*p == '.' && p[1] == '/') {
    for (p += 2; *p == '/';)
      p++;
  }
  return p;
}

static const char *print_dep_outpath(const char *input) {
  if (opt_MF)
    return opt_MF;
  if (opt_MD)
    return replace_extn(opt_o ? opt_o : input, ".d");
  return opt_o ? opt_o : "-";
}

static void print_dependencies(const char *input, FILE *out) {
  if (opt_MT)
    fprintf(out, "%s:", opt_MT);
  else
    fprintf(out, "%s:", quote_makefile(replace_extn(input, ".o")));

  for (int i = 0; i < dep_files.len; i++)
    fprintf(out, " \\\n  %s", skip_dot_slash(dep_files.data[i]));

  fputc('\n', out);

  if (opt_MP)
    for (int i = 1; i < dep_files.len; i++)
      fprintf(out, "%s:\n", quote_makefile(skip_dot_slash(dep_files.data[i])));
}

static void cc1(const char *input, const char *output, bool is_asm_pp) {
  if (is_asm_pp)
    opt_E = opt_cc1_asm_pp = true;

  arena_on(&cc1_arena);
  arena_on(&pp_arena);

  init_macros();
  platform_init_cc1();
  define_cli_macros(is_asm_pp);

  FILE *out = open_file(output);

  Token *tok = preprocess(input, &opt_include, &opt_imacros);

  if (opt_M) {
    print_dependencies(input, out);

    close_file(out);
    arena_off(&pp_arena);
    arena_off(&cc1_arena);
    return;
  }

  if (opt_MD) {
    FILE *out2 = open_file(print_dep_outpath(input));

    print_dependencies(input, out2);

    close_file(out2);
  }

  if (opt_E) {
    if (opt_dM)
      dump_defines(out);
    else
      print_tokens(tok, out);

    close_file(out);
    arena_off(&pp_arena);
    arena_off(&cc1_arena);
    return;
  }

  tok = prepare_parse(tok);

  arena_off(&pp_arena);

  // 関数は構文解析の中で出力されるので、その前に決める
  opt_direct = cc1_make_obj && !opt_fsyntax_only;
  Obj *prog = parse(tok);

  if (cc1_make_obj && !opt_fsyntax_only) {
    // .o まで作る（アセンブリの文字列はメモリに置き、中間表現の関数はかけらで渡す）
    close_file(out);
    char *buf;
    size_t len;
    FILE *mem = open_memstream(&buf, &len);
    codegen(prog, mem);
    fclose(mem);
    arena_off(&cc1_arena);
    AsmObj *obj = asm_assemble(buf, input);
    slimcc_frag_free_all();
    FILE *of = open_file(output);
    asm_write_elf(obj, of);
    close_file(of);
    free(buf);
    return;
  }

  if (!opt_fsyntax_only)
    codegen(prog, out);

  close_file(out);
  arena_off(&cc1_arena);
}

static char *read_whole_file(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    error("cannot open %s: %s", path, strerror(errno));
  char *buf;
  size_t len;
  FILE *mem = open_memstream(&buf, &len);
  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0)
    fwrite(tmp, 1, n, mem);
  fclose(fp);
  fclose(mem);
  return buf;
}

static void run_integrated_assembler(const char *input, const char *output) {
  char *text = read_whole_file(input);
  AsmObj *obj = asm_assemble(text, input);
  FILE *out = open_file(output);
  asm_write_elf(obj, out);
  close_file(out);
  free(text);
}

void run_assembler_gnustyle(StringArray *args, const char *input, const char *output) {
  if (opt_integrated_as && !opt_g && !opt_use_as) {
    if (opt_verbose)
      fprintf(stderr, "(integrated assembler) %s -o %s\n", input, output);
    if (!opt_verbose_noop)
      run_integrated_assembler(input, output);
    return;
  }

  StringArray arr = {0};

  strarray_push(&arr, opt_use_as ? opt_use_as : default_as);
  strarray_push(&arr, input);
  strarray_push(&arr, "-o");
  strarray_push(&arr, output);
  strarray_push(&arr, "--fatal-warnings");

  for (int i = 0; i < args->len; i++)
    strarray_push(&arr, args->data[i]);

  strarray_push(&arr, NULL);

  run_subprocess(arr.data);
}

static char *find_file(const char *pattern) {
  char *path = NULL;
  glob_t buf = {0};
  glob(pattern, 0, NULL, &buf);
  if (buf.gl_pathc > 0)
    path = strdup(buf.gl_pathv[buf.gl_pathc - 1]);
  globfree(&buf);
  return path;
}

char *find_dir_w_file(const char *pattern) {
  static char *path;
  if (!path) {
    path = find_file(pattern);
    if (!path)
      return NULL;
    path = dirname(path);
  }
  return path;
}

bool file_exists(const char *path) {
  struct stat st;
  return !stat(path, &st);
}

LinkType link_type_gnustyle(void) {
  if (opt_r)
    return LT_RELO;
  if (opt_shared)
    return LT_SHARED;
  if (opt_static_pie)
    return LT_STATIC_PIE;
  if (opt_static)
    return LT_STATIC;
  if (opt_pie)
    return LT_PIE;
  return LT_DYNAMIC;
}

void ldarg_gnu_base(StringArray *arr, const char *output) {
  strarray_push(arr, opt_use_ld ? opt_use_ld : default_ld);
  strarray_push(arr, "-o");
  strarray_push(arr, output);
  strarray_push(arr, "-m");
  strarray_push(arr, "aarch64linux");
  strarray_push(arr, "--eh-frame-hdr");

  if (opt_s)
    strarray_push(arr, "-s");
}

void ldarg_gnu_linktype(StringArray *arr, LinkType lt, const char *ldso_path) {
  switch (lt) {
  case LT_RELO:       strarray_push(arr, "-r"); break;
  case LT_SHARED:     strarray_push(arr, "-shared"); break;
  case LT_STATIC:     strarray_push(arr, "-static"); break;
  case LT_STATIC_PIE: strarray_push(arr, "-static");
  case LT_PIE:        strarray_push(arr, "-pie"); break;
  }

  switch (lt) {
  case LT_STATIC_PIE: {
    strarray_push(arr, "-no-dynamic-linker");
    break;
  }
  case LT_DYNAMIC:
  case LT_SHARED:
  case LT_PIE:
    if (ldso_path) {
      if (opt_rdynamic)
        strarray_push(arr, "--export-dynamic");

      strarray_push(arr, "-dynamic-linker");
      strarray_push(arr, ldso_path);
    }
  }
}

void ldarg_gnu_crtbegin(StringArray *arr, LinkType lt, const char *gcc_libpath) {
  if (gcc_libpath) {
    switch (lt) {
    case LT_STATIC_PIE:
    case LT_SHARED:
    case LT_PIE:        strarray_push(arr, format("%s/crtbeginS.o", gcc_libpath)); break;
    case LT_STATIC:     strarray_push(arr, format("%s/crtbeginT.o", gcc_libpath)); break;
    case LT_DYNAMIC:    strarray_push(arr, format("%s/crtbegin.o", gcc_libpath)); break;
    }
  }
}

void ldarg_gnu_inputs(StringArray *arr, StringArray *paths, StringArray *args) {
  for (int i = 0; i < paths->len; i++) {
    strarray_push(arr, "-L");
    strarray_push(arr, paths->data[i]);
  }

  for (int i = 0; i < args->len; i++)
    strarray_push(arr, args->data[i]);
}

static void link_libgcc(StringArray *arr, bool has_libgcc, bool is_static) {
  if (!has_libgcc)
    return;
  strarray_push(arr, "-lgcc");

  if (is_static) {
    strarray_push(arr, "-lgcc_eh");
  } else {
    strarray_push(arr, "--push-state");
    strarray_push(arr, "--as-needed");
    strarray_push(arr, "-lgcc_s");
    strarray_push(arr, "--pop-state");
  }
}

static void link_libc(StringArray *arr) {
  if (opt_pthread)
    strarray_push(arr, "-lpthread");
  if (!opt_nolibc) {
    strarray_push(arr, "-lc");
    strarray_push(arr, "--push-state");
    strarray_push(arr, "--as-needed");
    strarray_push(arr, "-lm");
    strarray_push(arr, "--pop-state");
  }
}

void ldarg_gnu_lc_lgcc(StringArray *arr, LinkType lt, bool has_libgcc) {
  if (lt == LT_STATIC_PIE || lt == LT_STATIC) {
    strarray_push(arr, "--start-group");
    link_libgcc(arr, has_libgcc, true);
    link_libc(arr);
    strarray_push(arr, "--end-group");
  } else {
    link_libgcc(arr, has_libgcc, opt_static_libgcc);
    link_libc(arr);
    link_libgcc(arr, has_libgcc, opt_static_libgcc);
  }
}

void ldarg_gnu_crtend(StringArray *arr, LinkType lt, const char *gcc_libpath) {
  if (gcc_libpath) {
    switch (lt) {
    case LT_STATIC_PIE:
    case LT_SHARED:
    case LT_PIE:        strarray_push(arr, format("%s/crtendS.o", gcc_libpath)); break;
    case LT_STATIC:
    case LT_DYNAMIC:    strarray_push(arr, format("%s/crtend.o", gcc_libpath)); break;
    }
  }
}

void run_linker_gnustyle(StringArray *paths, StringArray *args, const char *output,
                         const char *ldso_path, const char *libpath,
                         const char *gcc_libpath) {
  StringArray arr = {0};

  ldarg_gnu_base(&arr, output);

  LinkType lt = link_type_gnustyle();

  ldarg_gnu_linktype(&arr, lt, ldso_path);

  if (!opt_nostartfiles && lt != LT_RELO) {
    switch (lt) {
    case LT_STATIC_PIE: strarray_push(&arr, format("%s/rcrt1.o", libpath)); break;
    case LT_PIE:        strarray_push(&arr, format("%s/Scrt1.o", libpath)); break;
    case LT_STATIC:
    case LT_DYNAMIC:    strarray_push(&arr, format("%s/crt1.o", libpath)); break;
    }
    strarray_push(&arr, format("%s/crti.o", libpath));

    ldarg_gnu_crtbegin(&arr, lt, gcc_libpath);
  }

  ldarg_gnu_inputs(&arr, paths, args);

  if (!opt_nostartfiles && lt != LT_RELO) {
    ldarg_gnu_lc_lgcc(&arr, lt, gcc_libpath);

    ldarg_gnu_crtend(&arr, lt, gcc_libpath);

    strarray_push(&arr, format("%s/crtn.o", libpath));
  }

  strarray_push(&arr, NULL);

  run_subprocess(arr.data);
}

static FileType get_file_type(const char *filename) {
  const char *p = strrchr(filename, '.');
  if (p && p[1] && !p[2]) {
    switch (p[1]) {
    case 'c':
    case 'i': return FILE_C;
    case 's': return FILE_ASM;
    case 'S': return FILE_PP_ASM;
    case 'h':
      if (opt_E)
        return FILE_C;
      error("pch not supported");
    }
  }
  if (!strcmp(filename, "-")) {
    if (opt_E)
      return FILE_C;
    error("-E or -x required when input is from standard input");
  }
  return FILE_LDARG;
}

int main(int argc, char **argv) {
  argv0 = argv[0];
  platform_init_driver();

  StringArray input_args = {0};
  int input_cnt = parse_args(argc, argv, &input_args);

  if (opt_verbose)
    version();

  if (!input_cnt) {
    if (opt_verbose)
      exit(0);
    error("no input files");
  }

  if (opt_dump_internal_as) {
    for (int i = 0; i < input_args.len; i++) {
      const char *path = input_args.data[i];
      char *buf = read_whole_file(path);
      asm_dump(asm_assemble(buf, path), stdout);
      free(buf);
    }
    exit(0);
  }

  if (input_cnt == 1 && (opt_M || opt_E || opt_S))
    nofork = true;

  StringArray ld_args = {0};
  FileType opt_x = FILE_NONE;
  for (int i = 0; i < input_args.len; i++) {
    const char *input = input_args.data[i];

    if (!strcmp(input, "-x")) {
      opt_x = parse_opt_x(input_args.data[++i]);
      continue;
    }

    const char *arg;
    if (startswith(input, &arg, "-Wl,")) {
      if (opt_M || opt_E || opt_S || opt_c)
        continue;
      comma_arg(arg, &ld_args);
      continue;
    }

    FileType type = opt_x ? opt_x : get_file_type(input);

    if (type == FILE_LDARG) {
      if (opt_M || opt_E || opt_S || opt_c) {
        if (!file_exists(input)) {
          fprintf(stderr, "linker input file '%s' not found\n", input);
          cleanup_exit(1);
        }
        continue;
      }
      strarray_push(&ld_args, input);
      continue;
    }

    if (type == FILE_ASM) {
      if (opt_M || opt_E || opt_S)
        continue;

      if (opt_c) {
        run_assembler(&as_args, input, opt_o ? opt_o : replace_extn(input, ".o"));
        continue;
      }
      char *tmp = create_tmpfile();
      run_assembler(&as_args, input, tmp);
      strarray_push(&ld_args, tmp);
      continue;
    }

    if (type == FILE_PP_ASM) {
      if (opt_M) {
        run_cc1(input, print_dep_outpath(input), true);
        continue;
      }
      if (opt_E || opt_S) {
        run_cc1(input, (opt_o ? opt_o : "-"), true);
        continue;
      }
      if (opt_c) {
        run_cc1_as(input, (opt_o ? opt_o : replace_extn(input, ".o")), true);
        continue;
      }
      char *tmp = create_tmpfile();
      run_cc1_as(input, tmp, true);
      strarray_push(&ld_args, tmp);
      continue;
    }

    if (opt_M) {
      run_cc1(input, print_dep_outpath(input), false);
      continue;
    }
    if (opt_E) {
      run_cc1(input, (opt_o ? opt_o : "-"), false);
      continue;
    }
    if (opt_fsyntax_only) {
      run_cc1(input, "/dev/null", false);
      continue;
    }
    if (opt_S) {
      run_cc1(input, (opt_o ? opt_o : replace_extn(input, ".s")), false);
      continue;
    }
    if (opt_c) {
      run_cc1_as(input, (opt_o ? opt_o : replace_extn(input, ".o")), false);
      continue;
    }
    char *tmp = create_tmpfile();
    run_cc1_as(input, tmp, false);
    strarray_push(&ld_args, tmp);
    continue;
  }

  if (ld_args.len)
    run_linker(&ld_paths, &ld_args, opt_o ? opt_o : "a.out");

  cleanup_exit(0);
}
