#include "test.h"

#ifdef __aarch64__
// slimcc-a64 のフレーム（x29 と、[x29 - 16] に退避した x19・x20）を畳んで戻る
char *asm_fn1(void) {
  __asm__("mov x0, #50\n\t"
          "ldp x19, x20, [x29, #-16]\n\t"
          "mov sp, x29\n\t"
          "ldp x29, x30, [sp], #16\n\t"
          "ret");
}

char *asm_fn2(void) {
  __asm__ inline volatile("mov x0, #55\n\t"
                          "ldp x19, x20, [x29, #-16]\n\t"
                          "mov sp, x29\n\t"
                          "ldp x29, x30, [sp], #16\n\t"
                          "ret");
}
#else
char *asm_fn1(void) {
  __asm__("mov $50, %rax\n\t"
          "mov %rbp, %rsp\n\t"
          "pop %rbp\n\t"
          "ret");
}

char *asm_fn2(void) {
  __asm__ inline volatile("mov $55, %rax\n\t"
                          "mov %rbp, %rsp\n\t"
                          "pop %rbp\n\t"
                          "ret");
}

#endif

int main() {
  ASSERT(50, asm_fn1());
  ASSERT(55, asm_fn2());

  printf("OK\n");
  return 0;
}
