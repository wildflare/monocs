#ifndef _SLIMCC_UNISTD_H
#define _SLIMCC_UNISTD_H

#include_next <unistd.h>

// Monocs（picolibc）: glibc と同じく _GNU_SOURCE のとき environ を宣言する（picolibc は宣言しない。2026-11-20、QuickJS）
#if defined(__MONOCS__) && __GNU_VISIBLE
extern char **environ;
#endif

#endif
