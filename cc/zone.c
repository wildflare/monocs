// コンパイル用のメモリ領域（ゾーン）。libslimcc.c が使う
//
// slimcc は「プロセスが終われば OS がメモリを回収する」前提で書かれていて、
// 解放しないメモリが多い。Monocs では同じカーネルの中で何度もコンパイルするので、
// コンパイル（前処理・構文解析・コード生成）の間の確保をすべてこのゾーンから行い、
// 終わったらまとめて捨てる。
// slimcc.h が SLIMCC_LIBRARY のとき、malloc などを zone_malloc などに置き換える。
// ゾーンが止まっているとき（内蔵アセンブラ、メモリ上のリンカ）は、通常のヒープを使う。

#define SLIMCC_NO_ZONE
#include "slimcc.h"

#define CHUNK_SIZE (1 << 20)
#define ALIGN 16

typedef struct Chunk Chunk;
struct Chunk {
  Chunk *next;
  size_t size;
  size_t used;
  _Alignas(16) char data[];
};

typedef struct {
  size_t size;
  size_t pad;
} Header;

TLS static Chunk *chunks;
TLS static Chunk *last_hit;
TLS static bool active;
TLS static size_t total;

void zone_begin(void) {
  active = true;
}

// ゾーンを止め、確保したものをすべて捨てる
void zone_end(void) {
  active = false;
  while (chunks) {
    Chunk *next = chunks->next;
    free(chunks);
    chunks = next;
  }
  last_hit = NULL;
  total = 0;
}

size_t zone_total(void) {
  return total;
}

static bool in_zone(const void *p) {
  const char *c = p;
  if (last_hit && c >= last_hit->data && c < last_hit->data + last_hit->size)
    return true;
  for (Chunk *ch = chunks; ch; ch = ch->next) {
    if (c >= ch->data && c < ch->data + ch->size) {
      last_hit = ch;
      return true;
    }
  }
  return false;
}

void *zone_malloc(size_t n) {
  if (!active)
    return malloc(n);
  size_t need = sizeof(Header) + ((n + ALIGN - 1) & ~(size_t)(ALIGN - 1));
  if (!chunks || chunks->used + need > chunks->size) {
    size_t sz = need > CHUNK_SIZE / 4 ? need : CHUNK_SIZE;
    Chunk *ch = malloc(sizeof(Chunk) + sz);
    if (!ch) {
      // ヒープが尽きた: このコンパイルを誤りで終える（NULL を返すと、呼び出し元はほとんど確かめずに書き込み、
      // カーネルのコードの領域や NULL に書いて止まった。2026-11-20、vim を 3 コアでコンパイルしたとき）。
      // ゾーンを止めてから（error が使う確保は通常のヒープから。足りなければそれまで）
      active = false;
      error("out of memory (%zu KB used by this compilation)", total / 1024);
    }
    ch->size = sz;
    ch->used = 0;
    if (need > CHUNK_SIZE / 4 && chunks) {
      // 大きいものは専用のチャンクにし、今のチャンクの残りは使い続ける
      ch->next = chunks->next;
      chunks->next = ch;
    } else {
      ch->next = chunks;
      chunks = ch;
    }
    Header *h = (Header *)(ch->data + ch->used);
    ch->used += need;
    h->size = n;
    total += need;
    return h + 1;
  }
  Header *h = (Header *)(chunks->data + chunks->used);
  chunks->used += need;
  h->size = n;
  total += need;
  return h + 1;
}

// ゾーンの外（通常のヒープ）に確保する（コンパイルをまたいで持つもの。tokenize.c のファイルの覚え）
void *heap_malloc(size_t n) {
  return malloc(n);
}

void heap_free(void *p) {
  free(p);
}

void *zone_calloc(size_t n, size_t sz) {
  void *p = zone_malloc(n * sz);
  if (p)
    memset(p, 0, n * sz);
  return p;
}

void zone_free(void *p) {
  if (!p || in_zone(p))
    return;
  free(p);
}

void *zone_realloc(void *p, size_t n) {
  if (!p)
    return zone_malloc(n);
  if (!in_zone(p))
    return realloc(p, n);
  Header *h = (Header *)p - 1;
  if (n <= h->size)
    return p;
  void *q = zone_malloc(n);
  if (q)
    memcpy(q, p, h->size);
  return q;
}

char *zone_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = zone_malloc(n);
  return p ? memcpy(p, s, n) : NULL;
}

char *zone_strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *p = zone_malloc(len + 1);
  if (!p)
    return NULL;
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

// ゾーンの外（ふつうのヒープ）から確保する。コンパイルの終わりにゾーンを捨てた後も使うもの（かけら）
void *zone_heap_malloc(size_t n) {
  return malloc(n);
}

void zone_heap_free(void *p) {
  free(p);
}
