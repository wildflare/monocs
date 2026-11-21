// 構造体を受け渡しする小さな関数のインライン展開（2026-11-21。QuickJS の JSValue と同じ形）
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef union { int32_t i; double d; void *p; } U;
typedef struct { U u; int64_t tag; } V;            // 16 バイト（JSValue と同じ）
typedef struct { int a, b, c; } S12;                // 12 バイト（8 の倍数でない）
typedef struct { double x, y; } D2;                 // HFA

static inline V mk_int(int32_t v) { return (V){.u.i = v, .tag = 0}; }
static inline V mk_f(double d) { return (V){.u.d = d, .tag = 7}; }
static inline int is_int(V v) { return v.tag == 0; }
static inline V add(V a, V b)
{
    if (is_int(a) && is_int(b))
        return mk_int(a.u.i + b.u.i);
    double x = is_int(a) ? a.u.i : a.u.d, y = is_int(b) ? b.u.i : b.u.d;
    return mk_f(x + y);
}
static inline V bump(V v) { v.u.i += 1; return v; }     // 引数を書き換える（呼び出し元は変わらない）
static inline void set(V *dst, V v) { *dst = v; }
static inline S12 s12(int a) { return (S12){a, a * 2, a * 3}; }
static inline int s12sum(S12 s) { return s.a + s.b + s.c; }
static inline D2 d2(double x) { return (D2){x, x * 0.5}; }
static inline double d2sum(D2 d) { return d.x + d.y; }

int main(void)
{
    V st[16];
    int sp = 0;
    for (int i = 0; i < 10; i++)
        set(&st[sp++], (i & 1) ? mk_f(i * 0.25) : mk_int(i));
    V acc = mk_int(0);
    for (int i = 0; i < sp; i++)
        acc = add(acc, st[i]);
    printf("acc tag %lld %.6f\n", (long long)acc.tag, acc.tag ? acc.u.d : acc.u.i);
    V a = mk_int(41), b = bump(a);
    printf("bump %d %d\n", a.u.i, b.u.i);
    V n = add(add(mk_int(1), mk_int(2)), add(mk_int(3), bump(mk_int(4))));   // 入れ子
    printf("nested %d %lld\n", n.u.i, (long long)n.tag);
    long t = 0;
    for (int i = 0; i < 1000; i++)
        t += s12sum(s12(i)) + (long)d2sum(d2(i));
    printf("s12 %ld\n", t);
    // 共用体を別の型で読み直す（型の組 0 で写すので、別名の判定で取り違えない）
    V f = mk_f(1.5);
    uint64_t bits;
    memcpy(&bits, &f.u, 8);
    V g = bump(f);
    printf("bits %llx %llx\n", (unsigned long long)bits, (unsigned long long)g.u.i);
    return 0;
}
