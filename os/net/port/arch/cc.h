// lwIP の移植層: コンパイラと C ライブラリ（picolibc）について
// 出典: lwIP の src/include/lwip/arch.h（ここで定義するもの）、doc/NO_SYS_SampleCode.c
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define BYTE_ORDER  LITTLE_ENDIAN

// 診断の表示と、表明（assert）が外れたとき（止めずに表示だけする。割り込みの中のこともあるため）
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwip: assertion \"%s\" failed at %s:%d\n", x, \
                                            __FILE__, __LINE__); } while (0)
