// 割り込みの振り分け（ARM ローカル割り込みコントローラ）
#pragma once

void irq_init(void);
void irq_dispatch(void);
