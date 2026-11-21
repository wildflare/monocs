// monocs_inflate.h: gzip・zlib・deflate で圧縮されたものを展開する。共有のライブラリ /lib/inflate（自作。
// 圧縮する側はない）
//
//   使い方: compile_flags.txt か cc に -linflate。`#include <monocs_inflate.h>`
//   入力を「読む関数」、出力を「書く関数」で渡す（どちらも少しずつでよい。全体をメモリに置かない）:
//
//     static int rd(void *ctx, void *buf, int size) { return (int)fread(buf, 1, size, ctx); }
//     static int wr(void *ctx, const void *buf, int len) { return fwrite(buf, 1, len, ctx) == (size_t)len ? 0 : -1; }
//     const char *err;
//     if (monocs_inflate(MONOCS_INFLATE_GZIP, rd, in, wr, out, &err) != 0) printf("%s\n", err);
//
// 出典: RFC 1951（DEFLATE）、RFC 1952（gzip）、RFC 1950（zlib）
#pragma once

#define MONOCS_INFLATE_RAW    0   // DEFLATE だけ（頭も検査もなし）
#define MONOCS_INFLATE_ZLIB   1   // zlib（2 バイトの頭と Adler-32）
#define MONOCS_INFLATE_GZIP   2   // gzip（.gz。頭、CRC-32、大きさ。続けて並んだ複数の塊も）
#define MONOCS_INFLATE_AUTO   3   // 始めの 2 バイトを見て gzip・zlib・DEFLATE だけ、のどれかを選ぶ（HTTP の Content-Encoding 用）

// 読む関数: buf に size バイトまで読み、読んだバイト数を返す。0 = 終わり、負 = エラー
typedef int (*MonocsInflateRead)(void *ctx, void *buf, int size);
// 書く関数: buf の len バイトを受け取る。0 = 成功、ほか = やめる
typedef int (*MonocsInflateWrite)(void *ctx, const void *buf, int len);

// 展開する。0 = 成功。負 = 失敗（*error に英語の説明。error は NULL 可）
//   -1 入力が壊れている・途中で終わった、-2 読む関数のエラー、-3 書く関数がやめた、-4 メモリが足りない
int monocs_inflate(int format, MonocsInflateRead read, void *read_ctx, MonocsInflateWrite write, void *write_ctx,
                 const char **error);

// CRC-32（gzip・zip・PNG と同じ多項式）。続きから計算するときは前の値を crc に（最初は 0）
unsigned monocs_crc32(unsigned crc, const void *data, unsigned long len);
