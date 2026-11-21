// softuart: ソフトウェアの UART で機器とやりとりする（Monocs のコマンド。/lib/softuart）
//   softuart [-b BAUD] TX RX               端末のように使う（打った文字を送り、受けたものを出す）。Ctrl-] で終わる
//   softuart [-b BAUD] -x TEXT [-w MS] TX RX   TEXT を送り、MS ミリ秒（既定 500）の間に受けたものを出して終わる
// TX・RX は GPIO の番号（-1 で使わない）。BAUD の既定は 9600。TEXT の \r \n \t \\ \xNN は文字に直す。
// 受けたもののうち、表示できない文字（改行・タブ以外の制御文字）は \xNN で出す（-x のとき）。
// picocom・minicom の代わりの、Monocs のもの。
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <monocs_softuart.h>

static void usage(void)
{
    fprintf(stderr, "Usage: softuart [-b BAUD] TX RX                 (terminal; Ctrl-] to quit)\n"
                    "       softuart [-b BAUD] -x TEXT [-w MS] TX RX  (send TEXT, print what comes back for MS ms)\n"
                    "  TX, RX: GPIO numbers (-1: not used). BAUD 300-115200 (default 9600). 8N1.\n");
}

static int unescape(const char *s, char *out)
{
    int n = 0;
    while (*s) {
        char c = *s++;
        if (c == '\\' && *s) {
            char e = *s++;
            if (e == 'r')
                c = '\r';
            else if (e == 'n')
                c = '\n';
            else if (e == 't')
                c = '\t';
            else if (e == 'x' && s[0] && s[1]) {
                char h[3] = {s[0], s[1], 0};
                c = (char)strtol(h, nullptr, 16);
                s += 2;
            } else
                c = e;
        }
        out[n++] = c;
    }
    return n;
}

int main(int argc, char **argv)
{
    long baud = 9600, wait_ms = 500;
    const char *text = nullptr;
    int i = 1;
    for (; i < argc; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-b") && i + 1 < argc)
            baud = atol(argv[++i]);
        else if (!strcmp(o, "-x") && i + 1 < argc)
            text = argv[++i];
        else if (!strcmp(o, "-w") && i + 1 < argc)
            wait_ms = atol(argv[++i]);
        else if (!strcmp(o, "-h") || !strcmp(o, "--help")) {
            usage();
            return 0;
        } else
            break;      // TX（-1 もある）から後は番号
    }
    if (argc - i != 2) {
        usage();
        return 1;
    }
    int tx = atoi(argv[i]), rx = atoi(argv[i + 1]);
    int u = softuart_open(tx, rx, (int)baud);
    if (u < 0) {
        perror("softuart: open");
        return 1;
    }
    if (text) {
        char *buf = malloc(strlen(text) + 1);
        int n = unescape(text, buf);
        if (tx >= 0)
            softuart_write(u, buf, n);
        if (rx >= 0) {
            char r[256];
            int got;
            // 最初の 1 バイトまでは wait_ms、その後は 50ms 途切れたら終わり
            int t = (int)wait_ms;
            while ((got = softuart_read(u, r, sizeof r, t)) > 0) {
                for (int k = 0; k < got; k++) {
                    unsigned char c = (unsigned char)r[k];
                    if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f))
                        putchar(c);
                    else if (c != '\r')
                        printf("\\x%02x", c);
                }
                fflush(stdout);
                t = 50;
            }
            putchar('\n');
        }
        unsigned e = softuart_errors(u);
        if (e)
            fprintf(stderr, "softuart: %u receive errors\n", e);
        softuart_close(u);
        return 0;
    }
    // 端末: 1 文字ずつ、エコーなし、Ctrl-C もそのまま送る
    struct termios old, raw;
    tcgetattr(0, &old);
    raw = old;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    fprintf(stderr, "softuart: TX GPIO%d, RX GPIO%d, %ld bps, 8N1 (rx on %s). Ctrl-] to quit\r\n", tx, rx, baud,
            softuart_rx_background(u) ? "another core" : "this core");
    for (;;) {
        struct pollfd pf = {.fd = 0, .events = POLLIN};
        if (poll(&pf, 1, 0) > 0) {
            char c;
            if (read(0, &c, 1) == 1) {
                if (c == 0x1d)      // Ctrl-]
                    break;
                if (tx >= 0)
                    softuart_write(u, &c, 1);
            }
        }
        if (rx >= 0) {
            char r[256];
            int got = softuart_read(u, r, sizeof r, softuart_rx_background(u) ? 0 : 5);
            if (got > 0) {
                fwrite(r, 1, (size_t)got, stdout);
                fflush(stdout);
            }
        }
    }
    tcsetattr(0, TCSANOW, &old);
    fprintf(stderr, "\r\n");
    unsigned e = softuart_errors(u);
    if (e)
        fprintf(stderr, "softuart: %u receive errors\n", e);
    softuart_close(u);
    return 0;
}
