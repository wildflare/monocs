// SHA-256（FIPS 180-4）。サンドボックスで取ってきたソースの中身を確かめる（os/slimcc/sandbox_cmd.c）
#pragma once
#include <stddef.h>
#include <stdint.h>
void sha256(const void *data, size_t len, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);   // 小文字の 16 進 64 文字
