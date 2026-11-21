// layout.c で使う型。x86-64 と AArch64 で違いが出やすいものを含める
#pragma once

struct s_char_double { char c; double d; };
struct s_char_ldouble { char c; long double ld; };
struct s_char_int128 { char c; __int128 i; };
struct s_hfa3 { float a, b, c; };
struct s_hva { double a; double b; double c; double d; };
union u_mixed { char c; long double ld; int i; };
struct s_flex { short n; long data[]; };
struct s_packed { char c; int i; } __attribute__((packed));
struct s_aligned { char c; _Alignas(32) char c2; };

struct bf_basic { int a : 3; int b : 5; char c; };
struct bf_cross { char a : 7; char b : 3; char c; };
struct bf_zero_int { char a; int : 0; char b; };
struct bf_zero_char { char a : 4; char : 0; char b : 4; };
struct bf_unnamed_long { char a; long : 4; };
struct bf_long_long { long long a : 40; long long b : 30; };
struct bf_char_type { char a : 2; };
struct bf_short_span { char a; short b : 10; };

enum e_small { E_SMALL_A, E_SMALL_B };
enum e_big { E_BIG_A = 0x100000000LL };
enum e_neg { E_NEG_A = -1 };
union u_bf_unnamed { char c; long : 4; };
struct bf_unnamed_packed { char a; long : 4; } __attribute__((packed));
struct bf_unnamed_after { char a; int : 3; char b; };
#pragma pack(push, 2)
struct bf_unnamed_pragma { char a; long : 4; };
#pragma pack(pop)
