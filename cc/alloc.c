#include "slimcc.h"

#define FREE_THRESHOLD (100 * 1024)
#define ARENA_POOL_SIZE 8160

// 塊: 頭に次への鎖、後ろに中身（ふつうは ARENA_POOL_SIZE。それより大きい 1 つの確保は、その大きさの塊を
// 別に取る: big_pool。2026-11-20、QuickJS の intl/locale-data.c の 13,709 文字の文字列リテラルで内部エラーだった）
struct Pool {
  Pool *next;
  char buf[];
};

TLS Arena ast_arena;
TLS Arena cc1_arena;
TLS Arena pp_arena;
TLS bool free_alloc = EAGER_FREE;

TLS static Pool *pool_freelist;

#if !EAGER_FREE
bool check_mem_usage(void) {
#if defined(SLIMCC_LIBRARY)
  return false; // ゾーンでまとめて捨てるので、逐次の解放はしない
#else
  struct rusage stat;
  getrusage(RUSAGE_SELF, &stat);
  return stat.ru_maxrss > FREE_THRESHOLD;
#endif
}
#endif

// ゾーンを捨てる前に呼ぶ（プールはゾーンの中にあるため）
void alloc_reset(void) {
  pool_freelist = NULL;
  ast_arena = cc1_arena = pp_arena = (Arena){0};
}

static Pool *new_pool(void) {
  Pool *p;
  if (pool_freelist) {
    p = pool_freelist;
    pool_freelist = p->next;
  } else {
    p = malloc(sizeof(Pool) + ARENA_POOL_SIZE);
  }

#if defined(USE_ASAN)
  __asan_poison_memory_region(&p->buf, ARENA_POOL_SIZE);
#endif
  p->next = NULL;
  return p;
}

// ARENA_POOL_SIZE より大きい 1 つの確保: その大きさの塊を取り、鎖の頭に入れる（今の塊はそのまま使い続ける。
// 解放・使い回しはほかの塊と同じ。使い回すときは、ふつうの塊として頭の ARENA_POOL_SIZE だけを使う）
static void *big_pool(Arena *arena, size_t aligned_sz) {
  Pool *p = malloc(sizeof(Pool) + aligned_sz);
  if (!p)
    error("out of memory");
  p->next = arena->head;
  arena->head = p;
  return p->buf;
}

static void *allocate(Arena *arena, size_t sz, bool clear) {
  size_t aligned_sz = (sz + 15) & -16LL;

  void *ptr;
  if ((arena->used + aligned_sz) <= ARENA_POOL_SIZE) {
    ptr = &arena->cur->buf[arena->used];
    arena->used += aligned_sz;
  } else if (aligned_sz > ARENA_POOL_SIZE) {
    ptr = big_pool(arena, aligned_sz);
  } else {
    arena->cur = arena->cur->next = new_pool();
    ptr = &arena->cur->buf;
    arena->used = aligned_sz;
  }

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, aligned_sz);
#endif

#if defined(USE_TYSAN)
  memset(ptr, 0, aligned_sz);
#else
  if (clear)
    memset(ptr, 0, aligned_sz);
#endif
  return ptr;
}

char *arena_format(Arena *arena, const char *fmt, ...) {
  void *ptr = &arena->cur->buf[arena->used];
  size_t n = ARENA_POOL_SIZE - arena->used;

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, n);
#endif

  va_list ap;
  va_start(ap, fmt);

  size_t sz = 1 + (size_t)vsnprintf(ptr, n, fmt, ap);
  size_t aligned_sz = (sz + 15) & -16LL;

  if (aligned_sz <= n) {
    va_end(ap);
    arena->used += aligned_sz;
#if defined(USE_ASAN)
    __asan_poison_memory_region(ptr + aligned_sz, ARENA_POOL_SIZE - arena->used);
#endif
    return ptr;
  }
  va_end(ap);
  if (aligned_sz > ARENA_POOL_SIZE) {
    ptr = big_pool(arena, aligned_sz);
  } else {
    arena->cur = arena->cur->next = new_pool();
    ptr = &arena->cur->buf;
    arena->used = aligned_sz;
  }

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, aligned_sz);
#endif

  va_start(ap, fmt);
  vsnprintf(ptr, aligned_sz, fmt, ap);
  va_end(ap);
  return ptr;
}

char *arena_strdup(Arena *arena, const char *str) {
  size_t len = 1 + strlen(str);
  return memcpy(arena_malloc(arena, len), str, len);
}

char *arena_copy_string(Arena *arena, const char *src, size_t slen) {
  char *p = memcpy(arena_malloc(arena, slen + 1), src, slen);
  p[slen] = '\0';
  return p;
}

void arena_on(Arena *arena) {
  arena->head = arena->cur = new_pool();
  arena->used = 0;
}

// 真なら、次の arena_off で ast_arena を解放しない（インライン展開する関数の AST を、後の関数で使うため。
// codegen.c の ir.inc が設定する。解放しなかったメモリは、ライブラリではゾーンと一緒に捨てる）
TLS bool ast_arena_keep;

void arena_off(Arena *arena) {
  if (arena == &ast_arena && ast_arena_keep) {
    ast_arena_keep = false;
    arena->cur = NULL;
    return;
  }
  if (free_alloc) {
    for (Pool *p = arena->head; p;) {
      Pool *tmp = p;
      p = p->next;
      free(tmp);
    }
  } else {
    arena->cur->next = pool_freelist;
    pool_freelist = arena->head;
  }
  arena->cur = NULL;
}

void *arena_malloc(Arena *a, size_t sz) {
  return allocate(a, sz, false);
}

void *arena_calloc(Arena *a, size_t sz) {
  return allocate(a, sz, true);
}
