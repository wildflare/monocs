// 端末の設定（POSIX の termios）と、アプリの stdin の行の扱い（2026-11-12。Linux に合わせる。ユーザーと合意）
// 出典: POSIX.1-2024「General Terminal Interface」（11 章）、Linux の termios(3)・n_tty の既定の動き
//
// アプリの stdin（端末から読むとき。リダイレクト・パイプでファイルのときは関係ない。os/libc/syscalls.c）:
//   - 既定（ICANON・ECHO・ICRNL・ISIG。Linux の端末の既定と同じ）: 打った文字を表示し、Backspace（VERASE、
//     ^H も）・Ctrl-U（VKILL）・Ctrl-W（IEXTEN の単語の消去）で直せる。Enter（'\r' は ICRNL で '\n'）で 1 行が
//     確定してアプリに届く。行の頭の Ctrl-D（VEOF）は EOF、途中なら、そこまでを改行なしで渡す。fgets・scanf が
//     Linux と同じに使える。
//   - ICANON を外すと、キーを 1 つずつ渡す（VMIN・VTIME も Linux と同じ意味）。ECHO を外すと表示しない。
//   - ISIG を外すと Ctrl-C をキーとして渡し、アプリを止めるのは Ctrl-\（curses の raw() と同じ。uart_set_raw）。
//   - 出力の OPOST・ONLCR（'\n' を "\r\n" にする）も効く（syscalls.c の screen_putc が term_onlcr を見る）。
// 端末は 1 つだけ（fd 0・1・2 が端末のとき）。アプリが終わると既定に戻す（termios_reset。os/slimcc/run.c）。
// curses は UART から直接読むので、ここを通らない（os/curses/curses.c）。シェルも自分の行の編集で読む。

#include <errno.h>
#include <string.h>
// struct winsize は Linux と同じ形のもの（os/api/include/sys/termios.h。picolibc の sys/termios.h と同じ印で、こちらを先に）
#include "../api/include/sys/termios.h"
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
// sys/ioctl.h は読まない: picolibc のもの（ioctl の 3 つ目の引数が void *）と、アプリ用に置き換えたもの
// （os/api/include/sys/ioctl.h。Linux と同じく可変長）で ioctl の宣言が違い、セルフホストのビルド（/include の
// アプリ用のヘッダーで、このファイルもコンパイルする）とぶつかるので、要るものだけ書く（値はどちらも同じ）
#include <stdarg.h>
#ifndef TIOCGWINSZ
#define TIOCGWINSZ  (('T' << 8) | 1)
#define TIOCSWINSZ  (('T' << 8) | 2)
#endif
int ioctl(int fd, unsigned long op, ...);
#include "termios_os.h"
#include "socket_os.h"
#include "pipe_os.h"
#include "console.h"

int wcwidth(wchar_t c);    // os/libc/wcwidth.c（picolibc の wchar.h は POSIX のときだけ宣言する）
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../kernel/term.h"

#define CTRL(c)     ((c) & 0x1F)
#define LINE_MAX_T  4096        // 1 行の上限（Linux の n_tty と同じ）

static const struct termios default_tio = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD,
    .c_lflag = ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN,
    .c_cc = {
        [VEOF] = CTRL('D'), [VEOL] = 0, [VERASE] = 0x7F, [VINTR] = CTRL('C'), [VKILL] = CTRL('U'),
        [VMIN] = 1, [VQUIT] = CTRL('\\'), [VSTART] = CTRL('Q'), [VSTOP] = CTRL('S'), [VSUSP] = CTRL('Z'),
        [VTIME] = 0,
    },
    .c_ispeed = B3000000,
    .c_ospeed = B3000000,
};

static struct termios tio = default_tio;
static char line[LINE_MAX_T];
static int line_len, line_pos;
static bool line_eof;           // 行の頭の Ctrl-D（次の読み出しで EOF を 1 回返す）

// 入力の IXON（Ctrl-S で出力を止め、Ctrl-Q で再開する。既定はあり。os/drivers/uart.c が見る）
bool term_ixon(void)
{
    return (tio.c_iflag & IXON) != 0;
}

bool term_onlcr(void)
{
    return (tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
}

// 入れ子のアプリ（system()）: 外側のアプリの端末の設定と、読みかけの行を退避して、子は既定から始める
#define TERMIOS_NEST_MAX 8
static struct {
    struct termios tio;
    char line[LINE_MAX_T];
    int line_len, line_pos;
    bool line_eof;
} saved[TERMIOS_NEST_MAX];
static int nsaved;

bool termios_push(void)
{
    if (nsaved == TERMIOS_NEST_MAX)
        return false;
    saved[nsaved].tio = tio;
    memcpy(saved[nsaved].line, line, sizeof line);
    saved[nsaved].line_len = line_len;
    saved[nsaved].line_pos = line_pos;
    saved[nsaved].line_eof = line_eof;
    nsaved++;
    termios_reset();
    return true;
}

void termios_pop(void)
{
    if (nsaved == 0)
        return;
    nsaved--;
    tio = saved[nsaved].tio;
    memcpy(line, saved[nsaved].line, sizeof line);
    line_len = saved[nsaved].line_len;
    line_pos = saved[nsaved].line_pos;
    line_eof = saved[nsaved].line_eof;
    // ISIG を外していた（Ctrl-C をキーとして読む）なら、そのモードに戻す。curses の raw はこの後の refresh で戻る
    uart_set_raw(!(tio.c_lflag & ISIG));
}

void termios_reset(void)
{
    tio = default_tio;
    line_len = line_pos = 0;
    line_eof = false;
    uart_set_raw(false);
}

// 端末への表示（エコー）。stdout をリダイレクトしていても端末に出す（Linux と同じ）
static void echo_char(char c)
{
    if (c == '\n' && (tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR))
        uart_putc('\r');
    uart_putc(c);
}

static void echo_str(const char *s)
{
    for (; *s; s++)
        echo_char(*s);
}

// 入力の変換（c_iflag）。捨てるなら -1
static int input_map(int c)
{
    if (c == '\r') {
        if (tio.c_iflag & IGNCR)
            return -1;
        if (tio.c_iflag & ICRNL)
            return '\n';
    } else if (c == '\n' && (tio.c_iflag & INLCR)) {
        return '\r';
    }
    if (tio.c_iflag & ISTRIP)
        c &= 0x7F;
    return c;
}

// 表示に使う桁数（制御文字は ^X の 2 桁、全角は 2、結合文字は 0）
static int char_width(const char *p, int n)
{
    unsigned char b = (unsigned char)p[0];
    if (b < 0x20 || b == 0x7F)
        return 2;
    if (b < 0x80)
        return 1;
    wchar_t wc = 0;
    int need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
    if (n < need)
        return 1;
    wc = b & (0x3F >> (need - 1));
    for (int i = 1; i < need; i++)
        wc = (wc << 6) | ((unsigned char)p[i] & 0x3F);
    int w = wcwidth(wc);
    return w < 0 ? 1 : w;
}

// 行の終わりの 1 文字（UTF-8 の続きのバイトも）を消す。消したら true
static bool erase_one(void)
{
    if (line_len == 0)
        return false;
    int start = line_len - 1;
    while (start > 0 && ((unsigned char)line[start] & 0xC0) == 0x80)
        start--;
    int w = char_width(line + start, line_len - start);
    line_len = start;
    if ((tio.c_lflag & ECHO) && (tio.c_lflag & ECHOE))
        for (int i = 0; i < w; i++)
            echo_str("\b \b");
    return true;
}

static void echo_input(char c)
{
    if (!(tio.c_lflag & ECHO)) {
        if (c == '\n' && (tio.c_lflag & ECHONL))
            echo_char('\n');
        return;
    }
    unsigned char b = (unsigned char)c;
    if ((b < 0x20 && c != '\n' && c != '\t') || b == 0x7F) {
        echo_char('^');     // 制御文字は ^X の形で（Linux の ECHOCTL と同じ）
        echo_char(b == 0x7F ? '?' : (char)(b + '@'));
    } else {
        echo_char(c);
    }
}

// 1 行を読んで line に置く（ICANON のとき）
static void read_canonical(void)
{
    line_len = line_pos = 0;
    for (;;) {
        int c = input_map((unsigned char)uart_getc());
        if (c < 0)
            continue;
        const cc_t *cc = tio.c_cc;
        if (c == cc[VEOF] && cc[VEOF]) {
            if (line_len == 0)
                line_eof = true;
            return;         // 途中なら、そこまでを改行なしで渡す
        }
        if ((c == cc[VERASE] && cc[VERASE]) || c == '\b') {
            erase_one();
            continue;
        }
        if (c == cc[VKILL] && cc[VKILL]) {
            while (erase_one())
                ;
            continue;
        }
        if ((tio.c_lflag & IEXTEN) && c == CTRL('W')) {
            while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t'))
                erase_one();
            while (line_len > 0 && line[line_len - 1] != ' ' && line[line_len - 1] != '\t')
                erase_one();
            continue;
        }
        if (c == '\n' || (c == cc[VEOL] && cc[VEOL])) {
            if (line_len < LINE_MAX_T)
                line[line_len++] = (char)c;
            echo_input('\n');
            return;
        }
        if (line_len < LINE_MAX_T - 1) {    // 改行の 1 文字分は残す
            line[line_len++] = (char)c;
            echo_input((char)c);
        }
    }
}

// 標準入力が読めるか（select・poll。os/libc/select.c）: ファイルに向けているとき、読み残した行（ICANON）があるとき、
// 届いた文字があるとき。ICANON のときは、Linux（行が終わるまで読めない）と違い、1 文字届けば読めるとする（read は
// 行が終わるまで待つ）
bool term_input_ready(void)
{
    return console_in_is_file() || line_pos < line_len || line_eof || uart_has_input();
}

// 文字が届くまで、最大 ms ミリ秒待つ（負ならいつまでも）。届いたら true
static bool wait_input(int64_t ms)
{
    uint64_t start = timer_now_us();
    while (!uart_has_input()) {
        if (ms >= 0 && timer_now_us() - start >= (uint64_t)ms * 1000)
            return false;
        uart_wait_input();      // 次の割り込み（文字・タイマー）まで眠る（コア 0）
    }
    return true;
}

int term_read(void *buf, int count)
{
    char *out = buf;
    if (count <= 0)
        return 0;
    if (tio.c_lflag & ICANON) {
        if (line_pos == line_len) {
            if (line_eof) {
                line_eof = false;
                return 0;
            }
            read_canonical();
            if (line_eof && line_len == 0) {
                line_eof = false;
                return 0;
            }
        }
        int n = line_len - line_pos < count ? line_len - line_pos : count;
        memcpy(out, line + line_pos, (size_t)n);
        line_pos += n;
        return n;
    }
    // ICANON なし: VMIN・VTIME（POSIX 11.1.7.1。VTIME は 0.1 秒単位）
    //   VMIN > 0、VTIME = 0: VMIN 文字そろうまで待つ
    //   VMIN > 0、VTIME > 0: 最初の 1 文字は待ち、その後は文字の間が VTIME を超えたら、そこまでを返す
    //   VMIN = 0、VTIME > 0: VTIME だけ待ち、何も来なければ 0
    //   VMIN = 0、VTIME = 0: 待たない（あるだけ返す。なければ 0）
    int vmin = tio.c_cc[VMIN], vtime = tio.c_cc[VTIME];
    int target = vmin < count ? vmin : count;
    int n = 0;
    while (n < count) {
        bool got;
        if (n == 0)
            got = vmin == 0 ? (vtime ? wait_input(vtime * 100) : uart_has_input()) : wait_input(-1);
        else if (n < target)
            got = vtime ? wait_input(vtime * 100) : wait_input(-1);
        else
            got = uart_has_input();     // そろった: 今ある分だけ足して返す
        if (!got)
            break;
        int c = input_map((unsigned char)uart_getc());
        if (c < 0)
            continue;
        out[n++] = (char)c;
        echo_input((char)c);
    }
    return n;
}

int term_getc(void)
{
    unsigned char c;
    return term_read(&c, 1) == 1 ? c : -1;
}

// ---- POSIX の関数 ----

// fd が端末か（0・1・2 で、リダイレクトされていない）
static bool is_term(int fd)
{
    if (isatty(fd))
        return true;
    errno = (fd >= 0 && fd < 3) ? ENOTTY : EBADF;
    return false;
}

static void discard_input(void)
{
    while (uart_has_input())
        uart_getc();
    line_len = line_pos = 0;
    line_eof = false;
}

int tcgetattr(int fd, struct termios *t)
{
    if (!is_term(fd))
        return -1;
    *t = tio;
    return 0;
}

int tcsetattr(int fd, int action, const struct termios *t)
{
    if (!is_term(fd))
        return -1;
    if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    if (action == TCSAFLUSH)
        discard_input();
    tio = *t;
    uart_set_raw(!(tio.c_lflag & ISIG));   // ISIG なし: Ctrl-C はキー、止めるのは Ctrl-\。
    return 0;
}

int tcdrain(int fd)
{
    if (!is_term(fd))
        return -1;
    uart_flush();
    return 0;
}

int tcflush(int fd, int queue)
{
    if (!is_term(fd))
        return -1;
    if (queue == TCIFLUSH || queue == TCIOFLUSH)
        discard_input();
    return 0;
}

int tcflow(int fd, int action)
{
    (void)action;
    return is_term(fd) ? 0 : -1;
}

int tcsendbreak(int fd, int duration)
{
    (void)duration;
    return is_term(fd) ? 0 : -1;
}

pid_t tcgetsid(int fd)
{
    return is_term(fd) ? getpid() : -1;
}

speed_t cfgetispeed(const struct termios *t)
{
    return t->c_ispeed;
}

speed_t cfgetospeed(const struct termios *t)
{
    return t->c_ospeed;
}

int cfsetispeed(struct termios *t, speed_t s)
{
    t->c_ispeed = s;
    return 0;
}

int cfsetospeed(struct termios *t, speed_t s)
{
    t->c_ospeed = s;
    return 0;
}

// 端末の画面の大きさ（os/kernel/term.c。端末に問い合わせる。telnet なら知らされた大きさ）
int tcgetwinsize(int fd, struct winsize *ws)
{
    if (!is_term(fd))
        return -1;
    int rows, cols;
    term_size(&rows, &cols);
    ws->ws_row = (unsigned short)rows;
    ws->ws_col = (unsigned short)cols;
    ws->ws_xpixel = ws->ws_ypixel = 0;  // 画素の大きさは知らない（Linux の多くの端末と同じ）
    return 0;
}

int tcsetwinsize(int fd, const struct winsize *ws)
{
    (void)ws;
    return is_term(fd) ? 0 : -1;    // 端末の大きさはこちらからは変えられない
}

// ioctl: 端末の画面の大きさ（TIOCGWINSZ。Linux のプログラムがよく使う）と、ソケットの FIONBIO・FIONREAD
int ioctl(int fd, unsigned long op, ...)
{
    va_list ap;
    va_start(ap, op);
    void *param = va_arg(ap, void *);   // AArch64 では、可変長の引数も 3 つ目のレジスタで届く
    va_end(ap);
    if (socket_is_fd(fd))
        return socket_ioctl(fd, op, param);     // FIONBIO・FIONREAD（os/libc/socket.c）
    if (pipe_is_fd(fd)) {                       // pipe（os/libc/pipe.c）
        if (op == 0x541B) {                     // FIONREAD（Linux と同じ値。os/api/include/sys/ioctl.h）
            *(int *)param = (int)pipe_available(fd);
            return 0;
        }
        if (op == 0x5421)                       // FIONBIO
            return pipe_set_flags(fd, *(int *)param ? pipe_get_flags(fd) | 0x4000 : 0);  // O_NONBLOCK（picolibc の _FNONBLOCK）
        errno = ENOTTY;
        return -1;
    }
    if (op == TIOCGWINSZ)
        return tcgetwinsize(fd, param);
    if (op == TIOCSWINSZ)
        return tcsetwinsize(fd, param);
    errno = (fd >= 0 && fd < 3) ? EINVAL : ENOTTY;
    return -1;
}
