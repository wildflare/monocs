// 0 にした値に即値を足す・比べる（2026-11-21。slimcc が「add w10, wzr, #1」を出し、即値の add では 31 番が sp なので
// sp + 1 になっていた。QuickJS の文字列の比べで見つけた）
#include <stdio.h>
#include <stdint.h>
typedef struct { union { void *ptr; int32_t i; } u; int64_t tag; } V;
typedef struct { uint32_t len; uint8_t w, depth; V left, right; } Rope;
typedef struct { V stack[60]; int stack_len; } It;
static void init(It *s, V v) { s->stack_len = 0; s->stack[s->stack_len++] = v; }
static V *next(It *s, V *out)
{
    if (s->stack_len == 0) return NULL;
    V val = s->stack[--s->stack_len];
    for (;;) {
        if (val.tag == -7) { *out = val; return out; }
        Rope *r = val.u.ptr;
        if (s->stack_len >= 60) { printf("overflow %d\n", s->stack_len); return NULL; }
        s->stack[s->stack_len++] = r->right;
        val = r->left;
    }
}
static int cmp(V a, V b)
{
    It i1, i2; V o1, o2;
    init(&i1, a); init(&i2, b);
    V *p1 = next(&i1, &o1), *p2 = next(&i2, &o2);
    printf("lens %d %d %d %d\n", i1.stack_len, i2.stack_len, p1 == &o1, p2 == &o2);
    return 0;
}
// 0 からの足し算・引き算・比べ（定数の 0 が残る形）
struct C { int n; long m; };
__attribute__((noinline)) static void zc(struct C *c) { c->n = 0; c->n++; c->m = 0; c->m -= 5; }
__attribute__((noinline)) static int zcmp(struct C *c) { c->n = 0; return c->n < 3; }
static int main2(void)
{
    struct C c;
    zc(&c);
    printf("zc %d %ld %d\n", c.n, c.m, zcmp(&c));
    return 0;
}

int main(void)
{
    static Rope r1, r2, r3;
    V s = {{(void *)1}, -7};
    r1.left = s; r1.right = s;
    r2.left = (V){{&r1}, -6}; r2.right = s;
    r3.left = (V){{&r1}, -6}; r3.right = s;
    cmp((V){{&r2}, -6}, (V){{&r3}, -6});
    main2();
    return 0;
}
