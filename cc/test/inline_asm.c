#include "test.h"

#ifdef __aarch64__

// AArch64（slimcc-a64）版。x86 版と同じ観点のうち、AArch64 で意味のあるものを確かめる。
// x87、x86 固有のレジスタ制約、条件フラグの出力（=@cc）、方言の切り替えは対象外

void mem_operand_triival() {
  int i;
  __asm__ ("mov w16, #11; str w16, %0" : "=m"(i) :: "x16");
  ASSERT(11, i);

  struct {
    char c,d,e;
  } s;
  __asm__ ("mov w16, #22; strb w16, %0" : "=m"(s.d) :: "x16");
  ASSERT(22, s.d);

  short arr[10];
  __asm__ ("mov w16, #33; strh w16, %0" : "=m"(arr[7]) :: "x16");
  ASSERT(33, arr[7]);

  static struct {
    char c;
    struct {
      long long l[11];
    } s[11];
  } s2 [11];

  __asm__ ("mov x16, #44; str x16, %0" : "=m"(s2[3].s[7].l[5]) :: "x16");
  ASSERT(44, s2[3].s[7].l[5]);

  int m = 55, r;
  __asm__ ("ldr %w0, %1" : "=r"(r) : "m"(m));
  ASSERT(55, r);
}

void verify_123(int i, int j, int k) {
  ASSERT(11, i);
  ASSERT(22, j);
  ASSERT(33, k);
}

void reg_label() {
  register int
  i __asm__("x0"),
  j __asm__("w1"),
  k __asm__("x2") = 33;
  __asm__ ("mov %w0, #11;"
           "mov %w1, #22;"
           "bl verify_123"
           : "=r"(i), "=r"(j) : "r"(k) : "x30", "memory");
}

void i_constraint(int var) {
  __asm__ volatile (
    "mov w0, %w[lvar];"
    "add w0, w0, #%[imm];"
    "mov w1, #%c[j];"
    "mov w2, #33;"
    "bl verify_123"
    :
    : [imm]"i"(6), [lvar]"ir"(var), [j]"n"(22)
    : "x0", "x1", "x2", "x30", "memory"
  );
}

void reg_assign(void) {
  long a, b, c;
  __asm__ volatile(
    "mov %0, #1;"
    "add %1, %3, #2;"
    "add %2, %3, %4;"
    : "=&r"(a), "=&r"(b), "=r"(c)
    : "r"(10L), "r"(20L)
  );
  ASSERT(1, a);
  ASSERT(12, b);
  ASSERT(30, c);

  long x = 5;
  __asm__("add %0, %0, #10" : "+r"(x));
  ASSERT(15, x);

  long y = 3;
  __asm__("add %0, %1, #1" : "=r"(y) : "0"(y));
  ASSERT(4, y);

  int w;
  __asm__("mov %w0, %w1" : "=r"(w) : "rZ"(0));
  ASSERT(0, w);
  __asm__("mov %x0, %x1" : "=r"(x) : "rZ"(0L));
  ASSERT(0, x);
}

int fp_operand(void) {
  double d = 1.5, e;
  __asm__("fadd %d0, %d1, %d1" : "=w"(e) : "w"(d));
  ASSERT(1, e == 3.0);

  float f = 2.0f, g;
  __asm__("fmul %s0, %s1, %s1" : "=w"(g) : "w"(f));
  ASSERT(1, g == 4.0f);

  float register fz __asm__("v3") = 0.0f;
  float out;
  __asm__("fmov %s0, %s1" : "=w"(out) : "w"(fz));
  ASSERT(1, out == 0.0f);
  return 1;
}

int asm_goto(int arg){
  __asm__ goto (
    "subs %w0, %w0, #5; b.eq %l2;"
    "subs %w0, %w0, #2; b.lt %l1;"
    :
    : "r"(arg)
    :
    : labA, labB
  );
  return 11;
  labA:;
  return 22;
  labB:;
  return 33;
}

int asm_goto_output(int *arg){
  __asm__ goto (
    "subs %w0, %w0, #5; b.eq %l[labB];"
    "subs %w0, %w0, #2; b.lt %l[labA];"
    "mov %w0, #3;"
    : "+r"(*arg)
    :
    : "x19"
    : labA, labB
  );
  return *arg + 10;
  labA:;
  return *arg + 20;
  labB:;
  return *arg + 30;
}

int asm_goto_local_label(void) {
#define LE_OR_QUIT(_v) do { __label__ LE;                    \
    __asm goto ("cmp %w[val], #10; b.le %l[LE]; b %l[QUIT]" \
      ::[val]"r"(_v) : "cc" : LE, QUIT);                     \
    LE:                                                      \
  } while (0)

  int cnt = 0;
  LE_OR_QUIT(9);
  cnt += 1;
  LE_OR_QUIT(10);
  cnt += 10;
  LE_OR_QUIT(11);
  cnt += 100;
QUIT:
  return cnt;
}

int ptr_conversion(char *str) {
  int res;
  __asm__ volatile (
    "mov x0, %1;"
    "mov x1, %2;"
    "blr %3;"
    "mov %w0, w0"
    : "=r"(res)
    : "r"("bar"), "r"(str), "r"(strcmp)
    : "x0", "x1", "x30", "memory"
  );
  return !res;
}

// asm が呼び出し先保存レジスタ（x19〜x28）を壊しても、局所変数の基準（x19）や
// _BitInt の作業バッファ（x20）が保たれること
int callee_saved_clobber(int v) {
  _Alignas(64) int res = v;
  _BitInt(100) b = 7;
  __asm__ volatile(
    "mov x19, #1; mov x20, #2; mov x21, #3; mov x22, #4; mov x23, #5;"
    "mov x24, #6; mov x25, #7; mov x26, #8; mov x27, #9; mov x28, #10;"
    "fmov d8, xzr; fmov d15, xzr"
    ::: "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "d8", "d15"
  );
  ASSERT(0, 63 & (intptr_t)&res);
  b = b * 6;
  return res + (int)b;
}

int main(void) {
  mem_operand_triival();
  reg_label();
  i_constraint(5);
  reg_assign();
  ASSERT(1, fp_operand());

  ASSERT(0, ptr_conversion("foo"));
  ASSERT(1, ptr_conversion("bar"));

  ASSERT(11, asm_goto(7));
  ASSERT(22, asm_goto(6));
  ASSERT(33, asm_goto(5));

  ASSERT(13, asm_goto_output(&(int){7}));
  ASSERT(19, asm_goto_output(&(int){6}));
  ASSERT(30, asm_goto_output(&(int){5}));

  ASSERT(11, asm_goto_local_label());
  ASSERT(142, callee_saved_clobber(100));

  printf("OK\n");
}

#else

void mem_operand_triival() {
  int i;
  __asm__ ("movl $11, %0" : "=m"(i));
  ASSERT(11, i);

  struct {
    char c,d,e;
  } s;
  __asm__ ("movb $22, %0" : "=m"(s.d));
  ASSERT(22, s.d);

  short arr[10];
  __asm__ ("movw $33, %0" : "=m"(arr[7]));
  ASSERT(33, arr[7]);

  static struct {
    char c;
    struct {
      long long l[11];
    } s[11];
  } s2 [11];

  __asm__ ("movq $44, %0" : "=m"(s2[3].s[7].l[5]));

  ASSERT(44, s2[3].s[7].l[5]);
}

void verify_123(int i, int j, int k) {
  ASSERT(11, i);
  ASSERT(22, j);
  ASSERT(33, k);
}
void reg_label() {
  register int
  i __asm__("di"),
  j __asm__("si"),
  k __asm__("dx");
  __asm__ ("movl $11, %0;"
           "movl $22, %1;"
           "movl $33, %2;"
           "call verify_123"
           : "=r"(i), "=r"(j) : "r"(k));
}

void i_constraint(int var) {
  static int v = 22;
  static struct { char c; long long i; } s = {.i = 33};
  __asm__ volatile (
    "movl %[lvar], %%edi;"
    "addl %[imm], %%edi;"
#if !defined(__PIC__)
    "movabsq %[ptr], %%rsi; movl (%%rsi), %%esi;"
    "movabsq %[agg], %%rdx; movl (%%rdx), %%edx;"
    "movabsq %[fun], %%rax; callq *%%rax;"
#else
    "movl $22, %%esi;"
    "movl $33, %%edx;"
    "call verify_123"
#endif
    :
    : [imm]"i"(6), [lvar]"ir"(var)
#if !defined(__PIC__)
      , [ptr]"i"(&v), [agg]"i"(&s.i), [fun]"i"(verify_123)
#endif
    : "rsi", "rdi", "rdx", "rax"
  );
}

void reg_assign(void) {
  int a,b,c,d,e,f,g;
  __asm__ volatile(
    "movl %%edx, %3;"
    "movl %%ebp, %6;"
    :"=a"(a), "=b"(b), "=c"(c), "=&r"(d), "=S"(e), "=D"(f), "=&r"(g)
    :"0"(0), "1"(0), "2"(0), "Q"(44), "4"(0), "5"(0), "R"(55)
  );
  ASSERT(44, d);
  ASSERT(55, g);
}

long long *indir_flag(void) { static long long var; return &var; }
int float_flag(float f) {
  float register fz __asm__("xmm3") = 0.0f;
  static int e;
  short np;
  char p;
  __asm__ (
    "xorps %%xmm0, %%xmm0;"
    "ucomiss %[f_in], %%xmm3"
    : "=@ccp"(p), "=@ccnp"(np), "=@cce"(e), "=@ccne"(*indir_flag())
    : [f_in]"x"(f), "x"(fz)
    : "xmm0", "cc"
  );
  return p * 1000 + np * 100 + e * 10 + *indir_flag();
}

int asm_goto(int arg){
  __asm__ goto (
    "subl $5, %0; je %l2;"
    "subl $2, %0; jl %l1;"
    :
    : "r"(arg)
    :
    : labA, labB
  );
  return 11;
  labA:;
  return 22;
  labB:;
  return 33;
}

int asm_goto_output(int *arg){
  __asm__ goto (
    "subl $5, %0; je %l[labB];"
    "subl $2, %0; jl %l[labA];"
    "movq $0, %%rbp;"
    "movl $3, %0;"
    : "+r"(*arg)
    :
    : "rbp"
    : labA, labB
  );
  return *arg + 10;
  labA:;
  return *arg + 20;
  labB:;
  return *arg + 30;
}

int asm_goto_local_label(void) {
#define LE_OR_QUIT(_v) do { __label__ LE;                    \
    __asm goto ("cmpl $10, %[val]; jle %l[LE]; jmp %l[QUIT]" \
      ::[val]"r"(_v) : "cc" : LE, QUIT);                     \
    LE:                                                      \
  } while (0)

  int cnt = 0;
  LE_OR_QUIT(9);
  cnt += 1;
  LE_OR_QUIT(10);
  cnt += 10;
  LE_OR_QUIT(11);
  cnt += 100;
QUIT:
  return cnt;
}

void x87_clobber(void) {
  volatile long double f1;
  volatile double f2;
  volatile float f3;

  __asm__ volatile (""::
    "f"((float){11}),
    "t"((double){22}),
    "u"((long double){33}) : "%st", "st(1)"
    );
  __asm__ volatile ("": "=t"(f1));
  __asm__ volatile ("": "=t"(f2));
  ASSERT(1, 33 == f1);
  ASSERT(1, 11 == f2);

  __asm__ volatile (""::
    "u"((float){44}),
    "f"((double){55}),
    "t"((long double){66}) : "st"
    );
  __asm__ volatile("": "=t"(f3));
  ASSERT(1, 55 == f3);

  __asm__ volatile (""::
    "f"((float){77}),
    "u"((double){88}),
    "t"((long double){99}) : "st", "st(1)", "st(2)"
    );
  __asm__ volatile ("": "=t"(f1));
  __asm__ volatile ("": "=t"(f2));
  ASSERT(1, 88 == f1);
  ASSERT(1, 77 == f2);
}

int ptr_conversion(char *str) {
    int res;
    __asm__ volatile (
      "  leaq %3, %%rsi;"
      "  call *%2"
      : "=a"(res)
      : "D"("bar"), "r"(strcmp), "m"(*str)
      : "rsi"
    );
    return !res;
}

struct Large {
    char arr[33];
    int i;
};

int exhaust_inner2(struct Large s) {
  int i[] = {1,2,3,4,5,6,7,8,9,10};

  _Alignas(1024) int res;
  ASSERT(0, 1023 & (intptr_t)&res);

  int register init __asm__("%bh") = 0;
  __asm__ volatile(
    "shl $4, %1;"
    "add %1, %%ebx;"
    "shl $3, %%eax;"
    "shl $3, %%ecx;"
    "shl $3, %%edx;"
    "add %%eax, %%ebx;"
    "add %%ecx, %%ebx;"
    "add %%edx, %%ebx;"
    "shl $2, %%esi;"
    "shl $2, %%edi;"
    "shl $2, %%ebp;"
    "add %%edi, %%ebx;"
    "add %%esi, %%ebx;"
    "add %%ebp, %%ebx;"
    "shl $1, %%r8d;"
    "shl $1, %%r9d;"
    "shl $1, %%r10d;"
    "shl $1, %%r11d;"
    "add %%r8d, %%ebx;"
    "add %%r9d, %%ebx;"
    "add %%r10d, %%ebx;"
    "add %%r11d, %%ebx;"
    : "=b"(res),"+r"(s.i),
      "+U"(i[6]),"+U"(i[7]),"+U"(i[8]),"+U"(i[9]), // r8,r9,r10,r11
      "+R"(i[0]),"+R"(i[1]),"+R"(i[2]), // si,di,bp
      "+Q"(i[3]),"+Q"(i[4]),"+Q"(i[5]) // ax,cx,dx
    : "r"(init)
  );

  ASSERT(68, i[6] + i[7] + i[8] + i[9]);
  ASSERT(120, i[3] + i[4] + i[5]);
  ASSERT(24, i[0] + i[1] + i[2]);
  ASSERT(272, s.i);
  return res;
}

int exhaust_inner(void) {
  struct Large s = {.i = 17};
  return exhaust_inner2(s);
}

void exhaust(void) {
  void *(arr)[12];
  int res;
  __asm__ volatile(
    "movq %%r12, %[arr];"
    "movq %%r13, 8+0%[arr];"
    "movq %%r14, 16+0%[arr];"
    "movq %%r15, 24+0%[arr];"
    "movq %%rbp, 32+0%[arr];"
    "movq %%rbx, 40+0%[arr];"
    "call exhaust_inner;"
    "movq %%r12, 48+0%[arr];"
    "movq %%r13, 56+0%[arr];"
    "movq %%r14, 64+0%[arr];"
    "movq %%r15, 72+0%[arr];"
    "movq %%rbp, 80+0%[arr];"
    "movq %%rbx, 88+0%[arr];"
    :"=a"(res), [arr]"=m"(arr)
  );
  ASSERT(484, res);
  ASSERT(0, memcmp(&arr[0], &arr[6], 6 * 8));
}

void bitfield_out() {
  struct S {
   int i:4, j:3, k:5, z:20;
  };
  struct S s;
  s.z = -1;
  struct S *p = &s;
  __asm__ (
    "mov $13, %b2;"
    "mov $6, %b0;"
    "mov $-2, %b1;"
    :"=r"(p->i), "=r"(p->j), "=r"(p->k)
  );
  ASSERT(6, s.i);
  ASSERT(-2, s.j);
  ASSERT(13, s.k);
  ASSERT(-1, s.z);
}

void x87(void) {
  long double f1 = 11, f2 = 22;
  long double *p2 = &f2;
  __asm__ (
    "fxch %1;"
    "fmul %2, %0;"
    :"+t"(f1), "+u"(*p2)
    :"f"(3.0L)
  );
  ASSERT(1, 66 == f1);
  ASSERT(1, 11 == f2);
}

int dialect_skip() {
  int x;
  __asm("{movl $31, %0| junk %} }" : "=r"(x));
  ASSERT(31, x);
  __asm("{|movl $22, %0}" : "=r"(x));
  ASSERT(31, x);
  __asm("{movl $17, %0" : "=r"(x));
  ASSERT(17, x);
  return 1;
}

int main(void) {
  mem_operand_triival();
  reg_label();
  i_constraint(5);
  reg_assign();
  x87();
  x87_clobber();
  exhaust();
  bitfield_out();

  ASSERT(0, ptr_conversion("foo"));
  ASSERT(1, ptr_conversion("bar"));

  ASSERT(101, float_flag(1.0f));
  ASSERT(110, float_flag(0.0f));
  ASSERT(1010, float_flag(0.0f/0.0f));

  ASSERT(11, asm_goto(7));
  ASSERT(22, asm_goto(6));
  ASSERT(33, asm_goto(5));

  ASSERT(13, asm_goto_output(&(int){7}));
  ASSERT(19, asm_goto_output(&(int){6}));
  ASSERT(30, asm_goto_output(&(int){5}));

  ASSERT(11, asm_goto_local_label());
  ASSERT(1, dialect_skip());

  printf("OK\n");
}
#endif
