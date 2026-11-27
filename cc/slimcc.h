#ifndef SLIMCC_H
#define SLIMCC_H

#define _XOPEN_SOURCE 700
#include <assert.h>
#include <errno.h>
#include <glob.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_builtin)
# define SLIMCC_HAS_BUILTIN(x) __has_builtin(x)
#else
# define SLIMCC_HAS_BUILTIN(x) 0
#endif

#if defined(__has_attribute)
# define SLIMCC_HAS_ATTR(x) __has_attribute(x)
#else
# define SLIMCC_HAS_ATTR(x) 0
#endif

#if defined(__has_c_attribute)
# define SLIMCC_HAS_C_ATTR(x) __has_c_attribute(x)
#else
# define SLIMCC_HAS_C_ATTR(x) 0
#endif

#if defined(__has_feature)
# define SLIMCC_HAS_FEAT(x) __has_feature(x)
#else
# define SLIMCC_HAS_FEAT(x) 0
#endif

#if SLIMCC_HAS_FEAT(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
# define USE_ASAN
# include <sanitizer/asan_interface.h>
#endif

#if SLIMCC_HAS_FEAT(undefined_behavior_sanitizer) || defined(__SANITIZE_UNDEFINED__)
# define USE_UBSAN
#endif

#if SLIMCC_HAS_FEAT(type_sanitizer) || defined(__SANITIZE_TYPE__)
# define USE_TYSAN
#endif

// コンパイラの状態（書き換えるファイルスコープ・関数内の静的な変数）は、ライブラリ（SLIMCC_LIBRARY。
// Monocs のカーネルに組み込む）ではコアごとに持つ（_Thread_local）。複数のコアで同時にコンパイルできるように
#ifdef SLIMCC_LIBRARY
# define TLS _Thread_local
#else
# define TLS
#endif

// ライブラリ（SLIMCC_LIBRARY）ではゾーンでまとめて捨てるので、ASan のときも逐次の解放はしない
#if (defined(USE_ASAN) || defined(__FILC__)) && !defined(SLIMCC_LIBRARY)
# define EAGER_FREE 1
#else
# define EAGER_FREE 0
#endif

#if (defined(__GNUC__) && __GNUC__ >= 3) || SLIMCC_HAS_ATTR(format)
# define FMTCHK(x, y) __attribute__((format(printf, (x), (y))))
#else
# define FMTCHK(x, y)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
# define NORETURN _Noreturn
#elif __GNUC__ >= 3 || SLIMCC_HAS_ATTR(noreturn)
# define NORETURN __attribute__((noreturn))
#elif SLIMCC_HAS_C_ATTR(noreturn)
# define NORETURN [[noreturn]]
#else
# define NORETURN
#endif

#if defined(__SIZEOF_LONG_LONG__) && __SIZEOF_LONG_LONG__ == 8
# if SLIMCC_HAS_BUILTIN(__builtin_popcountll)
#  define Pop64(x) __builtin_popcountll((uint64_t)(x))
# endif
# if SLIMCC_HAS_BUILTIN(__builtin_ctzll)
#  define Ctz64(x) __builtin_ctzll((uint64_t)(x))
# endif
# if SLIMCC_HAS_BUILTIN(__builtin_clzll)
#  define Clz64(x) __builtin_clzll((uint64_t)(x))
# endif
#endif

#if defined(__has_include)
# if __has_include(<stdbit.h>)
#  include <stdbit.h>
#  ifndef Pop64
#   define Pop64(x) (int)stdc_count_ones((uint64_t)(x))
#  endif
#  ifndef Ctz64
#   define Ctz64(x) (int)stdc_trailing_zeros((uint64_t)(x))
#  endif
#  ifndef Clz64
#   define Clz64(x) (int)stdc_leading_zeros((uint64_t)(x))
#  endif
# endif
#endif

#ifndef Pop64
# define Pop64(x) _pop64_impl(x)
static inline int _pop64_impl(uint64_t v) {
  int cnt = 0;
  for (; v; cnt++)
    v &= v - 1;
  return cnt;
}
#endif

#ifndef Ctz64
# define Ctz64(x) _ctz64_impl(x)
static inline int _ctz64_impl(uint64_t v) {
  return Pop64((v & -v) - 1);
}
#endif

#ifdef __clang__
# pragma clang diagnostic ignored "-Wswitch"
#endif

#if defined(__GNUC__)
# define BUFF_CAST(_t, _ptr)  \
   __extension__({            \
    union U {                 \
      char _m1;               \
      _t _m2;                 \
    };                        \
    ((union U *)(_ptr))->_m2; \
   })
#else
# define BUFF_CAST(_t, _ptr) (*((_t *)(_ptr)))
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
# define S_ASSERT(x) static_assert((x), "");
#else
# define S_ASSERT(x)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
# define ANON_UNION_START union {
# define ANON_UNION_END \
   }                    \
   ;
#else
# define ANON_UNION_START
# define ANON_UNION_END
#endif

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define Ucast(c) (unsigned int)(unsigned char)(c)
#define Inrange(c, x, y) ((Ucast(c) - Ucast(x)) <= (Ucast(y) - Ucast(x)))
#define Isdigit(c) Inrange((c), '0', '9')
#define Isalpha(c) Inrange((c) | 0x20, 'a', 'z')
#define Isalnum(c) (Isalpha(c) || Isdigit(c))
#define Isxdigit(c) (Isdigit(c) || Inrange((c) | 0x20, 'a', 'f'))
#define Casecmp(c, a) (((c) | 0x20) == a)

#ifdef BOOTSTRAP_NO_LDOUBLE
typedef double long_double_t;
#else
typedef long double long_double_t;
#endif

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;
typedef struct LocalLabel LocalLabel;
typedef struct EnumVal EnumVal;
typedef union FPVal FPVal;
typedef struct AsmContext AsmContext;
typedef struct FuncObj FuncObj;
typedef struct SlashDelta SlashDelta;

//
// alloc.c
//

typedef struct Pool Pool;
typedef struct {
  Pool *cur;
  Pool *head;
  int used;
} Arena;

char *arena_format(Arena *arena, const char *fmt, ...);
char *arena_strdup(Arena *arena, const char *str);
char *arena_copy_string(Arena *arena, const char *src, size_t len);
void arena_on(Arena *arena);
void arena_off(Arena *arena);
extern TLS bool ast_arena_keep;
void *arena_calloc(Arena *a, size_t sz);
void *arena_malloc(Arena *a, size_t sz);

bool check_mem_usage(void);

extern TLS Arena ast_arena;
extern TLS Arena cc1_arena;
extern TLS Arena pp_arena;
extern TLS bool free_alloc;

//
// hashmap.c
//

typedef struct {
  const char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int32_t capacity;
  int32_t used;
} HashMap;

HashEntry *hashmap_get_or_insert(HashMap *map, const char *key, int keylen);
void *hashmap_get(HashMap *map, const char *key);
void *hashmap_get2(HashMap *map, const char *key, int keylen);
void hashmap_put(HashMap *map, const char *key, void *val);
void hashmap_put2(HashMap *map, const char *key, int keylen, void *val);
void hashmap_delete(HashMap *map, const char *key);
void hashmap_delete2(HashMap *map, const char *key, int keylen);
void hashmap_test(void);
void hashmap_reserve(HashMap *map, int cap);
uint64_t hashmap_hash(const char *key, int keylen);
void *hashmap_get2h(HashMap *map, const char *key, int keylen, uint64_t hash);


// Represents a deleted hash entry
#define TOMBSTONE ((void *)-1)
#define TOMBSTONE_CASE ((uintptr_t)-1)

//
// strings.c
//

#define GrowArr(_data, _cap)                         \
  do {                                               \
    _data = grow_array(_data, sizeof(*_data), _cap); \
  } while (0)

typedef struct {
  const char **data;
  int32_t capacity;
  int32_t len;
} StringArray;

void *grow_array(void *data, size_t sz, int32_t *cap);
void strarray_push(StringArray *arr, const char *s);
char *format(const char *fmt, ...) FMTCHK(1, 2);

//
// tokenize.c
//

typedef enum {
  TK_IDENT,   // Identifiers
  TK_KEYWORD, // Keywords
  TK_STR,     // String literals
  TK_ASM_STR,
  TK_CHAR_LIT, // Character literals
  TK_CHAR_LIT_PPEV = TK_CHAR_LIT + 1,
  TK_PP_NUM, // Preprocessing numbers
  TK_PP_NUM_PPEV = TK_PP_NUM + 1,
  TK_INTMAX_NUM,
  TK_FMARK,  // Filemarkers for -E
  TK_PMARK,  // Placermarkers
  TK_ATTR,   // GNU attribute
  TK_BATTR,  // C23 attribute
  TK_PRAGMA, // #pragma's
  TK_EOF,    // End-of-file markers
  TK_INVALID,

  // Punctuators
  TK_PUNCT,
  TK_LPAREN,
  TK_RPAREN,
  TK_COMMA,
  TK_SEMI,
  TK_QMARK,
  TK_LBRACK,
  TK_RBRACK,
  TK_LCURLY,
  TK_RCURLY,
  TK_BITNOT,
  TK_LANGLE,
  TK_LANGLE_EQ,
  TK_LANGLE2,
  TK_RANGLE,
  TK_RANGLE_EQ,
  TK_RANGLE2,
  TK_NOT,
  TK_NOT_EQ,
  TK_REM,
  TK_MUL,
  TK_DIV,
  TK_XOR,
  TK_EQ2,
  TK_HASH,
  TK_HASH2,
  TK_DOT,
  TK_DOT3,
  TK_COLON,
  TK_COLON2,
  TK_AND,
  TK_AND2,
  TK_ADD,
  TK_ADD2,
  TK_SUB,
  TK_SUB2,
  TK_OR,
  TK_OR2,
  TK_ARROW,

  TK_EQ,
  TK_ADD_EQ,
  TK_SUB_EQ,
  TK_MUL_EQ,
  TK_DIV_EQ,
  TK_REM_EQ,
  TK_AND_EQ,
  TK_OR_EQ,
  TK_XOR_EQ,
  TK_LANGLE2_EQ,
  TK_RANGLE2_EQ,

  TK_PUNCT_END,

  TK_return,
  TK_if,
  TK_else,
  TK_for,
  TK_while,
  TK_do,
  TK_goto,
  TK_break,
  TK_continue,
  TK_switch,
  TK_case,
  TK_default,
  TK_sizeof,
  TK_Generic,
  TK_Countof,
  TK_alignof,
  TK_asm,
  TK_static_assert,
  TK_true,
  TK_false,
  TK_nullptr,
  TK_defer,
  TK_FUNCTION,
  TK_GNU_label,

  TK_TYPEKW,
  TK_void,
  TK_char,
  TK_short,
  TK_int,
  TK_long,
  TK_float,
  TK_double,
  TK_unsigned,
  TK_struct,
  TK_union,
  TK_enum,
  TK_typedef,
  TK_static,
  TK_extern,
  TK_auto,
  TK_register,
  TK_Atomic,
  TK_Noreturn,
  TK_BitInt,
  TK_int128,
  TK_auto_type,
  TK_alignas,
  TK_bool,
  TK_const,
  TK_constexpr,
  TK_inline,
  TK_restrict,
  TK_signed,
  TK_typeof,
  TK_typeof_unqual,
  TK_thread_local,
  TK_volatile,
  TK_TYPEKW_END,
} TokenKind;

typedef enum {
  INCL_ABS = -2,
  INCL_REL = -1,
} InclIdx;

typedef struct File File;
struct File {
  const char *name;
  const char *contents;
  int file_no;

  int display_file_no;
  int line_delta;
  InclIdx incl_idx;
  bool is_syshdr;
  bool is_placeholder;
};

typedef struct Token Token;
struct Token {
  Token *next;
  TokenKind kind : 16;
  bool at_bol : 1;      // True if this token is at beginning of line
  bool has_space : 1;   // True if this token follows a space character
  bool dont_expand : 1; // True if a macro name is encountered during its expansion
  bool is_incl_guard : 1;
  bool is_root : 1;
  bool is_live : 1;
  bool has_ucn : 1;
  int len;         // Token length
  const char *loc; // Token location
  File *file;
  Token *origin; // If this is expanded from a macro, the original token
  int line_no;   // Line number
  int display_line_no;
  int display_file_no;
  uint32_t hash;   // loc・len の名前の表のハッシュ（下位 32 ビット。0 はまだ。tok_hash。2026-11-16）
  Type *ty; // Used if TK_INT_NUM or TK_STR
  ANON_UNION_START
  Token *attr_next;
  Token *alloc_next;
  ANON_UNION_END
  ANON_UNION_START
  int64_t ival; // If kind is TK_INT_NUM, its value
  char *str;    // String literal contents including terminating '\0'
  Token *label_next;
  ANON_UNION_END
};

// トークンの名前のハッシュ（覚えておき、名前の表を何度も引くときに計算し直さない）。表の枠は 2^32 より
// 少ないので、下位 32 ビットだけで同じ場所を探す
static inline uint64_t tok_hash(Token *tok) {
  if (!tok->hash)
    tok->hash = (uint32_t)hashmap_hash(tok->loc, tok->len);
  return tok->hash;
}

NORETURN void error_ice(const char *file, int32_t line);
NORETURN void error(const char *fmt, ...) FMTCHK(1, 2);
NORETURN void error_at(const char *loc, const char *fmt, ...) FMTCHK(2, 3);
NORETURN void error_tok(Token *tok, const char *fmt, ...) FMTCHK(2, 3);
void warn_tok(Token *tok, const char *fmt, ...) FMTCHK(2, 3);
void notice_tok(Token *tok, const char *fmt, ...) FMTCHK(2, 3);
void verror_at_tok(Token *tok, const char *fmt, va_list ap);
bool equal(Token *tok, const char *op);
bool equal_ext(Token *tok, const char *op);
Token *skip_tk(Token *tok, TokenKind);
bool consume(Token **rest, Token *tok, const char *str);
bool consume_tk(Token **rest, Token *tok, TokenKind kind);
Token *tokenize_file(const char *path, Token *tok, Token **end);
Token *tokenize_main_file(const char *path);
void ir_mem_reset(void);    // 中間表現で関数ごとに使った量の記録（cc/ir.inc。slimcc_set_mem_hook）
void ir_mem_biggest(const char **fn, size_t *bytes);  // 本文の .c（ファイルの覚えに入れない。1 回しか読まないため）
File *new_file(const char *name, const char *contents);
int add_display_file(const char *path);
void tokenize_string_literal(Token *tok, Type *basety);
Token *tokenize(File *file, SlashDelta *delta, Token **end);
void tokenize_reset(void);
void set_memory_file(const char *name, const char *text);
void convert_pp_number(Token *tok, Node *node);
bool is_pp_token_int(Token *tok);
TokenKind ident_keyword(Token *tok);
void convert_ucn_ident(Token *tok);

#define internal_error() error_ice(__FILE__, __LINE__)

//
// preprocess.c
//

void init_macros(void);
void define_macro(const char *name, const char *buf);
void define_macro_cli(const char *str);
void undef_macro(const char *name);
void dump_defines(FILE *out);
Token *preprocess(const char *file, StringArray *incls, StringArray *macros);
Token *prepare_parse(Token *tok);
void preprocess_reset(void);
void print_tokens(Token *tok, FILE *out);

// -D と -U（options.c）
typedef struct {
  const char *arg;
  bool is_def;
} MacroChange;

typedef struct {
  MacroChange *data;
  int32_t capacity;
  int32_t len;
} MacroChangeArr;

extern TLS MacroChangeArr macrodefs;
extern TLS bool opt_no_pic_macros;
void macrochange_push(MacroChangeArr *arr, const char *arg, bool is_def);
void define_cli_macros(bool is_asm_pp);
extern TLS Token *last_alloc_tok;
extern TLS Token *tok_freelist;

//
// parse.c
//

typedef enum {
  OBJ_GLOBAL = 0,
  OBJ_TLS,
  OBJ_LOCAL,
  OBJ_INDIR,
} ObjKind;

typedef struct Obj Obj;
struct Obj {
  Obj *next;
  char *name;
  Type *ty;
  ObjKind kind;
  bool is_live;
  bool is_used;
  bool is_compound_lit;
  bool is_string_lit;
  int alt_align;
  // ir.inc の ir_slot の覚え: ir_gen が今の関数の番号（ir_slot_gen）と同じなら、場所は ir_ofs（2026-11-27）
  int64_t ir_gen;
  int ir_ofs;

  // Local variable
  int ofs;
  const char *ptr;
  Obj *param_next;
  bool pass_by_stack;
  bool is_zero_sized_arg;
  int stack_offset;
  // AArch64: codegen.c の calling_convention が決める引数の渡し方
  bool pass_by_ref; // 16 バイトを超える構造体などを、コピーへのポインタで渡す
  int arg_reg;      // レジスタで渡すとき、最初のレジスタ番号（x または v）。スタックなら -1
  // codegen.c のレジスタ割り当て: 置いたレジスタ（x21〜x28 なら 21〜28、d8〜d15 なら 108〜115）。0 はメモリ
  int reg;
  bool reg_pinned;    // アドレスやメモリが必要（レジスタに置けない）
  int64_t reg_weight; // 使われた回数（ループの中ほど重い）
  Node *arg_expr;
  Obj *vptr;

  // Global variable or function
  bool is_definition;
  bool is_static;
  bool is_weak;
  bool is_static_lvar;
  Obj *static_lvars;
  char *alias_name;
  char *visibility;
  char *asm_name;

  // Global variable
  bool is_common;
  bool is_nocommon;
  char *section_name;
  char *init_data;
  Relocation *rel;

  // constexpr variable
  char *constexpr_data;

  // Function
  bool export_fn;
  bool is_gnu_inline;
  bool is_always_inline;
  bool is_naked;
  bool is_noreturn;
  bool returns_twice;
  bool dont_reuse_stk;
  bool dealloc_vla;
  bool is_ctor;
  bool is_dtor;
  uint16_t ctor_prior;
  uint16_t dtor_prior;
  Node *body;
  FuncObj *output; // backend defined output object
  // 中間表現のコード生成（ir.inc）: 生成できた関数の大きさ（命令の数）と、インライン展開してよいか
  bool ir_inlinable;
  int ir_size;
};

struct Relocation {
  Relocation *next;
  int offset;
  Node *label;
  Obj *var;
  int64_t addend;
};

typedef enum {
  DF_VLA_DEALLOC,
  DF_CLEANUP_FN,
  DF_DEFER_STMT,
} DeferKind;

typedef struct DeferStmt DeferStmt;
struct DeferStmt {
  DeferKind kind;
  DeferStmt *next;
  Obj *vla;
  Node *cleanup_fn;
  Node *stmt;
};

typedef enum {
  ASMOP_NULL = 0,
  ASMOP_NUM,
  ASMOP_SYMBOLIC,
  ASMOP_MEM,
  ASMOP_REG,
  ASMOP_FLAG,
} AsmOpKind;

typedef struct AsmParam AsmParam;
struct AsmParam {
  AsmParam *next;
  AsmOpKind kind;
  Token *name;
  Token *constraint;
  char *flag;
  AsmParam *match;
  Node *arg;
  Obj *ptr;
  Obj *var;
  int64_t val;
  uint64_t reg_constraint;
  int label_id;
  int reg;
  int var_asm_reg;
  bool is_mem_inreg;
  bool is_early_clobber;
  bool is_clobbered_x87;
  // AArch64
  bool is_fp_reg;    // SIMD レジスタ（w 制約）
  bool use_zero_reg; // 値が 0 の即値を、ゼロレジスタ（xzr/wzr）として渡す（Z 制約）
  bool is_inout;     // + 制約（入出力）
};

// AST node
typedef enum {
  ND_NULL_STMT,
  ND_NULL_EXPR, // Do nothing
  ND_ADD,       // +
  ND_SUB,       // -
  ND_MUL,       // *
  ND_DIV,       // /
  ND_POS,       // unary +
  ND_NEG,       // unary -
  ND_REM,       // %
  ND_BITAND,    // &
  ND_BITOR,     // |
  ND_BITXOR,    // ^
  ND_SHL,       // <<
  ND_SHR,       // >>
  ND_SAR,       // arithmetic >>
  ND_EQ,        // ==
  ND_NE,        // !=
  ND_LT,        // <
  ND_LE,        // <=
  ND_GT,        // >
  ND_GE,        // >=
  ND_ASSIGN,    // =
  ND_COND,      // ?:
  ND_COMMA,     // ,
  ND_MEMBER,    // . (struct member access)
  ND_ADDR,      // unary &
  ND_DEREF,     // unary *
  ND_NOT,       // !
  ND_BITNOT,    // ~
  ND_LOGAND,    // &&
  ND_LOGOR,     // ||
  ND_RETURN,    // "return"
  ND_IF,        // "if"
  ND_FOR,       // "for" or "while"
  ND_DO,        // "do"
  ND_SWITCH,    // "switch"
  ND_CASE,      // switch cases
  ND_DEFAULT,   // switch default case
  ND_BLOCK,     // { ... }
  ND_BREAK,
  ND_CONT,
  ND_GOTO,      // "goto"
  ND_GOTO_EXPR, // "goto" labels-as-values
  ND_ASM_GOTO,
  ND_LABEL,     // Labeled statement
  ND_LABEL_VAL, // [GNU] Labels-as-values
  ND_FUNCALL,   // Function call
  ND_EXPR_STMT, // Expression statement
  ND_STMT_EXPR, // Statement expression
  ND_VAR,       // Variable
  ND_NUM,       // Integer
  ND_CAST,      // Type cast
  ND_INIT_SEQ,
  ND_ASM,      // "asm"
  ND_CAS,      // Atomic compare-and-swap
  ND_EXCH,     // Atomic exchange
  ND_VA_START, // "va_start"
  ND_VA_COPY,  // "va_copy"
  ND_VA_ARG,   // "va_arg"
  ND_CHAIN,
  ND_ALLOCA,
  ND_ALLOCA_ZINIT,
  ND_ARITH_ASSIGN,
  ND_POST_INCDEC,
  ND_CKD_ARITH,
  ND_FRAME_ADDR,
  ND_RTN_ADDR,
  ND_THREAD_FENCE,
  ND_UNREACHABLE,
  ND_UNKNOWN,
} NodeKind;

typedef struct CaseRange CaseRange;
struct CaseRange {
  CaseRange *next;
  Node *label;
  int64_t lo;
  int64_t hi;
};

typedef union {
  uint64_t as64;
  uint32_t as32;
  uint8_t as8;
} BitBuf;

// AST node type
struct Node {
  Node *next;
  NodeKind kind;
  NodeKind arith_kind : 30; // Arithmetic Assignment
  bool no_label : 1;
  bool is_nonlval : 1;
  Type *ty;
  Token *tok; // Representative token

  DeferStmt *dfr_from;
  DeferStmt *dfr_dest;

  ANON_UNION_START
  // Misc
  struct {
    Node *lhs;
    Node *rhs;
    Node *target;
    Obj *var;
    Member *member;
  } m;

  // Numeric literal
  struct {
    int64_t val;
    BitBuf *bitint_data;
    long_double_t fval;
    enum {
      MATH_CONSTANT_NOT = 0,
      MATH_CONSTANT_NANF,
      MATH_CONSTANT_INFF,
      MATH_CONSTANT_NANSF,
      MATH_CONSTANT_NANS,
      MATH_CONSTANT_NANSL,
    } constant;
  } num;

  // Block or statement expression
  struct {
    Node *body;
    Node *local_labels;
    Node *result;
  } blk;

  // if, ?:, for, do, while, switch
  struct {
    Node *cond;
    Node *then;
    ANON_UNION_START
    Node *els;
    Node *for_init;
    Node *sw_default;
    ANON_UNION_END
    ANON_UNION_START
    Node *for_inc;
    CaseRange *sw_cases;
    ANON_UNION_END
    int64_t id;
  } ctrl;

  // labels
  struct {
    Node *next;
    int64_t id;
  } lbl;

  // case
  struct {
    Node *parent_sw;
    int64_t id;
  } cases;

  // break, continue, goto
  struct {
    Node *parent_loop;
    Node *target;
  } jmp;

  // Function call
  struct {
    Node *expr;
    Obj *rtn_buf;
    Obj *args;
    Obj *alloca_args;
  } call;

  // Atomic compare-and-swap
  struct {
    Node *addr;
    Node *old_val;
    Node *new_val;
  } cas;

  // GNU inline assembly
  struct {
    Token *str_tok;
    AsmParam *outputs;
    AsmParam *inputs;
    Token *clobbers;
    AsmParam *labels;
    AsmContext *ctx; // backend defined
  } gasm;
  ANON_UNION_END
};

// Represents a block scope.
typedef struct Scope Scope;
struct Scope {
  Scope *parent;
  Scope *children;
  Scope *sibling_next;

  Obj *locals;
  LocalLabel *labels;
  Node *gotos;
  bool is_temporary;
  bool is_stmt;
  bool is_fn_base;
  bool has_label;
  bool has_alloca;

  HashMap vars;
  HashMap tags;
};

Node *new_cast(Node *expr, Type *ty);
int64_t const_expr(Token **rest, Token *tok);
int64_t eval_sign_extend(Type *ty, int64_t val);
void eval_fp(Node *node, FPVal *fval);
Obj *parse(Token *tok);
void parse_reset(void);
Token *skip_paren(Token *tok);
Node *new_node(NodeKind kind, Token *tok);
Node *new_var_node(Obj *obj, Token *tok);
Obj *new_lvar(Type *ty);
bool is_const_var(Obj *var);
bool is_const_expr(Node *node, int64_t *val);
bool is_const_expr_strict(Node *node, int64_t *val);
bool is_const_fp(Node *node, FPVal *fval);
bool is_const_zero_bitint(Node *node);
Obj *eval_var_opt(Node *node, int *ofs, bool let_array, bool let_atomic);
bool equal_tok(Token *a, Token *b);
Obj *get_symbol_var(const char *);
Type *vla_cond_result_len(Type *ty1, Type *ty2, Type *base, Node **cond, Obj **cond_var);

//
// bitint.c
//

int32_t eval_bitint_first_set(int32_t bits, BitBuf *op);
bool eval_bitint_to_bool(int32_t bits, BitBuf *op);
void eval_bitint_sign_ext(int32_t bits, BitBuf *op, int32_t bits2, bool is_unsigned);
void eval_bitint_neg(int32_t bits, BitBuf *op);
void eval_bitint_bitnot(int32_t bits, BitBuf *op);
void eval_bitint_bitand(int32_t bits, BitBuf *lh, BitBuf *rh);
void eval_bitint_bitor(int32_t bits, BitBuf *lh, BitBuf *rh);
void eval_bitint_bitxor(int32_t bits, BitBuf *lh, BitBuf *rh);
void eval_bitint_shl(int32_t bits, BitBuf *src, BitBuf *dst, int32_t amount);
void eval_bitint_shr(int32_t bits, BitBuf *src, BitBuf *dst, int32_t amount,
                     bool is_unsigned);
void *eval_bitint_bitfield_load(int32_t bits, BitBuf *src, BitBuf *dst, int32_t width,
                                int32_t ofs, bool is_unsigned);
void eval_bitint_bitfield_save(int32_t bits, BitBuf *src, BitBuf *dst, int32_t width,
                               int32_t ofs);
void eval_bitint_add(int32_t bits, BitBuf *lh, BitBuf *rh);
void eval_bitint_sub(int32_t bits, BitBuf *lh, BitBuf *rh);
void eval_bitint_mul(int32_t bits, BitBuf *lh, BitBuf *rh);
void eval_bitint_div(int32_t bits, BitBuf *lh, BitBuf *rh, bool is_unsigned, bool is_div);
int eval_bitint_cmp(int32_t bits, BitBuf *lh, BitBuf *rh, bool is_unsigned);

//
// type.c
//

typedef enum {
  ETY_I8 = 0,
  ETY_U8,
  ETY_I16,
  ETY_U16,
  ETY_I32,
  ETY_U32,
  ETY_I64,
  ETY_U64,
} EnumType;

struct EnumVal {
  EnumVal *next;
  Token *name;
  int64_t val;
  Type *ty;
};

union FPVal {
  uint64_t chunk[2];
  uint32_t chunk32[4];
  long_double_t ld;
  double d;
  float f;
};

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_PCHAR,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_LONGLONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_ENUM_INCMP,
  TY_PTR,
  TY_NULLPTR,
  TY_FUNC,
  TY_FUNC_INCMP,
  TY_ARRAY,
  TY_ARRAY_INCMP,
  TY_VLA, // variable-length array
  TY_STRUCT,
  TY_UNION,
  TY_BITINT,
  TY_AUTO,
  TY_ASM,
} TypeKind;

typedef enum {
  Q_NONE = 0,
  Q_CONST = 1,
  Q_VOLATILE = 1 << 1,
  Q_ATOMIC = 1 << 2,
  Q_RESTRICT = 1 << 3,
} QualMask;

struct Type {
  TypeKind kind;
  int64_t size;
  int32_t align;
  bool is_unsigned;
  bool is_enum;
  bool is_int_enum;
  bool is_fixed_enum;
  QualMask qual;
  Type *origin;
  Type *decl_next; // forward declarations
  Token *tag;
  EnumVal *enums;

  // Pointer-to or array-of type.
  Type *base;

  // _BitInt
  int64_t bit_cnt;

  // Array
  int64_t array_len;

  // Variable-length array
  Node *vla_len_expr;
  Obj *vla_len_val;

  // Struct
  Member *members;
  bool is_flexible;
  bool is_constructing;
  bool has_const_member;

  // Function type
  Scope *scopes;
  Type *return_ty;
  Obj *param_list;
  Node *pre_calc;
  bool is_variadic;
  bool is_oldstyle;
};

// Struct member
struct Member {
  Member *next;
  Type *ty;
  Token *name;
  int64_t offset;
  int idx;
  int alt_align;
  bool is_packed;

  bool is_bitfield;
  bool is_aligned_bitfield;
  int bit_offset;
  int bit_width;
};

extern TLS Type *ty_void;
extern TLS Type *ty_bool;
extern TLS Type *ty_nullptr;

extern TLS Type *ty_pchar;

extern TLS Type *ty_char;
extern TLS Type *ty_short;
extern TLS Type *ty_int;
extern TLS Type *ty_long;
extern TLS Type *ty_llong;

extern TLS Type *ty_uchar;
extern TLS Type *ty_ushort;
extern TLS Type *ty_uint;
extern TLS Type *ty_ulong;
extern TLS Type *ty_ullong;

extern TLS Type *ty_float;
extern TLS Type *ty_double;
extern TLS Type *ty_ldouble;

extern TLS Type *ty_size_t;
extern TLS Type *ty_ptrdiff_t;

extern TLS Type *ty_intmax_t;
extern TLS Type *ty_uintmax_t;
extern TLS Type *ty_eval_int;

extern TLS Type *ty_char16_t;
extern TLS Type *ty_char32_t;
extern TLS Type *ty_wchar_t;

extern TLS Type *enum_ty[8];
extern TLS EnumType ety_of_int;

bool is_pow_of_two(uint64_t val);
bool is_integer(Type *ty);
bool is_int_class(Type *ty);
bool is_flonum(Type *ty);
bool is_numeric(Type *ty);
bool is_array(Type *ty);
bool is_decay_ty(Type *ty);
bool is_bitfield(Node *node);
bool is_redundant_cast(Node *expr, Type *ty);
bool is_compatible(Type *t1, Type *t2);
bool is_compatible2(Type *t1, Type *t2);
bool is_record_compat(Type *t1, Type *t2, bool is_redecl);
bool is_null_ptr_constant(Node *node);
bool is_ptr(Type *ty);
int next_pow_of_two(int val);
int64_t bit_size(Type *ty);
int64_t bitint_buffer_size(Type *ty);
int32_t bitfield_footprint(Member *mem);
void init_ty_lp64(void);
Type *copy_type(Type *ty);
Type *pointer_to(Type *base);
Type *ptr_decay(Type *ty);
void ptr_convert(Node **node);
void unqual_rtn_ty(Type *ty, Token *tok);
Type *func_type(TypeKind kind, Type *return_ty);
Type *get_func_ty(Node *node);
Type *array_type(Type *base);
Type *array_of(Type *base, int64_t size);
Type *vla_of(Type *base, Node *expr, int64_t arr_len);
void array_setty(Type *ty, Type *base, int64_t size);
void vla_setty(Type *ty, Node *size, int64_t arr_len);
Type *new_type(TypeKind kind, int64_t size, int align);
void type_reset(void);
Type *new_bitint(int64_t width, Token *tok);
Type *get_elem(Type *ty);
Type *unqual(Type *ty);
Type *tyof_unqual(Type *ty);
Type *new_derived_type(Type *newty, QualMask qual, Type *ty);
bool chk_qual_type(QualMask qual, Type *ty);
Type *aligned_type(int align, Type *ty);
Type *add_qual(QualMask msk, Type *ty, Token *tok);
bool mem_iter(Member **mem);
bool vmty_iter(Type **ty);
Node *assign_cast(Type *to, Node *expr);

void _add_type(Node *node);
#define add_type(_n) \
  do {               \
    if (!(_n)->ty)   \
      _add_type(_n); \
  } while (0)

//
// codegen.c
//

int codegen(Obj *prog, FILE *out);
void codegen_reset(void);
void prepare_funcall(Node *node, Scope *scope);
void prepare_inline_asm(Node *node);
int64_t align_to(int64_t n, int64_t align);
bool va_arg_need_copy(Type *ty);
bool bitint_rtn_need_copy(size_t width);
void emit_text(Obj *fn);

//
// assembler.c（内蔵アセンブラ。codegen.c が出力する GNU as 形式のアセンブリを機械語にする）
//

// ELF の定数（assembler.c と、ELF を書き出す側で使う。<elf.h> のない環境でもビルドできるように定義する）
#define ASM_SHT_PROGBITS 1
#define ASM_SHT_NOTE 7
#define ASM_SHT_NOBITS 8
#define ASM_SHT_INIT_ARRAY 14
#define ASM_SHT_FINI_ARRAY 15
#define ASM_SHT_PREINIT_ARRAY 16
#define ASM_SHF_WRITE 0x1
#define ASM_SHF_ALLOC 0x2
#define ASM_SHF_EXECINSTR 0x4
#define ASM_SHF_MERGE 0x10
#define ASM_SHF_STRINGS 0x20
#define ASM_SHF_GROUP 0x200
#define ASM_SHF_TLS 0x400
#define ASM_STB_LOCAL 0
#define ASM_STB_GLOBAL 1
#define ASM_STB_WEAK 2
#define ASM_STT_NOTYPE 0
#define ASM_STT_OBJECT 1
#define ASM_STT_FUNC 2
#define ASM_STT_TLS 6
#define ASM_STV_DEFAULT 0
#define ASM_STV_INTERNAL 1
#define ASM_STV_HIDDEN 2
#define ASM_STV_PROTECTED 3

typedef struct AsmSection AsmSection;

typedef struct {
  int64_t offset;
  int type;        // R_AARCH64_*
  const char *sym; // シンボル名。セクションの中の位置を指すときは、セクション名
  bool is_section; // sym がセクション名
  int64_t addend;
} AsmReloc;

struct AsmSection {
  char *name;
  uint32_t type;
  uint64_t flags;
  int64_t align;
  uint8_t *data;   // SHT_NOBITS では NULL
  int64_t size;
  int64_t cap;
  AsmReloc *rels;
  int32_t nrel;
  int32_t caprel;
  int32_t index;
};

typedef struct {
  char *name;
  AsmSection *sect; // 未定義なら NULL
  int64_t value;
  int64_t size;
  uint8_t bind;
  uint8_t type;
  uint8_t vis;
  bool variant_pcs; // .variant_pcs（st_other の STO_AARCH64_VARIANT_PCS）
  bool bind_set;    // .globl/.weak/.local で指定された
  bool defined;
  bool is_common;
  bool referenced;
  int64_t common_align;
  char *equ;        // .set で別のシンボルと同じにしたとき、その名前
  bool rel_named;   // 名前で参照するリロケーションがある（resolve_fixups が付ける。asm_write_elf の .L。2026-11-27）
} AsmSymbol;

typedef struct {
  AsmSection **sects;
  int32_t nsect;
  int32_t capsect;
  AsmSymbol **syms;
  int32_t nsym;
  int32_t capsym;
  char **strs;      // asm_obj_load が作った、リロケーションの名前（セクション・シンボルの名前でないもの）
  int32_t nstr;
  bool named_marked; // resolve_fixups が rel_named を付けた（付けていない（asm_obj_load で読んだ）なら名前の表で決める）
} AsmObj;

AsmObj *asm_assemble(const char *text, const char *filename);
void asm_dump(AsmObj *obj, FILE *out);
void asm_write_elf(AsmObj *obj, FILE *out);
void asm_free(AsmObj *obj);
// コンパイルした結果をファイルに書く・読む（Monocs のコンパイルのキャッシュ。形式は assembler.c）
bool asm_obj_save(AsmObj *obj, FILE *out);
AsmObj *asm_obj_load(FILE *in);
AsmObj *asm_obj_load_mem(const void *p, size_t n);  // メモリの上の [p, p + n) から（asm_obj_load と同じ形）

//
// unicode.c
//

int encode_utf8(char *buf, uint32_t c);
uint32_t decode_utf8(const char **new_pos, const char *p);
bool is_ident1(uint32_t c);
bool is_ident2(uint32_t c);
int display_width(const char *p, int len);

//
// platform.c
//

void platform_init_driver(void);
void platform_init_cc1(void);
void platform_stdinc_paths(StringArray *paths);
void platform_search_dirs(StringArray *paths);
void run_assembler(StringArray *as_args, const char *input, const char *output);
void run_linker(StringArray *paths, StringArray *args, const char *output);

//
// main.c
//

typedef enum { STD_C89, STD_C94, STD_C99, STD_C11, STD_C17, STD_C23, STD_C2Y } StdVer;

typedef enum {
  LT_RELO,
  LT_SHARED,
  LT_DYNAMIC,
  LT_STATIC_PIE,
  LT_STATIC,
  LT_PIE,
} LinkType;

NORETURN void cleanup_exit(int status);
bool file_exists(const char *path);
bool in_sysincl_path(int idx);
bool ignore_missing_dep(const char *path, const char *filename, Token *tok);
void add_dep_file(const char *path, bool is_sys);
char *find_dir_w_file(const char *pattern);
void run_subprocess(const char **argv);
void add_include_path(StringArray *arr, const char *s);

LinkType link_type_gnustyle(void);
void ldarg_gnu_base(StringArray *arr, const char *output);
void ldarg_gnu_linktype(StringArray *arr, LinkType lt, const char *ldso_path);
void ldarg_gnu_crtbegin(StringArray *arr, LinkType lt, const char *gcc_libpath);
void ldarg_gnu_inputs(StringArray *arr, StringArray *paths, StringArray *args);
void ldarg_gnu_lc_lgcc(StringArray *arr, LinkType lt, bool has_libgcc);
void ldarg_gnu_crtend(StringArray *arr, LinkType lt, const char *gcc_libpath);
void run_assembler_gnustyle(StringArray *as_args, const char *input, const char *output);
void run_linker_gnustyle(StringArray *paths, StringArray *inputs, const char *output,
                         const char *ldso_path, const char *libpath,
                         const char *gcclibpath);

extern TLS char *argv0;
extern TLS StringArray include_paths;
extern TLS StringArray iquote_paths;
extern TLS StringArray display_files;
extern TLS bool opt_E;
extern TLS bool opt_dM;
extern TLS bool opt_P;
extern TLS int opt_fpic;
extern TLS int opt_fpie;
extern TLS bool opt_femulated_tls;
extern TLS int opt_fn_align;
extern TLS bool opt_use_plt;
extern TLS bool opt_fcommon;
extern TLS bool opt_optimize;
extern TLS bool opt_reuse_stack;
extern TLS bool opt_g;
extern TLS bool opt_func_sections;
extern TLS bool opt_openmp;
extern TLS int opt_fp_contract;
extern TLS bool opt_data_sections;
extern TLS bool opt_werror;
extern TLS const char *opt_visibility;
extern TLS bool opt_cc1_asm_pp;
extern TLS StdVer opt_std;
extern TLS bool is_iso_std;
extern TLS bool opt_fdefer_ts;
extern TLS bool opt_ir; // 中間表現（SSA）を使うコード生成（codegen.c の ir.inc）。既定で真、-fno-slimcc-ir で偽
extern TLS bool opt_strict_aliasing; // 型による別名の判定（ir.inc）。既定で真、-fno-strict-aliasing で偽
extern TLS bool opt_unroll; // ループの展開（ir.inc）。既定で真、-fno-unroll-loops で偽
extern TLS bool opt_sched; // 命令の並べ替え（codegen.c の sched.inc）。既定で真、-fno-schedule-insns2 で偽
extern TLS bool opt_short_enums;
extern TLS bool opt_gnu_keywords;
extern TLS bool opt_gnu89_inline;
extern TLS bool opt_ms_anon_struct;
extern TLS bool opt_disable_visibility;
extern TLS bool opt_fake_always_inline;

extern TLS bool opt_pie;
extern TLS bool opt_nopie;
extern TLS bool opt_pthread;
extern TLS bool opt_r;
extern TLS bool opt_rdynamic;
extern TLS bool opt_static;
extern TLS bool opt_static_pie;
extern TLS bool opt_static_libgcc;
extern TLS bool opt_shared;
extern TLS bool opt_s;
extern TLS bool opt_nostartfiles;
extern TLS bool opt_nodefaultlibs;
extern TLS bool opt_nolibc;
extern TLS const char *default_ld;
extern TLS const char *default_as;
extern TLS const char *dumpmachine_str;

// SLIMCC_LIBRARY（libslimcc.c として何度もコンパイルする）とき、コンパイル中の確保はゾーン（zone.c）から行い、
// コンパイルの終わりにまとめて捨てる。zone.c、assembler.c、memlink.c、libslimcc.c は SLIMCC_NO_ZONE で除く
void zone_begin(void);
void zone_end(void);
size_t zone_total(void);
void *zone_malloc(size_t n);
void *zone_calloc(size_t n, size_t sz);
void *zone_realloc(void *p, size_t n);
void zone_free(void *p);
char *zone_strdup(const char *s);
char *zone_strndup(const char *s, size_t n);
void alloc_reset(void);
#ifdef SLIMCC_LIBRARY
void *heap_malloc(size_t n);
void heap_free(void *p);
#else
# define heap_malloc(n) malloc(n)
# define heap_free(p) free(p)
#endif
void file_cache_set(bool on);
void deps_reset(void);
void deps_add(const char *path);
void deps_mark_volatile(void);
bool deps_is_volatile(void);
const char *const *deps_get(int *n);
void absent_reset(void);
#if defined(SLIMCC_LIBRARY) && !defined(SLIMCC_NO_ZONE)
# define malloc(n) zone_malloc(n)
# define calloc(n, sz) zone_calloc(n, sz)
# define realloc(p, n) zone_realloc(p, n)
# define free(p) zone_free(p)
# define strdup(s) zone_strdup(s)
# define strndup(s, n) zone_strndup(s, n)
#endif

// 小さな書式の処理（strings.c）。コンパイラの中の snprintf はこれ（picolibc の vfprintf を通さない）
int slimcc_vformat(char *buf, size_t size, const char *fmt, va_list ap, bool *ok);
int slimcc_snprintf(char *buf, size_t size, const char *fmt, ...);
#define snprintf slimcc_snprintf

// 中間表現の関数の出力の記録（cc/ir.inc）。内蔵アセンブラ（assembler.c）が、かけら（.slimcc_frag）として
// 文字列を経ずに読む（機械語を直接作る形。2026-11-16）
// 型付きの記録のオペランド（段階 1b）。ao_print が今までと同じ文字列にする
enum { AO_REG, AO_IMM, AO_FZERO, AO_SHIFT, AO_COND, AO_TEXT, AO_SYM, AO_MEM };
enum { AOM_OFF, AOM_IDX, AOM_IDXLSL, AOM_PRE, AOM_POST, AOM_GOTLO, AOM_OFF0 };   // AO_MEM の形（AOM_OFF0 は 0 でも #0 を書く）
enum { AS_PLAIN, AS_LO12, AS_GOT, AS_TPHI, AS_TPLO };            // AO_SYM の形（AS_TPHI・AS_TPLO は :tprel_hi12:・:tprel_lo12_nc:）
typedef struct {
  uint8_t kind;
  char c;         // AO_REG: 'x' 'w' 's' 'd'
  uint8_t r;      // AO_REG・AO_MEM の土台: 0〜30、31 は xzr・wzr、32 は sp
  uint8_t mode;   // AO_MEM・AO_SYM の形
  uint8_t idx;    // AO_MEM の添字（x のレジスタ）
  uint8_t sh;     // AO_MEM の lsl・AO_SHIFT の量
  int64_t imm;    // AO_IMM の値・AO_MEM のオフセット・AO_COND の条件
  const char *s;  // AO_TEXT（ラベル）・AO_SYM の名前・AO_SHIFT の名前
} AOp;

typedef struct {
  const char *s;  // IRR_LINE の行（改行なし）
  int len;
  uint8_t kind;   // IRR_*
  uint8_t nop;    // IRR_INSN のオペランドの数
  const char *mn; // IRR_INSN の命令の名前
  AOp op[4];
} IrRec;
enum { IRR_LINE, IRR_HOT_BEGIN, IRR_HOT_END, IRR_INSN };

typedef struct {
  IrRec *recs;
  int n;
} SlimccFrag;

#ifdef SLIMCC_LIBRARY
void *zone_heap_malloc(size_t n);
void zone_heap_free(void *p);
#else
#define zone_heap_malloc malloc
#define zone_heap_free free
#endif
extern TLS bool opt_direct;       // 中間表現の関数をかけらで内蔵アセンブラに渡す（同じプロセスで .o まで作るとき）
const SlimccFrag *slimcc_frag_get(int id);
int slimcc_frag_nrecs(void);
void slimcc_frag_free_all(void);
int slimcc_ao_print(char *b, const AOp *o);

#endif
