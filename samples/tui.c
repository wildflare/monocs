// 端末の画面を使う例（TUI）。picocom /dev/ttyACM0 などで動かす
//   cc -o tui samples/tui.c && ./tui
// 画面の大きさを端末に問い合わせて枠を描き、矢印キー（または hjkl）で @ を動かす。q で終わる。
// 画面の描き方は ANSI（VT100）のエスケープシーケンス: ESC [ 2 J（消去）、ESC [ 行 ; 列 H（移動）、
// ESC [ 3x m（色）、ESC [ ? 25 l / h（カーソルを隠す・出す）
// キーを 1 つずつ・表示なしで受け取るため、Linux と同じく termios で ICANON と ECHO を外す（終わったら戻す）
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int rows, cols;

static void move(int r, int c) { printf("\x1b[%d;%dH", r, c); }

static void draw_frame(bool known)
{
    printf("\x1b[2J\x1b[36m");
    for (int c = 1; c <= cols; c++) {
        move(1, c); putchar(c == 1 || c == cols ? '+' : '-');
        move(rows, c); putchar(c == 1 || c == cols ? '+' : '-');
    }
    for (int r = 2; r < rows; r++) {
        move(r, 1); putchar('|');
        move(r, cols); putchar('|');
    }
    printf("\x1b[0m");
    move(1, 3);
    printf(" Monocs TUI: %d x %d%s ", cols, rows, known ? "" : " (unknown)");
    move(rows, 3);
    printf(" arrows / hjkl: move   r: resize   q: quit ");
}

// 画面の大きさ（Linux と同じく ioctl の TIOCGWINSZ。わからなければ 24 行 80 桁）
static bool term_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        rows = ws.ws_row;
        cols = ws.ws_col;
        return true;
    }
    rows = 24;
    cols = 80;
    return false;
}

int main(void)
{
    struct termios saved, raw;
    tcgetattr(STDIN_FILENO, &saved);
    raw = saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);  // 1 文字ずつ、表示なし（Ctrl-C はそのまま効く）
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    bool known = term_size();
    int y = rows / 2, x = cols / 2;
    printf("\x1b[?25l");
    draw_frame(known);
    for (;;) {
        move(y, x);
        printf("\x1b[1;33m@\x1b[0m");
        fflush(stdout);
        int ch = getchar();
        move(y, x);
        putchar(' ');
        if (ch == 0x1b && getchar() == '[') {   // 矢印キー: ESC [ A〜D
            switch (getchar()) {
            case 'A': ch = 'k'; break;
            case 'B': ch = 'j'; break;
            case 'C': ch = 'l'; break;
            case 'D': ch = 'h'; break;
            }
        }
        switch (ch) {
        case 'k': if (y > 2) y--; break;
        case 'j': if (y < rows - 1) y++; break;
        case 'l': if (x < cols - 1) x++; break;
        case 'h': if (x > 2) x--; break;
        case 'r':   // 端末の大きさを変えた後に、問い合わせ直す
            known = term_size();
            if (y > rows - 1) y = rows - 1;
            if (x > cols - 1) x = cols - 1;
            draw_frame(known);
            break;
        case 'q':
            printf("\x1b[2J\x1b[H\x1b[?25h");
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            return 0;
        }
    }
}
