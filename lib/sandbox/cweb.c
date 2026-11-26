// C の Web のリンク（cweb.h）。SB_SYS_GO でページを終える（戻ってこない）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cweb.h"
#include "sandbox_sys.h"

[[noreturn]] void cweb_go(const char *url)
{
    fflush(stdout);
    sb_sys(SB_SYS_GO, (long)url, (long)strlen(url), 0);
    fprintf(stderr, "cweb_go: bad link: %s\n", url);     // 戻ってきた: 長すぎる・おかしい URL
    exit(1);
}

[[noreturn]] void cweb_back(void)
{
    fflush(stdout);
    sb_sys(SB_SYS_GO, 0, 0, 1);
    exit(1);
}
