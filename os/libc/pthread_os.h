// スレッド（os/libc/pthread.c）のカーネルの側
#pragma once

// アプリ（入れ子の深さ level。app_depth）が終わった: そのアプリと中のアプリが作ったスレッドの記録を消す
void pthread_app_end(int level);
