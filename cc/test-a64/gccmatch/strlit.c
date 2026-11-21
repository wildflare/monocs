// 同じ文字列リテラルを 1 つにまとめる（parse.c の string_literal_var）。GCC と同じく、同じ関数の中・
// 関数の外どうしの同じ文字列は同じ番地になる（run.sh の 8c で GCC と比べる）
#include <stdio.h>
#include <string.h>

static const char *g1 = "hello";
static const char *g2 = "hello";

static int span(const char *b, const char *e) { return (int)(e - b); }

__attribute__((noinline)) int same_fn(void) {
  // 同じ文字列を 2 回書いて範囲にする（Monocs のシェルで使っていた形）
  return span("HOME", "HOME" + 4) * 10 + ("ab" == "ab");
}

__attribute__((noinline)) int types(void) {
  // 中身が同じでも型が違うものは別（char と wchar_t）
  const void *a = "x";
  const void *b = L"x";
  return a != b;
}

int main(void) {
  printf("globals %d\n", g1 == g2);
  printf("same_fn %d\n", same_fn());
  printf("types %d\n", types());
  char buf[] = "hello";
  buf[0] = 'j';
  printf("array %s %s\n", buf, g1);
  printf("len %zu %d\n", strlen("HOME" + 1), (int)sizeof("HOME"));
  return 0;
}
