// picolibc などは、__need_wint_t を定義してから <stddef.h> を読み、wint_t を受け取る（GCC の stddef.h と同じ）
#if defined(__need_wint_t) && !defined(__SLIMCC_WINT_T) && defined(__WINT_TYPE__)
#define __SLIMCC_WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#undef __need_wint_t

#ifndef __STDDEF_H
#define __STDDEF_H

#if __STDC_VERSION__ >= 202311L
#define __STDC_VERSION_STDDEF_H__ 202311L

#define unreachable() __builtin_unreachable()

typedef typeof(nullptr) nullptr_t;
#endif

#define NULL ((void *)0)

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef __WCHAR_TYPE__ wchar_t;  // AArch64 では unsigned int（type.c の init_ty_lp64）
typedef struct {
  long long __ll;
  long double __ld;
} max_align_t;

#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
