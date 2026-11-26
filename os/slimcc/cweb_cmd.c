// シェルの cweb（2026-11-26。docs/cweb.md の 3）: C の Web のブラウザ。
//   cweb http://HOST/…/index.c[#sha256=…]     ほかの Monocs などから取ってくる（sandbox_fetch.c: HTTP ＋ハッシュ）
//   cweb /root/work/site/index.c[#sha256=…]   手元のファイルから（試すとき）
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

bool sandbox_fetch(const char *url, char *out, size_t out_size);
int sandbox_run_file(const char *path, int argc, char **argv, enum sandbox_result *res);

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

// 手元のファイルのページ: #sha256= があれば確かめる
static bool local_page(const char *url, char *path, size_t size)
{
    const char *hash = strchr(url, '#');
    snprintf(path, size, "%.*s", hash ? (int)(hash - url) : (int)strlen(url), url);
    if (!hash)
        return true;
    if (strncmp(hash, "#sha256=", 8) || strlen(hash + 8) != 64)
        return fprintf(stderr, "cweb: write the hash as #sha256= and 64 hex digits\n"), false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return fprintf(stderr, "cweb: %s: cannot open\n", path), false;
    static char buf[4u << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    char got[65];
    sha256_hex(buf, n, got);
    if (strncasecmp(got, hash + 8, 64))
        return fprintf(stderr, "cweb: hash mismatch: %s\n  got %s\n", path, got), false;
    return true;
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
        char path[300];
        if (is_http(cur) ? !sandbox_fetch(cur, path, sizeof path) : !local_page(cur, path, sizeof path))
            return 1;
        char shown[URL_MAX];                // ページに渡す argv[0]: ハッシュを除いた URL
        snprintf(shown, sizeof shown, "%.*s", (int)strcspn(cur, "#"), cur);
        char *pargv[] = { shown, nullptr };
        enum sandbox_result res;
        int r = sandbox_run_file(path, 1, pargv, &res);
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
