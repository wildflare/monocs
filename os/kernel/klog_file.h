// カーネルの記録を SD カードの /var/log/dmesg にも残す（os/kernel/klog_file.c）
#pragma once
#include <stdbool.h>

void klog_file_boot(void);          // 起動のとき（SD カードが使えるようになった後、シェルの最初のプロンプトの前）
void klog_file_sync(bool force);    // 増えた分を書き足す（force でなければ 5 秒に 1 回まで）
