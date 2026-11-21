// 文字列とハッシュ表（コンパイラに近い処理）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Ent { char *key; int val; struct Ent *next; } Ent;
static Ent *tab[4096];
static unsigned hash(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
int main(void) {
  char buf[32];
  long total = 0;
  for (int rep = 0; rep < 20; rep++) {
    for (int i = 0; i < 100000; i++) {
      snprintf(buf, sizeof(buf), "key%d", i % 50000);
      unsigned h = hash(buf) & 4095;
      Ent *e = tab[h];
      while (e && strcmp(e->key, buf)) e = e->next;
      if (!e) { e = malloc(sizeof(Ent)); e->key = strdup(buf); e->val = 0; e->next = tab[h]; tab[h] = e; }
      e->val++;
      total += e->val;
    }
  }
  printf("%ld\n", total);
  return 0;
}
