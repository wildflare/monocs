// 構造体の局所変数を値にする（SROA・mem2reg。2026-11-21）: 共用体の 4 バイトの書き込みと 8 バイトの読み出し、
// ループをまたぐ構造体、アドレスを渡す構造体（値にしない）、浮動小数点のメンバー（値にしない）、呼び出しの
// 引数・戻り値、部分的な書き込みの合成、1・2 バイトのメンバー
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef union { int32_t i; double d; void *p; uint64_t bits; } U;
typedef struct { U u; int64_t tag; } V;
typedef struct { uint8_t a, b; uint16_t c; int32_t d; uint64_t e; } Mix;   // 16 バイト

__attribute__((noinline)) static V mk(int32_t i, int64_t t) { V v; v.u.bits = 0x1111111122222222ULL; v.u.i = i; v.tag = t; return v; }
__attribute__((noinline)) static int64_t take(V v) { return (int64_t)v.u.bits ^ v.tag; }
__attribute__((noinline)) static void touch(V *v) { v->tag += 100; }

int main(void)
{
    V acc = mk(1, 2);
    for (int k = 0; k < 50; k++) {
        V t = mk(k, k * 3);
        if (k & 1) {
            acc.u.i += t.u.i;
            acc.tag ^= t.tag;
        } else {
            V s = acc;
            s.u.bits += 7;
            acc = s;
        }
    }
    printf("acc %llx %lld %lld\n", (unsigned long long)acc.u.bits, (long long)acc.tag, (long long)take(acc));
    V e = mk(5, 6);
    touch(&e);                      // アドレスを渡す（値にしない）
    printf("esc %d %lld\n", e.u.i, (long long)e.tag);
    V f;
    f.u.d = 2.5;                    // 浮動小数点（値にしない）
    f.tag = 1;
    printf("fp %.2f %llx\n", f.u.d + 1, (unsigned long long)f.u.bits);
    Mix m = {0};
    for (int k = 0; k < 10; k++) {
        m.a += 3;
        m.b ^= (uint8_t)k;
        m.c += 1000;
        m.d -= k;
        m.e = m.e * 31 + m.a + m.b + m.c;
    }
    uint64_t raw[2];
    memcpy(raw, &m, 16);
    printf("mix %u %u %u %d %llu %llx %llx\n", m.a, m.b, m.c, m.d, (unsigned long long)m.e,
           (unsigned long long)raw[0], (unsigned long long)raw[1]);
    V z = {0};
    z.u.i = -1;
    printf("zero %llx\n", (unsigned long long)z.u.bits);
    return 0;
}
