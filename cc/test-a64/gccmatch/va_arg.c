// va_arg・va_copy（ir.inc の ir_va_arg）が GCC と同じ結果か（run.sh の 8c）
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef struct { int a, b; } Small;
typedef struct { long a, b, c; } Big;
typedef struct { char s[3]; } Odd;
static long take(const char *fmt, va_list ap) {
  long h = 0;
  for (const char *p = fmt; *p; p++) {
    switch (*p) {
    case 'i': h = h * 31 + va_arg(ap, int); break;
    case 'l': h = h * 31 + va_arg(ap, long); break;
    case 'd': h = h * 31 + (long)(va_arg(ap, double) * 100); break;
    case 'f': h = h * 31 + (long)(va_arg(ap, double) * 10); break;
    case 's': { Small s = va_arg(ap, Small); h = h * 31 + s.a * 7 + s.b; break; }
    case 'b': { Big b = va_arg(ap, Big); h = h * 31 + b.a + b.b * 3 + b.c * 5; break; }
    case 'o': { Odd o = va_arg(ap, Odd); h = h * 31 + o.s[0] + o.s[2]; break; }
    case 'p': h = h * 31 + (long)strlen(va_arg(ap, char *)); break;
    }
  }
  return h;
}
static long vtwice(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  long x = take(fmt, ap);
  long y = take(fmt, ap2);
  va_end(ap2);
  return x * 1000003 + y;
}
static long f(const char *fmt, ...) { va_list ap; va_start(ap, fmt); long r = vtwice(fmt, ap); va_end(ap); return r; }
int main(void) {
  Small s = {3, 4}; Big b = {5, 6, 7}; Odd o = {{'a', 'b', 'c'}};
  printf("%ld\n", f("i", 1));
  printf("%ld\n", f("iiiiiiiiiiii", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
  printf("%ld\n", f("dddddddddddd", 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5, 11.5, 12.5));
  printf("%ld\n", f("idlfsbopisbdl", 1, 2.25, 3L, 4.75, s, b, o, "hello", 9, s, b, 6.5, 77L));
  printf("%ld\n", f("bbbbbbbbbb", b, b, b, b, b, b, b, b, b, b));
  printf("%ld\n", f("ssssssssssss", s, s, s, s, s, s, s, s, s, s, s, s));
  char buf[200]; snprintf(buf, sizeof buf, "%d %s %.3f %ld %x %c", 42, "str", 3.14159, 1234567890123L, 255, 'z');
  printf("%s\n", buf);
  return 0;
}
