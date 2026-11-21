// アプリ（slimcc-a64 でコンパイルしたプログラム）の実行と、シェルへの復帰
#pragma once

#include <stddef.h>
#include <stdint.h>

enum app_result {
    APP_RETURNED,       // main から戻った
    APP_EXITED,         // exit() などで終了した
    APP_FAULTED,        // 同期例外（不正アクセスなど）で止めた
    APP_INTERRUPTED,    // Ctrl-C で止めた
    APP_TIMEDOUT,       // シェルの timeout の期限で止めた
};

typedef int (*app_main_t)(int argc, char **argv);

// entry を呼ぶ。どのように終わっても戻ってくる。終了コードを *status に入れる
enum app_result app_call(app_main_t entry, int argc, char **argv, int *status);

bool app_is_running(void);
void app_current_name(char *buf, size_t size);  // いちばん内側のアプリの名前（argv[0] の最後の部分。なければ ""）

// 入れ子（アプリの中から system() で動かしたアプリ）: 動いているアプリの数と、いちばん内側のアプリの持ち主の番号
// （開いたファイル・接続の持ち主。外側のアプリは 0、その中は -1、-2…。サービスは 1〜。os/kernel/svc.c）
int app_depth(void);
int app_owner(void);

// atexit（アプリごと。32 個まで。0 で成功）と、登録された関数を呼んでから終わる exit（OS API 表の atexit・exit）
int app_atexit(void (*fn)(void));
[[noreturn]] void app_exit_with_handlers(int status);

// アプリを終わらせて app_call から戻る（実行中でなければ false を返す）
bool app_exit(int status);

// 例外ハンドラから呼ぶ。例外から戻る先（ELR_EL1）を app_call への復帰処理に差し替える
void app_abort_from_exception(uint64_t *elr, enum app_result reason);

// Ctrl-C を受けたときに UART から呼ばれる。アプリの実行中なら停止を予約して true を返す
bool app_request_interrupt(void);
bool app_interrupt_pending(void);
// ほかのコア（スレッド）から、アプリ全体を終わらせる・止める（reason は APP_EXITED か APP_FAULTED。status は終了の
// 値。run_atexit なら atexit の関数を呼んでから）。コア 0 が次のタイマー割り込みで止める
void app_request_abort(enum app_result reason, int status, bool run_atexit);
// 止めるときの理由（Ctrl-C なら APP_INTERRUPTED）
enum app_result app_pending_reason(void);

// シェルの timeout（Linux の timeout コマンドと同じ）: 期限（timer_now_us の値）を積む。この後に動かすアプリ
// （と、その中で system() から動かすアプリ）が期限を過ぎて動いていたら、シグナル sig を届ける: アプリが関数を
// 登録していればその関数（コア 0 が待つところで。os/libc/signal.c）、無視なら何もしない、既定（終わる）なら
// タイマー割り込みで止める（Ctrl-C と同じ道筋）。SIGKILL と、kill_after_us（0 = なし）を過ぎたときは必ず止める。
// 入れ子のアプリには、内側から 1 つずつ届ける。8 段まで。積んだ番号（-1 = いっぱい）
int app_timeout_push(uint64_t deadline_us, int sig, uint64_t kill_after_us);
// 外す（push の逆の順に）。期限で止めたら true
bool app_timeout_pop(int slot);
// タイマー割り込みから: 期限を過ぎた timeout の中でアプリが動いているか
bool app_timeout_due(void);
// 今のアプリを、timeout の期限で止める（system() から戻ったとき。割り込みを待たずに）
[[noreturn]] void app_abort_timeout(void);
