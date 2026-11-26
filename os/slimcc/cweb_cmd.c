// シェルの cweb（2026-11-26。docs/cweb.md の 3）: C の Web のブラウザ。
//   cweb http://HOST/…/index.c[#sha256=…]     ほかの Monocs などから取ってくる（sandbox_fetch.c: HTTP ＋ハッシュ）
//   cweb /root/work/site/index.c[#sha256=…]   手元のファイルから（試すとき）
//   cweb http://HOST/…/game/[#sha256=…]       フォルダのページ（目録 cweb.txt の .c をまとめて。sandbox_fetch.c）
// ページはサンドボックスで動く C のプログラム。cweb_go(URL)（lib/sandbox/cweb.h）で終わると、そのリンクへ移り、
// cweb_back() で 1 つ前へ戻る。Ctrl-C でも 1 つ前へ戻り（戻る先がなければ終わる）、Ctrl-\\ でブラウザごと終わる。
// ふつうに終わる・不正なアクセスなどで止められると、ブラウザも終わる。
// リンクは今のページからの相対（"next.c"）、/ から（同じサーバーか、手元なら絶対パス）、http://… 。.. は使えない。
// "#sha256=…" を付けたリンクは移った先の中身を確かめるので、最初のページのハッシュを信頼すれば、たどる先も確かめられる。
#define _GNU_SOURCE     // strncasecmp（カーネルは ISO C23 なので宣言を見せる）
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "../kernel/sandbox.h"
#include "../kernel/sha256.h"
#include "../drivers/uart.h"

#define URL_MAX     512
#define HISTORY     32

#include "sandbox_page.h"

static bool is_http(const char *u)
{
    return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8);
}

// base（今のページ）から見た rel。だめなら false
static bool resolve(const char *base, const char *rel, char *out)
{
    if (strstr(rel, ".."))
        return fprintf(stderr, "cweb: links cannot use ..: %s\n", rel), false;
    if (is_http(rel) || (rel[0] == '/' && !is_http(base)))
        return snprintf(out, URL_MAX, "%s", rel) < URL_MAX;
    size_t keep;
    if (rel[0] == '/') {                    // 同じサーバーの /…
        const char *host = strstr(base, "://") + 3;
        keep = (size_t)(strchr(host, '/') ? strchr(host, '/') - base : (long)strlen(base));
    } else {                                // 今のページと同じフォルダ
        size_t blen = strcspn(base, "#");
        keep = 0;
        for (size_t i = 0; i < blen; i++)
            if (base[i] == '/')
                keep = i + 1;
    }
    return snprintf(out, URL_MAX, "%.*s%s", (int)keep, base, rel) < URL_MAX;
}

int cweb_cmd(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: cweb http://HOST/…/PAGE.c[#sha256=…] | cweb FILE.c[#sha256=…]\n");
        return 2;
    }
    static char history[HISTORY][URL_MAX];
    static char cur[URL_MAX];
    int depth = 0;
    snprintf(cur, sizeof cur, "%s", argv[1]);
    for (;;) {
        static char files[SANDBOX_MAX_FILES][SANDBOX_PATH_MAX];   // ページのソース（フォルダのページは目録の .c）
        int nf = sandbox_prepare(cur, files, SANDBOX_MAX_FILES);
        if (nf <= 0)
            return 1;
        const char *paths[SANDBOX_MAX_FILES];
        for (int i = 0; i < nf; i++)
            paths[i] = files[i];
        char shown[URL_MAX];                // ページに渡す argv[0]: ハッシュを除いた URL
        snprintf(shown, sizeof shown, "%.*s", (int)strcspn(cur, "#"), cur);
        char *pargv[] = { shown, nullptr };
        enum sandbox_result res;
        int r = sandbox_run_files(paths, nf, 1, pargv, &res);
        bool back = false;
        const char *to = nullptr;
        if (res == SANDBOX_INTERRUPTED && !uart_interrupt_is_quit())
            back = true;                    // Ctrl-C: 1 つ前のページへ（Ctrl-\\ は終わる）
        else if (res != SANDBOX_NAVIGATE)
            return r;
        else
            to = sandbox_nav(&back);
        if (back) {
            if (depth == 0)
                return res == SANDBOX_INTERRUPTED ? 130 : 0;    // 戻る先がない: 終わる
            snprintf(cur, sizeof cur, "%s", history[--depth]);
            continue;
        }
        char next[URL_MAX];
        if (!resolve(cur, to, next))
            return 1;
        if (depth == HISTORY) {             // いちばん古いものを捨てる
            memmove(history[0], history[1], sizeof history[0] * (HISTORY - 1));
            depth--;
        }
        snprintf(history[depth++], URL_MAX, "%s", cur);
        snprintf(cur, sizeof cur, "%s", next);
    }
}
