// monocs_sysstat.h: システムの様子（monocs_sysstat。/bin/top が使う。os/kernel/sysstat.c）
#pragma once

#include <stdint.h>

#define MONOCS_STAT_CORES     4
#define MONOCS_STAT_SERVICES  8

// コアの様子
enum {
    MONOCS_CORE_OFF,          // 起きていない
    MONOCS_CORE_IDLE,         // 仕事を待っている
    MONOCS_CORE_SHELL,        // コア 0: シェル（コマンドを待っている・組み込みのコマンド）
    MONOCS_CORE_APP,          // コア 0: アプリ
    MONOCS_CORE_JOB,          // コア 1〜3: アプリが頼んだ仕事（monocs_core_start・OpenMP）
    MONOCS_CORE_THREAD,       // コア 1〜3: pthread のスレッド
    MONOCS_CORE_SERVICES,     // サービスのコア（コア 3）
};

typedef struct {
    uint64_t uptime_us;
    struct {
        int state;              // MONOCS_CORE_*
        uint64_t idle_us;       // 起動からの、眠っていた（仕事を待っていた）時間の合計
        uint64_t irqs;          // 受けた割り込みの数
    } core[MONOCS_STAT_CORES];
    char app[48];               // コア 0 のアプリ（入れ子ならいちばん内側。"" ならシェル）
    // メモリ（バイト）
    uint64_t mem_total;         // ヒープの領域全体（カーネル用とアプリ用で分け合う）
    uint64_t kernel_heap;       // カーネル用のヒープの大きさ（sbrk）
    uint64_t kernel_in_use;     // そのうち使っている量
    uint64_t app_heap;          // アプリ用のヒープの大きさ（アプリが終わると 0 に戻る）
    uint64_t app_in_use;
    uint64_t file_cache;        // ファイルのキャッシュ（カーネル用のヒープの中）
    // サービス
    int nservices;
    struct {
        char name[32];
        uint64_t busy_us;       // service_poll の中にいた時間の合計
        uint64_t polls;         // service_poll を呼んだ回数
        uint64_t work_polls;    // そのうち「仕事をした」（0 以外）を返した回数
    } service[MONOCS_STAT_SERVICES];
    uint64_t svc_sleeps;        // サービスのコアが眠った（wfe）回数
    // 温度（1/1000 ℃）・ARM のクロック（Hz）・電源の様子（GET_THROTTLED。0 = 問題なし）。読めなければ 0
    int32_t temp_mc;
    uint32_t arm_hz;
    uint32_t throttled;
} MonocsSysStat;

void monocs_sysstat(MonocsSysStat *st);
