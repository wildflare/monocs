// アプリの環境変数（os/libc/env.c）
#pragma once

void env_app_begin(void);   // アプリを始める（posix_app_begin から）
void env_app_end(void);     // アプリが終わった: アプリが変えた環境変数を、始めたときのものに戻す
void env_snapshot_now(void);    // 今の段の控えを、まだなければ今すぐ取る（system() の行の export を戻すため）

// アプリから呼ぶ setenv・unsetenv・putenv（OS API 表）
int app_setenv(const char *name, const char *value, int overwrite);
int app_unsetenv(const char *name);
int app_putenv(char *string);
