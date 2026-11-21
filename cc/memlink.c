// メモリ上のリンカ: 内蔵アセンブラの結果（AsmObj）をメモリに配置し、リロケーションを解決する
//
// 出典: ELF for the Arm 64-bit Architecture（AAELF64）5.7 節（リロケーションの計算式）、
//       Procedure Call Standard for the Arm 64-bit Architecture（AAPCS64）6.4 節（IP0 と中継コード）
//
// - 配置するのは SHF_ALLOC のセクションだけ。TLS のセクション（アプリが定義する _Thread_local の変数）は
//   扱わない。外部の TLS の変数（errno など）への initial-exec の参照は、resolve に "tls:名前" で
//   TP からのオフセットを問い合わせ、GNU ld と同じく local-exec（movz・movk）に書き換える
// - コモンシンボルは、配置したメモリの最後に置く
// - 外部のシンボルは resolve で探す（Monocs では OS API 表、Linux では dlsym）
// - bl/b が届かない（±128MB を超える）ときは、x16 を使う 16 バイトの中継コードを置く
// - 定義していない変数は GOT 経由で参照される（slimcc_compile は PIE と同じ扱いでコンパイルする）。
//   GOT は配置したメモリの中に作るので、参照先がどれだけ離れていても届く

#define SLIMCC_NO_ZONE
#include "slimcc.h"
#include "libslimcc.h"

#define R_ABS64 257
#define R_ABS32 258
#define R_ABS16 259
#define R_PREL64 260
#define R_PREL32 261
#define R_PREL16 262
#define R_LD_PREL_LO19 273
#define R_ADR_PREL_LO21 274
#define R_ADR_PREL_PG_HI21 275
#define R_ADD_ABS_LO12_NC 277
#define R_LDST8_ABS_LO12_NC 278
#define R_TSTBR14 279
#define R_CONDBR19 280
#define R_JUMP26 282
#define R_CALL26 283
#define R_LDST16_ABS_LO12_NC 284
#define R_LDST32_ABS_LO12_NC 285
#define R_LDST64_ABS_LO12_NC 286
#define R_LDST128_ABS_LO12_NC 299
#define R_ADR_GOT_PAGE 311
#define R_LD64_GOT_LO12_NC 312
#define R_TLSIE_ADR_GOTTPREL_PAGE21 541
#define R_TLSIE_LD64_GOTTPREL_LO12_NC 542

#define STUB_SIZE 16

struct SlimccImage {
  uint8_t *mem;
  size_t size;
  SlimccFree *free_mem;
  HashMap syms; // 名前 → アドレス（定義したグローバル・ローカルのシンボル）
  struct SortedSym *sorted; // 番地の順（slimcc_symbol_at）
  int nsorted;
};

void __clear_cache(void *begin, void *end);

TLS static bool link_failed;

FMTCHK(1, 2)
static void link_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "link error: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  link_failed = true;
}

static uint32_t rd32(uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static void wr32(uint8_t *p, uint32_t v) {
  memcpy(p, &v, 4);
}

static void wr64(uint8_t *p, uint64_t v) {
  memcpy(p, &v, 8);
}

// 命令の即値の欄を書き換える（mask は欄の位置）
static void patch(uint8_t *p, uint32_t mask, uint32_t bits) {
  wr32(p, (rd32(p) & ~mask) | (bits & mask));
}

static bool fits_signed(int64_t v, int bits) {
  return v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1));
}

static void free_keys(HashMap *map) {
  for (int i = 0; i < map->capacity; i++)
    if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
      free((void *)map->buckets[i].key);
  free(map->buckets);
  *map = (HashMap){0};
}

// 複数のオブジェクトを 1 つのメモリに並べてつなぐ（GNU ld と同じく、STB_LOCAL のシンボルはそのオブジェクトの中だけ、
// グローバルなシンボルは全体で 1 つ。強い定義が 2 つあればエラー、弱い定義は強い定義に負ける。
// コモンシンボルは同じ名前で 1 つにまとめ、定義があれば定義を使う）
SlimccImage *slimcc_link_many(SlimccObj *const *sobjs, int nobj, SlimccResolver *resolve, void *ctx,
                              SlimccAlloc *alloc, SlimccFree *free_mem) {
  AsmObj *const *objs = (AsmObj *const *)sobjs;
  link_failed = false;

  // 1. 大きさを数える（セクション、コモンシンボル、中継コード、GOT）
  int64_t **sect_ofs = calloc(nobj, sizeof(int64_t *));
  int64_t size = 0;
  for (int k = 0; k < nobj; k++) {
    AsmObj *obj = objs[k];
    sect_ofs[k] = calloc(obj->nsect ? obj->nsect : 1, sizeof(int64_t));
    for (int i = 0; i < obj->nsect; i++) {
      AsmSection *sec = obj->sects[i];
      if (!(sec->flags & ASM_SHF_ALLOC) || !sec->size)
        continue;
      if (sec->flags & ASM_SHF_TLS) {
        link_error("thread-local variables are not supported");
        continue;
      }
      size = align_to(size, MAX(sec->align, 1));
      sect_ofs[k][i] = size;
      size += sec->size;
    }
  }

  // グローバルな定義（コモン以外）: 名前 → (オブジェクト, シンボル)
  HashMap gdef = {0};
  for (int k = 0; k < nobj; k++)
    for (int i = 0; i < objs[k]->nsym; i++) {
      AsmSymbol *s = objs[k]->syms[i];
      if (!s->defined || s->is_common || s->bind == ASM_STB_LOCAL)
        continue;
      AsmSymbol *prev = hashmap_get(&gdef, s->name);
      if (prev && prev->bind != ASM_STB_WEAK && s->bind != ASM_STB_WEAK) {
        link_error("multiple definition of '%s'", s->name);
        continue;
      }
      if (prev && s->bind == ASM_STB_WEAK)
        continue;
      hashmap_put(&gdef, s->name, s);
    }
  // コモンシンボル: 定義がなければ、同じ名前で 1 つ（大きさと境界は最大のもの）
  HashMap common = {0}; // 名前 → オフセット + 1
  for (int pass = 0; pass < 2; pass++) {
    HashMap csize = {0}, calign = {0};
    for (int k = 0; k < nobj; k++)
      for (int i = 0; i < objs[k]->nsym; i++) {
        AsmSymbol *s = objs[k]->syms[i];
        if (!s->is_common || hashmap_get(&gdef, s->name))
          continue;
        int64_t sz = (int64_t)(intptr_t)hashmap_get(&csize, s->name);
        int64_t al = (int64_t)(intptr_t)hashmap_get(&calign, s->name);
        hashmap_put(&csize, s->name, (void *)(intptr_t)MAX(sz, s->size));
        hashmap_put(&calign, s->name, (void *)(intptr_t)MAX(al, MAX(s->common_align, 1)));
      }
    if (pass == 1)
      break;
    for (int k = 0; k < nobj; k++)
      for (int i = 0; i < objs[k]->nsym; i++) {
        AsmSymbol *s = objs[k]->syms[i];
        if (!s->is_common || hashmap_get(&gdef, s->name) || hashmap_get(&common, s->name))
          continue;
        size = align_to(size, (int64_t)(intptr_t)hashmap_get(&calign, s->name));
        hashmap_put(&common, s->name, (void *)(intptr_t)(size + 1));
        size += (int64_t)(intptr_t)hashmap_get(&csize, s->name);
      }
    free(csize.buckets);
    free(calign.buckets);
  }

  size = align_to(size, 16);
  int64_t stub_base = size;
  int nbranch = 0, ngot = 0;
  for (int k = 0; k < nobj; k++)
    for (int i = 0; i < objs[k]->nsect; i++)
      for (int j = 0; j < objs[k]->sects[i]->nrel; j++) {
        int t = objs[k]->sects[i]->rels[j].type;
        if (t == R_CALL26 || t == R_JUMP26)
          nbranch++;
        if (t == R_ADR_GOT_PAGE)
          ngot++;
      }
  size += (int64_t)nbranch * STUB_SIZE;
  int64_t got_base = size;
  size += (int64_t)ngot * 8;

  uint8_t *mem = alloc(size ? size : 1);
  if (!mem) {
    fprintf(stderr, "link error: out of memory (%" PRIi64 " bytes)\n", size);
    for (int k = 0; k < nobj; k++)
      free(sect_ofs[k]);
    free(sect_ofs);
    free(gdef.buckets);
    free(common.buckets);
    return NULL;
  }
  memset(mem, 0, size);

  // 2. セクションの中身を置く
  for (int k = 0; k < nobj; k++)
    for (int i = 0; i < objs[k]->nsect; i++) {
      AsmSection *sec = objs[k]->sects[i];
      if (!(sec->flags & ASM_SHF_ALLOC) || !sec->size || (sec->flags & ASM_SHF_TLS))
        continue;
      if (sec->type != ASM_SHT_NOBITS)
        memcpy(mem + sect_ofs[k][i], sec->data, sec->size);
    }

  // 3. シンボルのアドレス: グローバル（img->syms）と、オブジェクトごとのローカル（locals[k]）
  SlimccImage *img = calloc(1, sizeof(SlimccImage));
  img->mem = mem;
  img->size = size;
  img->free_mem = free_mem;
  HashMap *locals = calloc(nobj, sizeof(HashMap));
  HashMap *sect_addr = calloc(nobj, sizeof(HashMap));
  for (int k = 0; k < nobj; k++)
    for (int i = 0; i < objs[k]->nsect; i++)
      hashmap_put(&sect_addr[k], objs[k]->sects[i]->name, mem + sect_ofs[k][i]);

  for (int k = 0; k < nobj; k++) {
    AsmObj *obj = objs[k];
    for (int i = 0; i < obj->nsym; i++) {
      AsmSymbol *s = obj->syms[i];
      void *addr;
      if (s->is_common) {
        if (hashmap_get(&gdef, s->name))
          continue;
        addr = mem + (int64_t)(intptr_t)hashmap_get(&common, s->name) - 1;
      } else if (s->defined && s->sect) {
        addr = mem + sect_ofs[k][s->sect->index] + s->value;
      } else if (s->defined) {
        addr = (void *)(intptr_t)s->value; // 絶対値（.set NAME, 定数）
      } else {
        continue;
      }
      if (s->bind == ASM_STB_LOCAL && !s->is_common)
        hashmap_put(&locals[k], s->name, addr);
      else if (s->is_common || hashmap_get(&gdef, s->name) == s)
        hashmap_put(&img->syms, strdup(s->name), addr); // obj を先に解放してもよいように複製する
    }
  }
  // slimcc_symbol はローカルなシンボルも引ける（同じ名前のグローバルがなければ。前からの振る舞い）
  for (int k = 0; k < nobj; k++)
    for (int i = 0; i < locals[k].capacity; i++) {
      HashEntry *e = &locals[k].buckets[i];
      if (e->key && e->key != TOMBSTONE && !hashmap_get2(&img->syms, e->key, e->keylen))
        hashmap_put2(&img->syms, strndup(e->key, e->keylen), e->keylen, e->val);
    }

  // 4. リロケーション
  HashMap stubs = {0};
  int nstub = 0;
  HashMap got = {0}; // 宛先のアドレス → GOT の項目
  int ngot_used = 0;
  for (int k = 0; k < nobj; k++) {
    AsmObj *obj = objs[k];
    for (int i = 0; i < obj->nsect; i++) {
      AsmSection *sec = obj->sects[i];
      if (!(sec->flags & ASM_SHF_ALLOC) || (sec->flags & ASM_SHF_TLS))
        continue;
      uint8_t *base = mem + sect_ofs[k][i];

      for (int j = 0; j < sec->nrel; j++) {
        AsmReloc *r = &sec->rels[j];
        uint8_t *P = base + r->offset;
        uint64_t S;

        // 外部の TLS の変数（initial-exec）:
        //   adrp xd, :gottprel:sym → movz xd, #(オフセット >> 16), lsl #16
        //   ldr xd, [xn, :gottprel_lo12:sym] → movk xd, #(オフセット & 0xFFFF)
        if (r->type == R_TLSIE_ADR_GOTTPREL_PAGE21 || r->type == R_TLSIE_LD64_GOTTPREL_LO12_NC) {
          if (r->is_section || hashmap_get(&locals[k], r->sym) || hashmap_get(&img->syms, r->sym)) {
            link_error("thread-local variables are not supported ('%s')", r->sym);
            continue;
          }
          char name[256];
          snprintf(name, sizeof(name), "tls:%s", r->sym);
          void *a = resolve ? resolve(name, ctx) : NULL;
          if (!a) {
            link_error("undefined thread-local symbol '%s'", r->sym);
            continue;
          }
          uint64_t ofs = (uint64_t)(uintptr_t)a + r->addend;
          if (ofs > UINT32_MAX) {
            link_error("thread-local offset out of range for '%s'", r->sym);
            continue;
          }
          uint32_t reg = (uint32_t)(P[0] | P[1] << 8 | P[2] << 16 | P[3] << 24) & 0x1F;
          if (r->type == R_TLSIE_ADR_GOTTPREL_PAGE21)
            wr32(P, 0xD2A00000 | (uint32_t)((ofs >> 16) & 0xFFFF) << 5 | reg);
          else
            wr32(P, 0xF2800000 | (uint32_t)(ofs & 0xFFFF) << 5 | reg);
          continue;
        }

        if (r->is_section) {
          S = (uint64_t)(uintptr_t)hashmap_get(&sect_addr[k], r->sym);
        } else {
          void *a = hashmap_get(&locals[k], r->sym);
          if (!a)
            a = hashmap_get(&img->syms, r->sym);
          if (!a) {
            // 定義されていない（外部の）シンボル
            a = resolve ? resolve(r->sym, ctx) : NULL;
            if (!a) {
              AsmSymbol *s = NULL;
              for (int m = 0; m < obj->nsym; m++)
                if (!strcmp(obj->syms[m]->name, r->sym))
                  s = obj->syms[m];
              if (!(s && s->bind == ASM_STB_WEAK)) {
                link_error("undefined symbol '%s'", r->sym);
                continue;
              }
            }
          }
          S = (uint64_t)(uintptr_t)a;
        }
        uint64_t X = S + r->addend;
        int64_t pcrel = (int64_t)(X - (uint64_t)(uintptr_t)P);

        // GOT の項目（同じ宛先には 1 つだけ）
        uint8_t *G = NULL;
        if (r->type == R_ADR_GOT_PAGE || r->type == R_LD64_GOT_LO12_NC) {
          char key[48];
          snprintf(key, sizeof(key), "%" PRIx64, X);
          G = hashmap_get(&got, key);
          if (!G) {
            if (ngot_used == ngot) {
              link_error("too many GOT entries");
              continue;
            }
            G = mem + got_base + (int64_t)ngot_used++ * 8;
            wr64(G, X);
            hashmap_put(&got, strdup(key), G);
          }
        }

        switch (r->type) {
        case R_ADR_GOT_PAGE: {
          int64_t d = (int64_t)(((uint64_t)(uintptr_t)G & ~0xFFFULL) -
                                ((uint64_t)(uintptr_t)P & ~0xFFFULL)) >> 12;
          patch(P, (3u << 29) | (0x7FFFF << 5),
                ((uint32_t)(d & 3) << 29) | ((uint32_t)((d >> 2) & 0x7FFFF) << 5));
          break;
        }
        case R_LD64_GOT_LO12_NC:
          patch(P, 0xFFF << 10, (uint32_t)(((uintptr_t)G & 0xFFF) >> 3) << 10);
          break;
        case R_ABS64:
          wr64(P, X);
          break;
        case R_ABS32:
          if ((int64_t)X != (int32_t)X && X != (uint32_t)X)
            link_error("R_AARCH64_ABS32 out of range for '%s'", r->sym);
          wr32(P, (uint32_t)X);
          break;
        case R_ABS16:
          P[0] = X, P[1] = X >> 8;
          break;
        case R_PREL64:
          wr64(P, pcrel);
          break;
        case R_PREL32:
          if (!fits_signed(pcrel, 32))
            link_error("R_AARCH64_PREL32 out of range for '%s'", r->sym);
          wr32(P, (uint32_t)pcrel);
          break;
        case R_CALL26:
        case R_JUMP26:
          if (pcrel & 3 || !fits_signed(pcrel, 28)) {
            // 中継コード: ldr x16, #8; br x16; .xword X（同じ宛先には 1 つだけ）
            char key[48];
            snprintf(key, sizeof(key), "%" PRIx64, X);
            uint8_t *stub = hashmap_get(&stubs, key);
            if (!stub) {
              stub = mem + stub_base + (int64_t)nstub++ * STUB_SIZE;
              wr32(stub, 0x58000050);     // ldr x16, #8
              wr32(stub + 4, 0xD61F0200); // br x16
              wr64(stub + 8, X);
              hashmap_put(&stubs, strdup(key), stub);
            }
            pcrel = stub - P;
          }
          patch(P, 0x3FFFFFF, (uint32_t)(pcrel >> 2));
          break;
        case R_CONDBR19:
        case R_LD_PREL_LO19:
          if (pcrel & 3 || !fits_signed(pcrel, 21))
            link_error("branch out of range for '%s'", r->sym);
          patch(P, 0x7FFFF << 5, (uint32_t)(pcrel >> 2) << 5);
          break;
        case R_TSTBR14:
          if (pcrel & 3 || !fits_signed(pcrel, 16))
            link_error("test-and-branch out of range for '%s'", r->sym);
          patch(P, 0x3FFF << 5, (uint32_t)(pcrel >> 2) << 5);
          break;
        case R_ADR_PREL_LO21:
          if (!fits_signed(pcrel, 21))
            link_error("adr out of range for '%s'", r->sym);
          patch(P, (3u << 29) | (0x7FFFF << 5), ((uint32_t)(pcrel & 3) << 29) |
                                                     ((uint32_t)((pcrel >> 2) & 0x7FFFF) << 5));
          break;
        case R_ADR_PREL_PG_HI21: {
          int64_t d = (int64_t)((X & ~0xFFFULL) - ((uint64_t)(uintptr_t)P & ~0xFFFULL)) >> 12;
          if (!fits_signed(d, 21))
            link_error("adrp out of range for '%s'", r->sym);
          patch(P, (3u << 29) | (0x7FFFF << 5),
                ((uint32_t)(d & 3) << 29) | ((uint32_t)((d >> 2) & 0x7FFFF) << 5));
          break;
        }
        case R_ADD_ABS_LO12_NC:
          patch(P, 0xFFF << 10, (uint32_t)(X & 0xFFF) << 10);
          break;
        case R_LDST8_ABS_LO12_NC:
        case R_LDST16_ABS_LO12_NC:
        case R_LDST32_ABS_LO12_NC:
        case R_LDST64_ABS_LO12_NC:
        case R_LDST128_ABS_LO12_NC: {
          int shift = r->type == R_LDST8_ABS_LO12_NC ? 0 : r->type == R_LDST16_ABS_LO12_NC ? 1
                    : r->type == R_LDST32_ABS_LO12_NC ? 2 : r->type == R_LDST64_ABS_LO12_NC ? 3 : 4;
          if (X & ((1u << shift) - 1))
            link_error("misaligned address for '%s'", r->sym);
          patch(P, 0xFFF << 10, (uint32_t)((X & 0xFFF) >> shift) << 10);
          break;
        }
        default:
          link_error("unsupported relocation type %d for '%s'", r->type, r->sym);
          break;
        }
      }
    }
  }

  free_keys(&stubs);
  free_keys(&got);
  for (int k = 0; k < nobj; k++) {
    free(sect_addr[k].buckets);
    free(locals[k].buckets);
    free(sect_ofs[k]);
  }
  free(sect_addr);
  free(locals);
  free(sect_ofs);
  free(gdef.buckets);
  free(common.buckets);

  if (link_failed) {
    slimcc_free_image(img);
    return NULL;
  }

  // 命令キャッシュとの整合を取る
  __clear_cache(mem, mem + size);
  return img;
}

SlimccImage *slimcc_link(SlimccObj *sobj, SlimccResolver *resolve, void *ctx,
                         SlimccAlloc *alloc, SlimccFree *free_mem) {
  return slimcc_link_many(&sobj, 1, resolve, ctx, alloc, free_mem);
}

void *slimcc_symbol(SlimccImage *img, const char *name) {
  return hashmap_get(&img->syms, name);
}

// 番地の順に並べたシンボル（slimcc_symbol_at が初めて呼ばれたときに作る）
typedef struct SortedSym {
  const uint8_t *addr;
  const char *name;
} SortedSym;

static int cmp_sorted(const void *a, const void *b) {
  const SortedSym *x = a, *y = b;
  if (x->addr != y->addr)
    return x->addr < y->addr ? -1 : 1;
  return strcmp(x->name, y->name); // 同じ番地なら名前の順（結果をいつも同じにする）
}

// addr を含む（addr 以下でいちばん近い）シンボルの名前。addr が img の外なら NULL。off に addr との差
const char *slimcc_symbol_at(SlimccImage *img, const void *addr, size_t *off) {
  const uint8_t *a = addr;
  if (a < img->mem || a >= img->mem + img->size)
    return NULL;
  if (!img->sorted) {
    img->sorted = calloc(img->syms.used + 1, sizeof(SortedSym));
    if (!img->sorted)
      return NULL;
    for (int i = 0; i < img->syms.capacity; i++) {
      HashEntry *e = &img->syms.buckets[i];
      // .L で始まるもの（コンパイラが作る分岐先・定数のラベル）は関数ではないので除く
      if (e->key && e->key != TOMBSTONE && strncmp(e->key, ".L", 2))
        img->sorted[img->nsorted++] = (SortedSym){e->val, e->key};
    }
    qsort(img->sorted, img->nsorted, sizeof(SortedSym), cmp_sorted);
  }
  int lo = 0, hi = img->nsorted - 1, best = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (img->sorted[mid].addr <= a) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (best < 0)
    return NULL;
  // 同じ番地の名前が並ぶときは、その最初（名前の順で先）
  while (best > 0 && img->sorted[best - 1].addr == img->sorted[best].addr)
    best--;
  if (off)
    *off = (size_t)(a - img->sorted[best].addr);
  return img->sorted[best].name;
}

void slimcc_image_range(SlimccImage *img, void **mem, size_t *size) {
  *mem = img->mem;
  *size = img->size;
}

void slimcc_free_image(SlimccImage *img) {
  if (!img)
    return;
  if (img->free_mem)
    img->free_mem(img->mem, img->size);
  for (int i = 0; i < img->syms.capacity; i++)
    if (img->syms.buckets[i].key && img->syms.buckets[i].key != TOMBSTONE)
      free((void *)img->syms.buckets[i].key);
  free(img->syms.buckets);
  free(img->sorted);
  free(img);
}
