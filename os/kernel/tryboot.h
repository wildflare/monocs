// 新しいカーネルを試しに起動して、だめなら前のカーネルに戻る（os/kernel/tryboot.c）
#pragma once

#include <stdbool.h>
#include <stdint.h>

void tryboot_init(void);        // 起動の早いうちに（試しの起動なら、ウォッチドッグを入れる）
void tryboot_tick(void);        // タイマー割り込みから（ウォッチドッグに餌をやる）
bool tryboot_active(void);      // 試しの起動で、まだ確認されていない
bool tryboot_confirm(void);     // 確認する（ウォッチドッグを止める）。試しの起動でなければ false
bool tryboot_confirmed(void);   // 試しの起動で、確認された
int tryboot_seconds_left(void); // 確認されないまま、あと何秒で前のカーネルに戻るか
const char *tryboot_cmdline(void);  // ファームウェアから受け取ったコマンドライン
