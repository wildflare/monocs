// サービス（裏で動き続けるプログラム）。コア 3 で動かす（os/kernel/svc.c）
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SVC_CORE    3
#define SVC_MAX     8

// FILE.c をコンパイルして、サービスとして始める。成功なら 0
int svc_start(const char *path, int argc, char **argv);
// 名前（ファイル名から .c を除いたもの）のサービスを止める。成功なら 0
int svc_stop(const char *name);
// 一覧を表示する
void svc_list(void);
// 起動時に /etc/services.txt（古い SD カードは /services.txt）に書いたサービスを始める
void svc_autostart(void);
// サービスのコアが例外で止まっていたら、後始末をする（シェルが入力を待つ間に呼ぶ）
void svc_check(void);

// ファイルや TCP の接続の持ち主: 今動いているのがサービスなら、その番号 + 1。アプリ・カーネルなら 0、
// system() で入れ子に動かしているアプリは -1、-2…（app_owner。os/kernel/app.c）
int svc_owner(void);
// top: i 番目（0〜）の枠のサービスの名前と service_poll の中にいた時間（cntpct のカウント）。空なら false
bool svc_stat(int i, char *name, size_t size, uint64_t *busy_ticks, uint64_t *polls, uint64_t *work_polls);
uint64_t svc_sleeps(void);     // サービスのコアが眠った回数
