// コンパイラのオプションなど、ドライバ（main.c）とライブラリ（libslimcc.c）が共有する変数
#include "slimcc.h"

TLS StringArray include_paths;
TLS StringArray iquote_paths;
TLS StringArray display_files;
TLS bool opt_fcommon;
TLS int opt_fpic;
TLS int opt_fpie;
TLS bool opt_femulated_tls;
TLS int opt_fn_align = 1;
TLS bool opt_use_plt = true;
TLS bool opt_optimize = true;
TLS bool opt_reuse_stack = true;
TLS bool opt_g;
TLS bool opt_func_sections;
TLS bool opt_openmp;   // -fopenmp（cc/omp.inc）
TLS int opt_fp_contract = -1;   // -ffp-contract: 1 = on、0 = off、-1 = 既定（GNU の規格なら on、ISO なら off。GCC と同じ）
TLS bool opt_data_sections;
TLS bool opt_werror;
TLS bool opt_cc1_asm_pp;
TLS const char *opt_visibility;
TLS StdVer opt_std = STD_C17;
TLS bool is_iso_std;
TLS bool opt_fdefer_ts;
TLS bool opt_short_enums;
TLS bool opt_gnu_keywords;
TLS bool opt_gnu89_inline;
TLS bool opt_ms_anon_struct;
TLS bool opt_disable_visibility;
TLS bool opt_fake_always_inline;
TLS bool opt_E;
TLS bool opt_dM;
TLS bool opt_P;
TLS bool opt_pie;
TLS bool opt_nopie;
TLS bool opt_pthread;
TLS bool opt_r;
TLS bool opt_rdynamic;
TLS bool opt_static;
TLS bool opt_static_pie;
TLS bool opt_static_libgcc;
TLS bool opt_shared;
TLS bool opt_s;
TLS bool opt_nostartfiles;
TLS bool opt_nodefaultlibs;
TLS bool opt_nolibc;
TLS const char *default_ld = "ld";
TLS const char *default_as = "as";
TLS const char *dumpmachine_str;
TLS char *argv0;

// -D と -U（コマンドラインの順）
TLS MacroChangeArr macrodefs;
TLS bool opt_no_pic_macros;

void macrochange_push(MacroChangeArr *arr, const char *arg, bool is_def) {
  if (arr->len == arr->capacity)
    GrowArr(arr->data, &arr->capacity);

  MacroChange *m = &arr->data[arr->len++];
  m->arg = arg;
  m->is_def = is_def;
}

// コマンドラインのオプションに応じたマクロを定義する（-D・-U もここで反映する）
void define_cli_macros(bool is_asm_pp) {
  if (is_asm_pp) {
    define_macro("__ASSEMBLER__", "1");
  } else {
    if (is_iso_std)
      define_macro("__STRICT_ANSI__", "1");

    switch (opt_std) {
    case STD_C94: define_macro("__STDC_VERSION__", "199409L"); break;
    case STD_C99: define_macro("__STDC_VERSION__", "199901L"); break;
    case STD_C11: define_macro("__STDC_VERSION__", "201112L"); break;
    case STD_C17: define_macro("__STDC_VERSION__", "201710L"); break;
    case STD_C23: define_macro("__STDC_VERSION__", "202311L"); break;
    case STD_C2Y: define_macro("__STDC_VERSION__", "202400L"); break;
    }

    if (opt_std >= STD_C99)
      define_macro("__GNUC_STDC_INLINE__", "1");

    define_macro("__STDC_UTF_16__", "1");
    define_macro("__STDC_UTF_32__", "1");

    define_macro("__STDC_DEFER_TS25755__", opt_fdefer_ts ? "2" : "1");
  }

  if (opt_pthread)
    define_macro("_REENTRANT", "1");

  if (opt_openmp && !is_asm_pp)
    define_macro("_OPENMP", "201511");    // OpenMP 4.5（使える指示は cc/omp.inc）

  if (opt_no_pic_macros) {
    // libslimcc のメモリ上の実行（GOT を使うが、位置独立のマクロは定義しない）
  } else if (opt_fpic == 1) {
    define_macro("__pic__", "1");
    define_macro("__PIC__", "1");
  } else if (opt_fpic == 2) {
    define_macro("__pic__", "2");
    define_macro("__PIC__", "2");
  } else if (opt_fpie == 1) {
    define_macro("__pic__", "1");
    define_macro("__PIC__", "1");
    define_macro("__pie__", "1");
    define_macro("__PIE__", "1");
  } else if (opt_fpie == 2) {
    define_macro("__pic__", "2");
    define_macro("__PIC__", "2");
    define_macro("__pie__", "2");
    define_macro("__PIE__", "2");
  }

  if (ty_pchar->is_unsigned)
    define_macro("__CHAR_UNSIGNED__", "1");

  for (int i = 0; i < macrodefs.len; i++) {
    MacroChange *m = &macrodefs.data[i];
    if (m->is_def)
      define_macro_cli(m->arg);
    else
      undef_macro(m->arg);
  }
}
