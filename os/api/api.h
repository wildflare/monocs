// OS API の関数表（名前 → アドレス）
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct os_api_entry {
    const char *name;
    const void *addr;   // tls なら、このコアでのアドレスを返す関数（const void *(*)(void)）
    bool tls;
};

extern const struct os_api_entry os_api_table[];
extern const size_t os_api_count;
