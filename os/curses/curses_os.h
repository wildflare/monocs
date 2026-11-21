// curses（os/curses/curses.c）のカーネル側の入口
#pragma once

// アプリが終わったあとに呼ぶ（run.c）。endwin を呼ばずに終わっていたら端末を元に戻し、状態を捨てる
void curses_app_exit(void);

// 例外・Ctrl-\ でアプリを止めるとき、表示の前に呼ぶ（例外ハンドラの中から。uart_puts だけを使う）。
// 代わりの画面（?1049h）から戻して、止めた理由が元の画面に残るようにする
void curses_emergency_restore(void);

// 入れ子のアプリ（system()）の前後: 外側のアプリの curses の状態を退避して、子を空の状態から始める・戻す
bool curses_push(void);
void curses_pop(void);
