// SD カードへの書き込みを遅らせる（ライトバック。os/fs/diskio.c）
#pragma once

#include <stdint.h>

// まだ書いていないセクタを書き出す。force でなければ、最初の書き込みから 1 秒たっているときだけ。
// VFS のロックの中で呼ぶ（ふつうは vfs_sync を使う）
bool diskio_writeback(bool force);
// まだ書いていないセクタと、読み出しのキャッシュを捨てる（SD カードを直接書き換えた後に。すぐ再起動すること）
void diskio_discard(void);
// まだ書いていないセクタの数、書き出した回数、表の中で書き直した回数
void diskio_writeback_info(unsigned *dirty, uint64_t *flushes, uint64_t *rewrites);
