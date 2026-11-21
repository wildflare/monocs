// アプリ用のヒープ（os/libc/malloc.c）
#pragma once
#include <stddef.h>

// アプリを始める: 以後の確保はアプリ用のヒープから（コア 0〜2、サービスを除く）。アプリの実行中に呼ぶと入れ子
// （system() で動かす子のアプリ）: 外側のヒープを退避して、子の領域を作る。入れ子が深すぎれば false
bool malloc_app_begin(void);
void malloc_app_stop(void);     // アプリが終わった: 以後の確保はカーネル用から（アプリ用のものの解放は受け付ける）
size_t malloc_app_discard(void);    // アプリ用のヒープを丸ごと捨てる（入れ子なら子の分だけ捨てて、外側に戻す）。free されなかった量（見出しを含む）

// この間の確保は（アプリの実行中でも）カーネル用から取る。アプリが終わった後まで残すもの用。コアごと
void malloc_kernel_begin(void);
void malloc_kernel_end(void);
void malloc_kernel_reset(void); // このコアの深さを 0 に（中断された仕事の後など）

// 最後に終わったアプリのヒープの大きさと、free されないまま捨てた量（見出しを含む）
void malloc_last_app(size_t *arena, size_t *leaked);
// カーネル用・アプリ用のヒープの大きさと、使っている量（top）
void malloc_heap_info(size_t *k_arena, size_t *k_in_use, size_t *a_arena, size_t *a_in_use);
void malloc_peak(size_t *k_peak, size_t *a_peak, size_t *total, bool reset);   // mem の peak（reset で今に）
void malloc_out_of_memory(size_t n, bool app, size_t k_in_use, size_t a_in_use);  // 確保の失敗（カーネル: memory.c）
