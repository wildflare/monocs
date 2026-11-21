// 計算型の goto（GNU の &&label・goto *p。2026-11-21。中間表現で扱うようにした）: 静的な表を引く小さな
// インタプリタ（QuickJS の DIRECT_DISPATCH と同じ形）、局所の表、値がラベルをまたいで生きる、ループの中
#include <stdio.h>

enum { OP_PUSH, OP_ADD, OP_MUL, OP_DUP, OP_JNZ, OP_DEC, OP_SWAP, OP_POP, OP_HALT };

__attribute__((noinline)) static long run(const int *code, long x, double *fout)
{
    static const void *const table[] = {&&op_push, &&op_add, &&op_mul, &&op_dup, &&op_jnz, &&op_dec, &&op_swap,
                                        &&op_pop, &&op_halt};
    long stack[64];
    int sp = 0;
    const int *pc = code;
    double f = 0.5;     // 浮動小数点もラベルをまたいで
    long steps = 0;
#define NEXT goto *table[*pc++]
    stack[sp++] = x;
    NEXT;
op_push:
    stack[sp++] = *pc++;
    steps++;
    NEXT;
op_add:
    sp--;
    stack[sp - 1] += stack[sp];
    f += 1.25;
    steps++;
    NEXT;
op_mul:
    sp--;
    stack[sp - 1] *= stack[sp];
    f *= 1.5;
    steps++;
    NEXT;
op_dup:
    stack[sp] = stack[sp - 1];
    sp++;
    steps++;
    NEXT;
op_jnz: {
    int off = *pc++;
    if (stack[--sp])
        pc += off;
    steps++;
    NEXT;
}
op_dec:
    stack[sp - 1]--;
    steps++;
    NEXT;
op_swap: {
    long t = stack[sp - 1];
    stack[sp - 1] = stack[sp - 2];
    stack[sp - 2] = t;
    steps++;
    NEXT;
}
op_pop:
    sp--;
    steps++;
    NEXT;
op_halt:
    *fout = f;
    return stack[sp - 1] * 1000 + steps;
}

// 局所の表（番地を計算して選ぶ）と、goto *p の飛び先が 1 つの関数
__attribute__((noinline)) static int classify(int v)
{
    void *where[] = {&&neg, &&zero, &&pos};
    int r = 0;
    goto *where[(v > 0) - (v < 0) + 1];
neg:
    r -= 10;
zero:
    r += 1;
    return r;
pos:
    return 100 + v;
}

int main(void)
{
    // acc = x; n = 10; do { acc = acc * 2 + 1; n--; } while (n);
    static const int prog[] = {OP_PUSH, 10,                                  // [acc, n]
                               OP_SWAP, OP_PUSH, 2, OP_MUL, OP_PUSH, 1, OP_ADD, // [n, acc']
                               OP_SWAP, OP_DEC, OP_DUP, OP_JNZ, -12,           // 戻る
                               OP_POP, OP_HALT};
    double f;
    for (long x = 0; x < 3; x++)
        printf("run %ld %ld %.6f\n", x, run(prog, x, &f), f);
    printf("classify %d %d %d\n", classify(-5), classify(0), classify(7));
    return 0;
}
