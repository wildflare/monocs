// サンドボックスの入口（os/kernel/sandbox.c が EL0 でここから始める。x0 = struct sandbox_info *）
#include <stdio.h>
#include <stdlib.h>
#include "sandbox_sys.h"

int main(int argc, char **argv);

[[noreturn]] void __sandbox_start(struct sandbox_info *info)
{
    __sandbox_heap_init(info->heap_start, info->heap_end);
    exit(main(info->argc, info->argv));
}
