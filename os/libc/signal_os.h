// アプリのシグナル（os/libc/signal.c）
#pragma once
#include <stdint.h>

// アプリごとの状態（関数の表、止めているもの、届いて渡していないもの）の控え。posix_app_begin・end が出入りで
// 控えて戻す。中身（struct sigaction など。カーネルは ISO の C23 なので、ほかのファイルからは POSIX の型が
// 見えない）は signal.c だけが扱う。大きさは signal.c の static_assert で確かめる
typedef struct {
    alignas(8) unsigned char bytes[1024];
} SignalState;

void signal_app_begin(SignalState *outer);  // 外側の状態を控えて、新しいアプリの状態（すべて SIG_DFL）にする
void signal_app_end(const SignalState *outer);

// Ctrl-C（raw でないとき）: アプリが SIGINT に関数を登録していれば、届いた印を付けて true（止めない）。
// SIG_IGN なら何もせず true。SIG_DFL なら false（今までどおり止める）。割り込みから呼ぶ
bool signal_on_interrupt_key(void);
void signal_post(int sig);      // 届いた印を付ける（関数があるときだけ。どのコアからでも）
void signal_post_winch(void);   // 画面の大きさが変わった（SIGWINCH。contap.c からは POSIX の名前が見えないので）
// シェルの timeout の期限（os/kernel/app.c）: 今のアプリに sig を届ける。既定の動きで終わるなら true（呼んだ側が
// 止める）。関数があれば届いた印を付け、無視なら何もせず false
bool signal_send_timeout(int sig);
#define SIGNAL_KILL 9   // SIGKILL（app.c からは POSIX の名前が見えないので。signal.c の static_assert で確かめる）

// 渡す場所: コア 0 が待つところ（割り込みが入る状態で）から呼ぶ。アプリが動いていて、カーネルのロックを
// 持っていなければ、届いたものの関数を呼ぶ
void signal_point(void);
uint32_t signal_delivered(void);    // 関数を呼んだ回数（nanosleep・select が EINTR で戻るため）
uint32_t signal_delivered_norestart(void);  // そのうち SA_RESTART のない関数の回数（ソケットの待ちが EINTR で戻るため）
