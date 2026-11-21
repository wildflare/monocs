// 命令キャッシュとデータキャッシュの整合を取る（libgcc の __clear_cache の代わり）
//
// メモリに書いた命令を実行する前に呼ぶ（cc/memlink.c）。[begin, end) のデータキャッシュの行を
// 一致点（PoU）まで書き戻し、命令キャッシュの行を無効にする。
// 出典: ARM DDI 0487 D7.4（CTR_EL0: DminLine・IminLine・IDC・DIC）、B2.4.4（命令の変更の手順）

#include <stdint.h>

void __clear_cache(void *begin, void *end) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  uintptr_t b = (uintptr_t)begin, e = (uintptr_t)end;

  // IDC（ビット 28）: データキャッシュの書き戻しが不要
  if (!(ctr & (1ULL << 28))) {
    uintptr_t line = 4ULL << ((ctr >> 16) & 0xF); // DminLine（語数の 2 の対数）
    for (uintptr_t a = b & ~(line - 1); a < e; a += line)
      __asm__ volatile("dc cvau, %0" : : "r"(a) : "memory");
  }
  __asm__ volatile("dsb ish" : : : "memory");

  // DIC（ビット 29）: 命令キャッシュの無効化が不要
  if (!(ctr & (1ULL << 29))) {
    uintptr_t line = 4ULL << (ctr & 0xF); // IminLine
    for (uintptr_t a = b & ~(line - 1); a < e; a += line)
      __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
  }
  __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}
