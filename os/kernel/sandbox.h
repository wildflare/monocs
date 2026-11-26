// サンドボックス（2026-11-26。docs/sandbox.md）: 信用できないプログラムを EL0 で動かす
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "exception.h"

enum sandbox_result {
    SANDBOX_EXITED,         // SB_SYS_EXIT（終わる。終了の値）
    SANDBOX_FAULTED,        // EL0 からの不正なアクセス・命令、知らないシステムコール
    SANDBOX_INTERRUPTED,    // Ctrl-C
    SANDBOX_TIMEDOUT,       // 時間の上限
    SANDBOX_NOT_STARTED,    // 入れなかった（メモリ・コア 0 以外・入れ子・アプリの実行中）
};

// 領域: 頭の SANDBOX_IMAGE_SIZE がプログラム（機械語か、つないだプログラム）、その後の SANDBOX_DATA_SIZE が
// データ（頭に struct sandbox_info と引数、残りがヒープ）とスタック（上端から）
#define SANDBOX_IMAGE_SIZE  (4UL << 20)
#define SANDBOX_DATA_SIZE   (8UL << 20)

uint8_t *sandbox_region_alloc(void);       // 2MB の境界にそろえた領域（0 で埋める）。だめなら NULL
void sandbox_region_free(uint8_t *region);
// region の中の entry から EL0 で動かし、終わるまで待つ。writable_image: プログラムの部分も EL0 から書ける
// （つないだプログラムはデータもその中にある）か、実行と読み出しだけか。timeout_ms が 0 なら上限なし
enum sandbox_result sandbox_enter(uint8_t *region, uint64_t entry, bool writable_image, int argc, char **argv,
                                  uint32_t timeout_ms, int64_t *status);
// 機械語の並びを動かす（試し用。os/shell/shell.c の sbtest）
enum sandbox_result sandbox_run(const uint32_t *code, size_t ncode, uint32_t timeout_ms, int64_t *status);

// os/kernel/exception.c から（下位 EL からの例外）
void sandbox_handle_sync(struct trap_frame *f);
void sandbox_after_irq(void);
