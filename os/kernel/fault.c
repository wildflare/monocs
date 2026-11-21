// アプリが止まった場所の記録（os/kernel/fault.h）
#include "fault.h"
#include "exception.h"
#include "memory.h"
#include "sysreg.h"

static FaultRecord rec;

void fault_clear(void)
{
    rec.valid = false;
}

void fault_record(const struct trap_frame *f)
{
    if (rec.valid)
        return;
    rec.core = cpu_id();
    rec.pc = f->elr;
    rec.lr = f->x[30];
    rec.nframes = 0;
    // フレームポインタをたどる（[fp] = 前の fp、[fp + 8] = 戻り先）。RAM の中で、8 バイトにそろい、上に向かうときだけ
    uint64_t fp = f->x[29];
    while (rec.nframes < FAULT_FRAMES && fp >= 0x80000 && fp < memory_heap_end() && !(fp & 7)) {
        const uint64_t *frame = (const uint64_t *)fp;
        rec.frames[rec.nframes++] = frame[1];
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    rec.valid = true;
}

const FaultRecord *fault_get(void)
{
    return rec.valid ? &rec : nullptr;
}
