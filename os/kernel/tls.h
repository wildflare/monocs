// コアごとの TLS（_Thread_local の変数）
#pragma once

// コア core の TLS を初期値で用意し、TPIDR_EL0 に設定する（各コアで、MMU を有効にした後に呼ぶ）
void tls_init(int core);
