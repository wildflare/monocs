// Cortex-A53（Zero 2 W）の命令の遅延と、同時に出せる組を測る（命令の並べ替え cc/sched.inc の表の元）。
// 実機で: nos.py run tools/perf/a53lat.c   （clock の measured が 1000MHz のとき。1 サイクル = 1ns）
// 各試験は、本体を ITER 回まわす。表示は「本体の命令 1 つあたりのサイクル」で、空の本体の時間（ループと引数の
// 出し入れ。約 50 サイクル）を引いたもの。「依存」は前の結果を次が使う鎖（遅延）、「独立」「組」は出せる速さ
// （1 サイクルに 2 つ出せれば 0.5）。鎖が 2 命令の組なら、その 2 つの遅延の和は表示の 2 倍。
#include <stdio.h>
#include <time.h>

#define ITER 100000
#define R10(x) x x x x x x x x x x
#define R100(x) R10(R10(x))
#define R50(x) R25(x) R25(x)
#define R25(x) R10(x) R10(x) x x x x x

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static long cell[4] = {0};
static double base;

#define RUN(body)                                                                              \
    ({                                                                                         \
        long a = 0, b = 1, c = 2, d = 3;                                                       \
        long *p = &cell[0];                                                                    \
        cell[0] = (long)&cell[0];                                                              \
        cell[1] = 8;                                                                           \
        cell[2] = cell[3] = 0;                                                                 \
        double t0 = now();                                                                     \
        for (int i = 0; i < ITER; i++)                                                         \
            __asm__ volatile(body                                                              \
                             : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(p)                     \
                             :                                                                 \
                             : "x9", "x10", "x11", "x12", "v0", "v1", "v2", "v3", "v4", "v5",  \
                               "v6", "v7", "memory");                                          \
        (now() - t0) / ITER;                                                                   \
    })
// n: 本体の命令の数
#define TEST(name, n, body) printf("%-34s %5.2f\n", name, (RUN(body) - base) / (n))
#define T1(name, ins) TEST(name, 100, R100(ins))              // 1 命令を 100 回
#define T2(name, ins) TEST(name, 200, R100(ins))              // 2 命令を 100 回
#define T4(name, ins) TEST(name, 100, R25(ins))               // 4 命令を 25 回
// x9 = 8（cell[1] の値）から始める鎖
#define T1i(name, ins) TEST(name, 100, "mov x9, #8\n" R100(ins))
#define T2i(name, ins) TEST(name, 200, "mov x9, #8\n" R100(ins))
#define T4i(name, ins) TEST(name, 100, "mov x9, #8\n" R25(ins))

int main(void)
{
    base = RUN("");
    printf("1 命令あたりのサイクル（1000MHz。空の本体 %.1f サイクルを引いた）\n", base);
    // 準備: v1 = 1.0、v0 = 1.0
    __asm__ volatile("fmov d1, #1.0\n fmov d0, #1.0\n fmov d2, #1.0\n fmov d3, #1.0\n fmov d4, #1.0\n" ::: "v0", "v1", "v2", "v3", "v4");
    T1("add 依存", "add %0, %0, #1\n");
    T4("add 独立 4 つ", "add %0, %0, #1\n add %1, %1, #1\n add %2, %2, #1\n add %3, %3, #1\n");
    T1("add lsl 依存", "add %0, %0, %1, lsl #2\n");
    T4("add lsl 独立", "add %0, %0, %4, lsl #2\n add %1, %1, %4, lsl #2\n add %2, %2, %4, lsl #2\n add %3, %3, %4, lsl #2\n");
    T1("add sxtw 依存", "add %0, %0, %w1, sxtw\n");
    T1("and 依存", "and %0, %0, #0xff\n");
    T1("lsl 依存", "lsl %0, %0, #1\n");
    T1("ubfx 依存", "ubfx %0, %0, #1, #8\n");
    T1("sxtw 依存", "sxtw %0, %w0\n");
    T2("csel 依存", "cmp %1, #0\n csel %0, %0, %1, ne\n");
    T2("cmp+cset 依存", "cmp %0, #5\n cset %0, eq\n");
    T1("mov reg 依存", "mov %0, %0\n");
    T1("ldr 依存（自分を指す）", "ldr %4, [%4]\n");
    T4("ldr 独立", "ldr x9, [%4]\n ldr x10, [%4, #8]\n ldr x9, [%4, #16]\n ldr x10, [%4, #24]\n");
    T2("ldr+add 依存", "ldr %4, [%4]\n add %4, %4, #0\n");
    T2("ldr w sxtw 依存", "ldrsw %0, [%4, #8]\n add %4, %4, %0\n");
    T1("ldr idx 依存", "ldr %4, [%4, %0, lsl #3]\n");
    T4("str 独立", "str %0, [%4, #8]\n str %1, [%4, #16]\n str %2, [%4, #24]\n str %3, [%4, #8]\n");
    T2("str→ldr 同じ場所", "str %0, [%4, #8]\n ldr %0, [%4, #8]\n");
    T4("ldp 独立", "ldp x9, x10, [%4]\n ldp x9, x10, [%4, #16]\n ldp x9, x10, [%4]\n ldp x9, x10, [%4, #16]\n");
    T1("mul w 依存", "mul %w0, %w0, %w1\n");
    T1("mul x 依存", "mul %0, %0, %1\n");
    T4("mul w 独立", "mul %w0, %w0, %w1\n mul %w1, %w1, %w1\n mul %w2, %w2, %w1\n mul %w3, %w3, %w1\n");
    T4("mul x 独立", "mul %0, %0, %1\n mul %1, %1, %1\n mul %2, %2, %1\n mul %3, %3, %1\n");
    T1("madd w 依存（足す側）", "madd %w0, %w1, %w1, %w0\n");
    T1("madd x 依存（掛ける側）", "madd %0, %0, %1, %1\n");
    T1("smull 依存", "smull %0, %w0, %w1\n");
    T1("umulh 依存", "umulh %0, %0, %1\n");
    T1("sdiv w 依存（÷1）", "sdiv %w0, %w0, %w1\n");
    T1("sdiv x 依存（÷1）", "sdiv %0, %0, %1\n");
    T2("udiv w 大きい値÷1", "mov %w0, #-1\n udiv %w0, %w0, %w1\n");
    T1("fadd d 依存", "fadd d0, d0, d1\n");
    T4("fadd d 独立", "fadd d0, d0, d1\n fadd d2, d2, d1\n fadd d3, d3, d1\n fadd d4, d4, d1\n");
    T1("fmul d 依存", "fmul d0, d0, d1\n");
    T4("fmul d 独立", "fmul d0, d0, d1\n fmul d2, d2, d1\n fmul d3, d3, d1\n fmul d4, d4, d1\n");
    T1("fmadd d 依存（足す側）", "fmadd d0, d1, d1, d0\n");
    T1("fmadd d 依存（掛ける側）", "fmadd d0, d0, d1, d1\n");
    T4("fmadd d 独立", "fmadd d0, d1, d1, d0\n fmadd d2, d1, d1, d2\n fmadd d3, d1, d1, d3\n fmadd d4, d1, d1, d4\n");
    T1("fadd s 依存", "fadd s0, s0, s1\n");
    T1("fdiv d 依存", "fdiv d0, d0, d1\n");
    T1("fdiv s 依存", "fdiv s0, s0, s1\n");
    T1("fsqrt d 依存", "fsqrt d0, d0\n");
    T1("fabs d 依存", "fabs d0, d0\n");
    T1("fneg d 依存", "fneg d0, d0\n");
    T1("fmov d,d 依存", "fmov d0, d0\n");
    T2("fmov x→d→x 往復", "fmov d5, %0\n fmov %0, d5\n");
    T2("scvtf+fcvtzs 往復", "scvtf d5, %0\n fcvtzs %0, d5\n");
    T2("fcvt d→s→d 往復", "fcvt s5, d0\n fcvt d0, s5\n");
    T4("ldr d 独立", "ldr d5, [%4]\n ldr d4, [%4, #8]\n ldr d5, [%4, #16]\n ldr d4, [%4, #24]\n");
    T2("ldr d→fadd 依存", "ldr d5, [%4]\n fadd d0, d0, d5\n");
    // 同時に出せるか（2 つで 0.5 なら 1 サイクルに 2 つ）
    T4("組: add + add", "add %0, %0, #1\n add %1, %1, #1\n add %2, %2, #1\n add %3, %3, #1\n");
    T4("組: ldr + add", "ldr x9, [%4]\n add %0, %0, #1\n ldr x10, [%4, #8]\n add %1, %1, #1\n");
    T4("組: str + add", "str %2, [%4, #8]\n add %0, %0, #1\n str %3, [%4, #16]\n add %1, %1, #1\n");
    T4("組: ldr + str", "ldr x9, [%4]\n str %0, [%4, #8]\n ldr x10, [%4, #16]\n str %1, [%4, #24]\n");
    T4("組: ldr + ldr", "ldr x9, [%4]\n ldr x10, [%4, #8]\n ldr x9, [%4, #16]\n ldr x10, [%4, #24]\n");
    T4("組: mul + add", "mul %w0, %w0, %w1\n add %2, %2, #1\n mul %w1, %w1, %w1\n add %3, %3, #1\n");
    T4("組: mul + mul", "mul %w0, %w0, %w1\n mul %w2, %w2, %w1\n mul %w3, %w3, %w1\n mul %w1, %w1, %w1\n");
    T4("組: fadd + add", "fadd d0, d0, d1\n add %0, %0, #1\n fadd d2, d2, d1\n add %1, %1, #1\n");
    T4("組: fadd + fmul", "fadd d0, d0, d1\n fmul d2, d2, d1\n fadd d3, d3, d1\n fmul d4, d4, d1\n");
    T4("組: fadd + ldr", "fadd d0, d0, d1\n ldr x9, [%4]\n fadd d2, d2, d1\n ldr x10, [%4, #8]\n");
    T4("組: add lsl + add lsl", "add %0, %0, %4, lsl #2\n add %1, %1, %4, lsl #2\n add %2, %2, %4, lsl #2\n add %3, %3, %4, lsl #2\n");
    T4("組: add + b（分岐しない）", "add %0, %0, #1\n cbz %4, 1f\n add %1, %1, #1\n cbz %4, 1f\n1:\n");
    T4("組: csel + csel", "csel %0, %0, %1, ne\n csel %2, %2, %3, ne\n csel %1, %1, %0, ne\n csel %3, %3, %2, ne\n");
    T4("組: mov + mov", "mov x9, %0\n mov x10, %1\n mov x9, %2\n mov x10, %3\n");
    T4("組: movz + movk", "mov x9, #1\n movk x9, #2, lsl #16\n mov x10, #3\n movk x10, #4, lsl #16\n");
    // 2 回目に足したもの
    T4("mul w 独立（4 つの鎖）", "mul x9, x9, %1\n mul x10, x10, %1\n mul x11, x11, %1\n mul x12, x12, %1\n");
    T4("mul w 本当に独立", "mul w9, %w1, %w1\n mul w10, %w1, %w1\n mul w11, %w1, %w1\n mul w12, %w1, %w1\n");
    T4("mul x 本当に独立", "mul x9, %1, %1\n mul x10, %1, %1\n mul x11, %1, %1\n mul x12, %1, %1\n");
    T4("組: mul w + add 独立", "mul w9, %w1, %w1\n add %0, %0, #1\n mul w10, %w1, %w1\n add %2, %2, #1\n");
    T4("fadd d 本当に独立", "fadd d2, d1, d1\n fadd d3, d1, d1\n fadd d4, d1, d1\n fadd d5, d1, d1\n");
    T4("fmul d 本当に独立", "fmul d2, d1, d1\n fmul d3, d1, d1\n fmul d4, d1, d1\n fmul d5, d1, d1\n");
    T4("fmadd d 本当に独立", "fmadd d2, d1, d1, d1\n fmadd d3, d1, d1, d1\n fmadd d4, d1, d1, d1\n fmadd d5, d1, d1, d1\n");
    T4("組: fadd + add 独立", "fadd d2, d1, d1\n add %0, %0, #1\n fadd d3, d1, d1\n add %2, %2, #1\n");
    T4("組: fadd + ldr 独立", "fadd d2, d1, d1\n ldr x9, [%4]\n fadd d3, d1, d1\n ldr x10, [%4, #8]\n");
    T4("組: fadd + fmul 独立", "fadd d2, d1, d1\n fmul d3, d1, d1\n fadd d4, d1, d1\n fmul d5, d1, d1\n");
    T4("組: fadd s + fadd s 独立", "fadd s2, s1, s1\n fadd s3, s1, s1\n fadd s4, s1, s1\n fadd s5, s1, s1\n");
    T4("組: ldr d + fadd 独立", "ldr d6, [%4]\n fadd d2, d1, d1\n ldr d7, [%4, #8]\n fadd d3, d1, d1\n");
    T4("組: str + str", "str %0, [%4, #8]\n str %1, [%4, #16]\n str %2, [%4, #24]\n str %3, [%4, #8]\n");
    T4("組: lsl + lsl 独立", "lsl x9, %0, #1\n lsl x10, %0, #2\n lsl x11, %0, #3\n lsl x12, %0, #4\n");
    T4("組: ubfx + add 独立", "ubfx x9, %0, #1, #8\n add %1, %1, #1\n ubfx x10, %0, #2, #8\n add %2, %2, #1\n");
    T4("組: cmp + add", "cmp %0, #1\n add %1, %1, #1\n cmp %2, #1\n add %3, %3, #1\n");
    T4("組: add + madd 独立", "add %0, %0, #1\n madd w9, %w1, %w1, %w1\n add %2, %2, #1\n madd w10, %w1, %w1, %w1\n");
    T1i("ldr 添字の鎖（x9 = 8）", "ldr x9, [%4, x9]\n");
    T2i("ldr + add の鎖（添字）", "ldr x9, [%4, x9]\n add x9, x9, #0\n");
    T4i("ldr + add + add + add の鎖", "ldr x9, [%4, x9]\n add x9, x9, #0\n add x9, x9, #0\n add x9, x9, #0\n");
    T2i("ldr + lsl の鎖", "ldr x9, [%4, x9]\n lsl x9, x9, #0\n");
    T2("add + cbz（分岐しない）の鎖", "add x9, x9, #0\n cbz %4, 1f\n1:\n");
    T2("ldr d + fmov x の鎖", "ldr d5, [%4]\n fmov %4, d5\n");
    T2("fmov d,x + fmov x,d", "fmov d5, %0\n fmov %0, d5\n");
    T2i("ldr w + add（w）の鎖", "ldr w9, [%4, x9]\n add w9, w9, #0\n");
    T2i("ldrb + add の鎖", "ldrb w9, [%4, x9]\n add w9, w9, #0\n");
    T2("str + ldr の鎖（同じ場所、値）", "str x9, [%4, #16]\n ldr x9, [%4, #16]\n");
    T2("adrp + add", "adrp x9, cell\n add x9, x9, :lo12:cell\n");
    // 3 回目に足したもの
    T1i("add のずらした側の鎖", "add x9, %1, x9, lsl #2\n");
    T1i("add sxtw の拡張側の鎖", "add x9, %1, w9, sxtw\n");
    T1i("lsl（レジスタ）の鎖", "lsl x9, x9, %1\n");
    TEST("udiv x 大きい値÷1", 100, "mov x9, #-1\n" R100("udiv x9, x9, %1\n"));
    TEST("udiv w 大きい値÷1", 100, "mov w9, #-1\n" R100("udiv w9, w9, %w1\n"));
    TEST("sdiv x 大きい値÷1", 100, "mov x9, #0x7fffffffffffffff\n" R100("sdiv x9, x9, %1\n"));
    TEST("udiv x 大きい÷大きい + add", 200, "mov x9, #-1\n mov x10, #-1\n" R100("udiv x9, x10, x9\n add x9, x9, x10\n"));
    TEST("ldp の鎖", 100, "mov x9, %4\n" R100("ldp x9, x10, [x9]\n"));
    TEST("ldr 後置で番地を書く（番地の鎖）", 100, "mov x9, %4\n" R50("ldr x10, [x9], #8\n ldr x10, [x9], #-8\n"));
    T2i("scvtf + fmov x の鎖", "scvtf d5, x9\n fmov x9, d5\n");
    T2i("fmov d + fcvtzs の鎖", "fmov d5, x9\n fcvtzs x9, d5\n");
    T1i("madd x 足す側の鎖", "madd x9, %1, %1, x9\n");
    T1i("msub w 足す側の鎖", "msub w9, %w1, %w1, w9\n");
    TEST("fmov d,x + fadd + fmov x,d の鎖", 300, "mov x9, #8\n" R100("fmov d5, x9\n fadd d5, d5, d5\n fmov x9, d5\n"));
    T4i("cmp + b.eq（分岐しない）+ add + add", "cmp x9, #100\n b.eq 1f\n add x10, x10, #1\n add x11, x11, #1\n1:\n");
    T4i("ldr + 使う（すぐ）×2", "ldr x10, [%4, #8]\n add x11, x10, #1\n ldr x12, [%4, #16]\n add x11, x12, #1\n");
    T4i("ldr 3 つ先で使う", "ldr x10, [%4, #8]\n add x11, x11, #1\n add x12, x12, #1\n add x11, x10, #1\n");
    T4i("ldr + str + add + add", "ldr x10, [%4, #8]\n str x11, [%4, #16]\n add x11, x11, #1\n add x12, x12, #1\n");
    // 4 回目: 番地・値・分岐に回るときの遅延（繰り返しの周期で見る。鎖はない）
    T2i("add → str の番地", "add x10, x9, #8\n str x11, [%4, x10]\n");
    T2i("add → ldr の番地（使わない）", "add x10, x9, #8\n ldr x11, [%4, x10]\n");
    T2i("add → str の値", "add x10, x9, #8\n str x10, [%4, #16]\n");
    T2i("ldr → str の値", "ldr x10, [%4, #8]\n str x10, [%4, #16]\n");
    T2i("ldr → str の番地", "ldr x10, [%4, #8]\n str x11, [%4, x10]\n");
    T2i("ldr → cbz", "ldr x10, [%4, #16]\n cbz x10, 1f\n1:\n");
    T4i("ldr → cmp → b.ne", "ldr x10, [%4, #16]\n cmp x10, #1\n b.eq 1f\n add x11, x11, #1\n1:\n");
    T2i("ldr d → fadd", "ldr d6, [%4]\n fadd d2, d6, d1\n");
    T2i("ldr w → add", "ldr w10, [%4, #8]\n add w11, w10, #1\n");
    T2i("ldrb → add", "ldrb w10, [%4, #8]\n add w11, w10, #1\n");
    T2i("ldrsw → add", "ldrsw x10, [%4, #8]\n add x11, x10, #1\n");
    T2i("ldr → add lsl（ずらす側）", "ldr x10, [%4, #8]\n add x11, x12, x10, lsl #3\n");
    T2i("ldr → mul", "ldr x10, [%4, #8]\n mul x11, x10, x10\n");
    T2i("mul → add", "mul x10, x9, x9\n add x11, x10, #1\n");
    T2i("mul → str の値", "mul x10, x9, x9\n str x10, [%4, #16]\n");
    T2i("lsl → add", "lsl x10, x9, #3\n add x11, x10, #1\n");
    T2i("fadd → str d の値", "fadd d2, d1, d1\n str d2, [%4, #16]\n");
    T2i("fadd → fmov x", "fadd d2, d1, d1\n fmov x10, d2\n");
    T2i("fcmp → b.eq", "fcmp d1, d0\n b.eq 1f\n1:\n");
    T2i("fcmp → cset", "fcmp d1, d0\n cset x10, eq\n");
    T2i("cmp → cset", "cmp x9, #1\n cset x10, eq\n");
    T2i("ldp → add", "ldp x10, x11, [%4]\n add x12, x11, #1\n");
    T4i("str 4 つ続け（同じ組なし）", "str x9, [%4, #8]\n str x9, [%4, #16]\n str x9, [%4, #24]\n str x9, [%4, #8]\n");
    T4i("stp", "stp x9, x10, [%4, #8]\n stp x9, x10, [%4, #8]\n stp x9, x10, [%4, #8]\n stp x9, x10, [%4, #8]\n");
    return 0;
}
