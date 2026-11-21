// glob（パターンに合うパスの一覧。POSIX.1-2008 と、GNU の拡張の一部）。Monocs の実装は os/libc/glob.c。
// picolibc の glob.h は BSD 版（定数の値・名前が違い、本体もない）なので、Linux（glibc）と同じ形のものに置き換える。
// 構造体の並び・定数の値は glibc 2.41 の <glob.h> と同じ（Linux 用のソースがそのままコンパイルできるように）。
#ifndef _GLOB_H
#define _GLOB_H 1
#define _GLOB_H_    // picolibc の glob.h を読まないように

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// glob の flags（POSIX）
#define GLOB_ERR        (1 << 0)    // ディレクトリが読めなければ止める
#define GLOB_MARK       (1 << 1)    // ディレクトリの名前の後ろに / を付ける
#define GLOB_NOSORT     (1 << 2)    // 並べない
#define GLOB_DOOFFS     (1 << 3)    // gl_pathv の先頭に gl_offs 個の NULL を置く
#define GLOB_NOCHECK    (1 << 4)    // 合うものがなければ、パターンそのものを返す
#define GLOB_APPEND     (1 << 5)    // 前の結果に足す
#define GLOB_NOESCAPE   (1 << 6)    // \ を特別な文字として扱わない
#define GLOB_PERIOD     (1 << 7)    // 先頭の . にも * などを合わせる（GNU）
// GNU の拡張
#define GLOB_MAGCHAR    (1 << 8)    // （結果の gl_flags に立つ）パターンに * ? [ があった
#define GLOB_ALTDIRFUNC (1 << 9)    // gl_opendir などを使う（Monocs では使わない。いつもふつうのファイルの関数）
#define GLOB_BRACE      (1 << 10)   // {a,b} を展開する
#define GLOB_NOMAGIC    (1 << 11)   // * ? [ がなければ、ないパスでもパターンを返す
#define GLOB_TILDE      (1 << 12)   // 先頭の ~ を HOME にする
#define GLOB_ONLYDIR    (1 << 13)   // ディレクトリだけ（ほかのものが入ることもある。glibc と同じく目安）
#define GLOB_TILDE_CHECK (1 << 14)  // ~ を展開できなければ GLOB_NOMATCH

// 戻り値
#define GLOB_NOSPACE    1   // メモリが足りない
#define GLOB_ABORTED    2   // 読めないディレクトリで止めた（GLOB_ERR か errfunc）
#define GLOB_NOMATCH    3   // 合うものがない
#define GLOB_NOSYS      4   // 使えない
#define GLOB_ABEND      GLOB_ABORTED

struct stat;
struct dirent;

typedef struct {
    size_t gl_pathc;        // 合ったパスの数
    char **gl_pathv;        // パスの並び（NULL で終わる）
    size_t gl_offs;         // 先頭に置く NULL の数（GLOB_DOOFFS）
    int gl_flags;
    // GLOB_ALTDIRFUNC 用（Monocs では使わない。glibc と同じ大きさにするため）
    void (*gl_closedir)(void *);
    struct dirent *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *__restrict, struct stat *__restrict);
    int (*gl_stat)(const char *__restrict, struct stat *__restrict);
} glob_t;

int glob(const char *__restrict pattern, int flags, int (*errfunc)(const char *epath, int eerrno),
         glob_t *__restrict pglob);
void globfree(glob_t *pglob);

#ifdef __cplusplus
}
#endif

#endif
