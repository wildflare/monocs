// カーネルの関数名（os/kernel/ksym.c）
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 今のカーネルの表を読む（読んであれば何もしない）。なければ・違うカーネルのものなら false（理由を why に）
uint64_t ksym_kernel_id(void);
bool ksym_load(const char **why);
// addr を含む関数の名前（なければ NULL）。off に関数の頭からの距離
const char *ksym_at(uint64_t addr);
const char *ksym_at_off(uint64_t addr, uint64_t *off);
