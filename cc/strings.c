#include "slimcc.h"

void *grow_array(void *data, size_t sz, int32_t *cap) {
  int64_t newcap = (int64_t)*cap * 2;
  if (newcap <= INT32_MAX) {
    if (newcap == 0)
      newcap = 8;
    void *p = realloc(data, sz * newcap);
    if (p) {
      *cap = newcap;
      return p;
    }
  }
  internal_error();
}

void strarray_push(StringArray *arr, const char *s) {
  if (arr->capacity == arr->len)
    GrowArr(arr->data, &arr->capacity);

  arr->data[arr->len++] = s;
}

// Takes a printf-style format string and returns a formatted string.
char *format(const char *fmt, ...) {
  char *buf;
  size_t buflen;
  FILE *out = open_memstream(&buf, &buflen);

  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);
  fclose(out);
#ifdef SLIMCC_LIBRARY
  // open_memstream のバッファは通常のヒープにあるので、ゾーンに移す（zone.c）
  char *s = strdup(buf);
  free(buf);
  return s;
#else
  return buf;
#endif
}

// 小さな書式の処理（2026-11-15）: %s・%c・%d・%i・%u・%x とその l・ll（PRIi64 など）・%% だけを 1 回でたどって
// buf に書く（入りきらない分は切り、NUL で終え、本来の長さを返す）。ほかの書式に当たったら *ok を偽にする
// （呼び出し元が vsnprintf でやり直す）。picolibc の vfprintf は 1 文字ずつ書くので、実機でコンパイラの
// 出力（アセンブリの文字列・番地・ラベル）を作るのに時間を使っていた
int slimcc_vformat(char *buf, size_t size, const char *fmt, va_list ap, bool *ok) {
  size_t n = 0;
  *ok = true;
#define VF_PUT(ch)          \
  do {                      \
    if (n + 1 < size)       \
      buf[n] = (ch);        \
    n++;                    \
  } while (0)
// 文字の並びをまとめて写す（入りきらない分は切る）
#define VF_PUTN(src, len)                              \
  do {                                                 \
    size_t l_ = (len);                                 \
    if (n + 1 < size) {                                \
      size_t room = size - 1 - n;                      \
      memcpy(buf + n, (src), l_ < room ? l_ : room);   \
    }                                                  \
    n += l_;                                           \
  } while (0)
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      const char *q = p;
      while (*q && *q != '%')
        q++;
      VF_PUTN(p, (size_t)(q - p));
      p = q - 1;
      continue;
    }
    p++;
    if (*p == '%') {
      VF_PUT('%');
      continue;
    }
    int lng = 0;
    while (*p == 'l' && lng < 2)
      p++, lng++;
    char c = *p;
    if (c == 's') {
      const char *t = va_arg(ap, const char *);
      VF_PUTN(t, strlen(t));
      continue;
    }
    if (c == 'c' && !lng) {
      VF_PUT((char)va_arg(ap, int));
      continue;
    }
    if (c != 'd' && c != 'i' && c != 'u' && c != 'x') {
      *ok = false;
      return -1;
    }
    unsigned long long u;
    bool neg = false;
    if (c == 'd' || c == 'i') {
      long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
      neg = v < 0;
      u = neg ? -(unsigned long long)v : (unsigned long long)v;
    } else {
      u = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
    }
    char digits[24];
    int k = 0;
    unsigned base = c == 'x' ? 16 : 10;
    do {
      digits[k++] = "0123456789abcdef"[u % base];
      u /= base;
    } while (u);
    if (neg)
      VF_PUT('-');
    while (k)
      VF_PUT(digits[--k]);
  }
#undef VF_PUT
#undef VF_PUTN
  if (size)
    buf[n < size ? n : size - 1] = '\0';
  return (int)n;
}

#undef snprintf
// snprintf と同じ。書式が slimcc_vformat の中なら自分で作る（slimcc.h が snprintf をこれにする）
int slimcc_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  bool ok;
  int r = slimcc_vformat(buf, size, fmt, ap, &ok);
  if (!ok)
    r = vsnprintf(buf, size, fmt, ap2);
  va_end(ap2);
  va_end(ap);
  return r;
}
