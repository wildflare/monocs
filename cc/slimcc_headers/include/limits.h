#ifndef _SLIMCC_LIMITS_H
#define _SLIMCC_LIMITS_H

#ifdef __MONOCS__
// Monocs（picolibc）: picolibc の limits.h は、__GNUC__ があると基本の上限値（CHAR_BIT、INT_MAX など）を
// コンパイラ付属の limits.h に任せる（#include_next）。GCC の limits.h と同じものをここで定義し、
// 読み込み済みの印 _GCC_LIMITS_H_ を立てる。値は slimcc が予定義する __INT_MAX__ などから作る
#define _GCC_LIMITS_H_

#define CHAR_BIT __CHAR_BIT__
#define SCHAR_MIN (-SCHAR_MAX - 1)
#define SCHAR_MAX __SCHAR_MAX__
#define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif
#define SHRT_MIN (-SHRT_MAX - 1)
#define SHRT_MAX __SHRT_MAX__
#define USHRT_MAX (SHRT_MAX * 2 + 1)
#define INT_MIN (-INT_MAX - 1)
#define INT_MAX __INT_MAX__
#define UINT_MAX (INT_MAX * 2U + 1U)
#define LONG_MIN (-LONG_MAX - 1L)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define LLONG_MAX __LONG_LONG_MAX__
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
// POSIX の SSIZE_MAX（picolibc の limits.h は ((__ssize_t)(__SIZE_MAX__ >> 1)) で、#if に書けない。glibc と同じく
// 数にする。picolibc は #ifndef SSIZE_MAX で、こちらが先。2026-11-20、vim の fileio.c）
#define SSIZE_MAX __LONG_MAX__

#if __STDC_VERSION__ > 201710L
#define CHAR_WIDTH __SCHAR_WIDTH__
#define SCHAR_WIDTH __SCHAR_WIDTH__
#define UCHAR_WIDTH __SCHAR_WIDTH__
#define SHRT_WIDTH __SHRT_WIDTH__
#define USHRT_WIDTH __SHRT_WIDTH__
#define INT_WIDTH __INT_WIDTH__
#define UINT_WIDTH __INT_WIDTH__
#define LONG_WIDTH __LONG_WIDTH__
#define ULONG_WIDTH __LONG_WIDTH__
#define LLONG_WIDTH __LONG_LONG_WIDTH__
#define ULLONG_WIDTH __LONG_LONG_WIDTH__
#define BOOL_MAX 1
#define BOOL_WIDTH 1
#endif
#endif

// Monocs では、picolibc の limits.h から読まれたとき（picolibc 自身のビルド）は先がない
#if !defined(__MONOCS__) || __has_include_next(<limits.h>)
#include_next <limits.h>
#endif

#if __STDC_VERSION__ >= 202311L && !defined(BITINT_MAXWIDTH)
#define BITINT_MAXWIDTH __BITINT_MAXWIDTH__
#endif

#endif
