// アプリが止まった場所の記録（例外・Ctrl-C。os/kernel/fault.c）。表示は、アプリが終わった後で os/slimcc/run.c が
// 関数名に直して出す（例外の中ではファイルを読めず、アプリの記号表もまだある）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FAULT_FRAMES 10

struct trap_frame;

typedef struct {
    bool valid;
    int core;               // 止まったコア
    uint64_t pc;            // 止まった命令
    uint64_t lr;            // x30（葉の関数なら呼び出し元。フレームにまだ積んでいない）
    uint64_t frames[FAULT_FRAMES];   // フレームポインタ（x29）をたどった戻り先
    int nframes;
} FaultRecord;

void fault_clear(void);
// 記録する（もう記録があれば何もしない。最初に止まったところを残す）
void fault_record(const struct trap_frame *f);
const FaultRecord *fault_get(void);
