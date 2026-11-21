// watch: コマンドを繰り返し動かして、画面を更新し続ける（GNU procps の watch と同じ書き方。2026-11-20）
//   watch [-n 秒] [-t] [-d] [-g] [-e] [-x] コマンド [引数...]
//     -n 秒   間隔（既定 2。小数も。0.1 まで）
//     -t      頭の行（Every 2.0s: …）を出さない
//     -d      前の回から変わった文字を反転して出す
//     -g      出力が変わったら終わる
//     -e      コマンドが 0 以外で終わったら、キーが押されるまで止まる
//     -x      引数をシェルに通さず、そのまま渡す（Monocs では、引数を引用して 1 行にする）
// Ctrl-C で終わる。代わりの画面（alternate screen）に出し、終わったら元の画面に戻す。
// コマンドは popen（Monocs の popen は一時ファイルに出力を取ってから読む）。出力は画面の大きさで切る。
// 例: watch -n 1 gpio readall
#define _DEFAULT_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_int(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: watch [-n SECONDS] [-t] [-d] [-g] [-e] [-x] COMMAND [ARG...]\n");
}

// 引数をシェルの 1 行に（-x のとき、それぞれを '...' で引用する）
static void append_arg(char *cmd, size_t size, const char *a, bool quote)
{
    size_t n = strlen(cmd);
    if (n && n + 1 < size)
        cmd[n++] = ' ';
    if (!quote) {
        snprintf(cmd + n, size - n, "%s", a);
        return;
    }
    if (n + 1 < size)
        cmd[n++] = '\'';
    for (; *a && n + 5 < size; a++) {
        if (*a == '\'') {
            memcpy(cmd + n, "'\\''", 4);
            n += 4;
        } else {
            cmd[n++] = *a;
        }
    }
    if (n + 1 < size)
        cmd[n++] = '\'';
    cmd[n] = '\0';
}

// コマンドの出力を buf に（size まで）。終了の状態を *status に
static size_t run(const char *cmd, char *buf, size_t size, int *status)
{
    FILE *p = popen(cmd, "r");
    if (!p) {
        *status = -1;
        return (size_t)snprintf(buf, size, "watch: cannot run: %s\n", cmd);
    }
    size_t n = fread(buf, 1, size - 1, p);
    buf[n] = '\0';
    *status = pclose(p);
    return n;
}

// 画面の 1 行（row 行目、cols 桁まで）を出す。prev があれば、違う文字を反転する
static void put_line(const char *s, size_t len, const char *prev, size_t plen, int cols, bool diff)
{
    int col = 0;
    for (size_t i = 0; i < len && col < cols; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') {
            int next = (col / 8 + 1) * 8;
            while (col < next && col < cols) {
                putchar(' ');
                col++;
            }
            continue;
        }
        bool changed = diff && (i >= plen || prev[i] != s[i]);
        if (changed)
            fputs("\033[7m", stdout);
        putchar(c);
        if (changed)
            fputs("\033[m", stdout);
        // UTF-8 の続きのバイトは桁を進めない（全角は 1 桁と数える: 正確でないが、切りすぎない）
        if ((c & 0xC0) != 0x80)
            col++;
    }
}

int main(int argc, char **argv)
{
    double interval = 2.0;
    bool title = true, diff = false, chgexit = false, errexit = false, exec_mode = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "-n") || !strcmp(a, "--interval")) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            interval = atof(argv[++i]);
        } else if (!strncmp(a, "-n", 2)) {
            interval = atof(a + 2);
        } else if (!strncmp(a, "--interval=", 11)) {
            interval = atof(a + 11);
        } else {
            for (const char *f = a + 1; *f; f++) {
                switch (*f) {
                case 't': title = false; break;
                case 'd': diff = true; break;
                case 'g': chgexit = true; break;
                case 'e': errexit = true; break;
                case 'x': exec_mode = true; break;
                default: usage(); return 1;
                }
            }
        }
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    if (interval < 0.1)
        interval = 0.1;
    char cmd[2048] = "";
    for (; i < argc; i++)
        append_arg(cmd, sizeof cmd, argv[i], exec_mode);

    static char out[65536], prev[65536];
    size_t plen = 0;
    bool first = true;
    signal(SIGINT, on_int);
    signal(SIGTERM, on_int);
    fputs("\033[?1049h\033[?25l", stdout);      // 代わりの画面、カーソルを隠す
    int code = 0;
    while (!stop) {
        int status;
        size_t len = run(cmd, out, sizeof out, &status);
        struct winsize ws = {0};
        int rows = 24, cols = 80;
        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
            rows = ws.ws_row;
            cols = ws.ws_col;
        }
        fputs("\033[H\033[2J", stdout);
        int row = 0;
        if (title) {
            char head[2200], right[160], host[64] = "monocs";
            gethostname(host, sizeof host);
            time_t now = time(nullptr);
            char date[64];
            strftime(date, sizeof date, "%a %b %e %H:%M:%S %Y", localtime(&now));
            snprintf(head, sizeof head, "Every %.1fs: %s", interval, cmd);
            snprintf(right, sizeof right, "%s: %s", host, date);
            int hl = (int)strlen(head), rl = (int)strlen(right);
            if (hl + 1 + rl > cols)
                hl = cols - rl - 1 > 0 ? cols - rl - 1 : 0;
            printf("%.*s%*s%s\n\n", hl, head, cols - hl - rl > 0 ? cols - hl - rl : 0, "", right);
            row = 2;
        }
        // 出力を行に分けて、画面に入る分だけ
        const char *s = out;
        size_t off = 0;
        while (off < len && row < rows) {
            const char *nl = memchr(s + off, '\n', len - off);
            size_t l = nl ? (size_t)(nl - (s + off)) : len - off;
            put_line(s + off, l, first ? nullptr : prev + off, first || off > plen ? 0 : plen - off, cols,
                     diff && !first);
            off += l + (nl != nullptr);
            row++;
            if (row < rows && off < len)
                putchar('\n');
        }
        fflush(stdout);
        bool changed = !first && (len != plen || memcmp(out, prev, len) != 0);
        memcpy(prev, out, len);
        plen = len;
        first = false;
        if (chgexit && changed)
            break;
        if (errexit && status != 0) {
            printf("\033[%d;1H\033[7mcommand exit with a non-zero status, press a key to exit\033[m", rows);
            fflush(stdout);
            getchar();
            code = 8;
            break;
        }
        // 間隔だけ待つ（Ctrl-C で起きる。nanosleep はシグナルで早く戻る）
        struct timespec ts = {(time_t)interval, (long)((interval - (time_t)interval) * 1e9)};
        while (!stop && nanosleep(&ts, &ts) != 0)
            ;
    }
    fputs("\033[?25h\033[?1049l", stdout);      // 元の画面に戻す
    fflush(stdout);
    return code;
}
