// 型による別名の判定（ir.inc の ir_tbaa・ir_may_alias）とループの外への読み出しの移動（ir_licm_load）で、
// C の規則で許される別名（char での読み書き、符号だけ違う整数、共用体の読み替え、同じ型どうし）を
// 正しく扱うこと。GCC（-O2）と同じ結果になる（run.sh の 8c）
#include <stdio.h>
#include <string.h>

struct node { int val; struct node *next; int count; };
static struct node nodes[8];
static struct node **table;
static int ntable;
static long total;

__attribute__((noinline)) void count_all(void) {
  for (int i = 0; i < ntable; i++)
    for (struct node *n = table[i]; n; n = n->next)
      n->count += n->val;   // int の書き込み: table（ポインタ）・ntable（int）の読み出し
}

// ntable と同じ int を書く: ntable を読み直さなければならない
__attribute__((noinline)) int shrink(int *p) {
  int k = 0;
  for (int i = 0; i < ntable; i++) {
    k++;
    *p = *p - 1;
  }
  return k;
}

// unsigned で書いて int で読む（符号だけ違う整数は重なる）
__attribute__((noinline)) int signed_alias(int *a, unsigned *b, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += *a;
    *b += 3;
  }
  return s;
}

// char で書いて long で読む（char は何とでも重なる）
__attribute__((noinline)) long char_alias(long *x, char *c, int n) {
  long s = 0;
  for (int i = 0; i < n; i++) {
    s += *x;
    c[i & 7] += 1;
  }
  return s;
}

// 共用体の読み替え
union pun { float f; unsigned u; int i; };
__attribute__((noinline)) unsigned punning(union pun *p, int n) {
  unsigned s = 0;
  for (int i = 0; i < n; i++) {
    p->f = (float)i + 0.5f;
    s += p->u;
    p->i = i;
    s += p->u;
  }
  return s;
}

// 同じ型のポインタどうし（重なるかもしれない）
__attribute__((noinline)) long same_type(long *a, long *b, int n) {
  long s = 0;
  for (int i = 0; i < n; i++) {
    s += *a;
    *b = *b + i;
  }
  return s;
}

// memcpy での書き込み（読み出しを外に出さない）
__attribute__((noinline)) double via_memcpy(double *d, int n) {
  double s = 0;
  for (int i = 0; i < n; i++) {
    s += *d;
    double v = i * 0.25;
    memcpy(d, &v, sizeof v);
  }
  return s;
}

// ループの中の条件の中だけで読む（外に出すと NULL を読む）
__attribute__((noinline)) int guarded(struct node *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    if (p)
      s += p->val;
  return s;
}

int main(void) {
  struct node *ptrs[4];
  for (int i = 0; i < 8; i++) {
    nodes[i].val = i + 1;
    nodes[i].next = i % 2 == 0 ? &nodes[i + 1] : NULL;
  }
  for (int i = 0; i < 4; i++)
    ptrs[i] = &nodes[i * 2];
  table = ptrs;
  ntable = 4;
  count_all();
  count_all();
  for (int i = 0; i < 8; i++)
    total += nodes[i].count * (i + 1);
  printf("count_all %ld\n", total);
  ntable = 10;
  printf("shrink %d %d\n", shrink(&ntable), ntable);
  int v = 5;
  printf("signed_alias %d\n", signed_alias(&v, (unsigned *)&v, 10));
  long x = 0;
  printf("char_alias %ld\n", char_alias(&x, (char *)&x, 20));
  union pun u;
  printf("punning %u\n", punning(&u, 10));
  long arr[2] = {1, 2};
  printf("same_type %ld %ld\n", same_type(&arr[0], &arr[0], 10), same_type(&arr[0], &arr[1], 10));
  double d = 1.0;
  printf("via_memcpy %.2f\n", via_memcpy(&d, 10));
  printf("guarded %d %d\n", guarded(NULL, 5), guarded(&nodes[2], 5));
  return 0;
}
