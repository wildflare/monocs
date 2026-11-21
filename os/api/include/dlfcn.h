// 共有ライブラリを読み込む関数（Linux の glibc と同じ名前・値。os/libc/posix.c）
// Monocs には共有ライブラリ（.so）がない（ライブラリはソースのまま /usr/lib に置き、-l名前 で一緒にコンパイルする）。
// dlopen はいつも NULL を返し、理由を dlerror で返す（Linux のプログラムが、読み込みに失敗したときの道で動く）
#ifndef _DLFCN_H
#define _DLFCN_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY     0x00001
#define RTLD_NOW      0x00002
#define RTLD_NOLOAD   0x00004
#define RTLD_DEEPBIND 0x00008
#define RTLD_GLOBAL   0x00100
#define RTLD_LOCAL    0
#define RTLD_NODELETE 0x01000
#define RTLD_NEXT     ((void *)-1l)
#define RTLD_DEFAULT  ((void *)0)

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
