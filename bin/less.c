// less [FILE]: ファイルを 1 画面ずつ見る（curses）。ファイルがなければ stdin（パイプ: ls -l | less）。
// Monocs に付いてくるアプリ（/bin）（less で動く）。長い行は折り返す（全角・タブも幅で数える）。
//
//   Space f PgDn Ctrl-F  次の画面      b PgUp Ctrl-B  前の画面      d u  半画面
//   j ↓ Enter e          1 行下        k ↑ y          1 行上        g Home <  先頭    G End >  最後
//   /文字  前へ探す       ?文字  後ろへ探す    n  次    N  逆向き（見つかったところは反転）
//   Ctrl-L  描き直す      q  終わり
// wcwidth（XSI）の宣言のため
#define _XOPEN_SOURCE 700
#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

static char *text;          // ファイルの中身
static size_t text_len;
static size_t *line_start;  // 行の始まり（最後に text_len を足してある）
static long nlines;
static bool start_at_end;     // +G

typedef struct {
    long line;              // 何行目か
    size_t from, to;        // その行の中のバイトの範囲（text の位置）
} Row;

static Row *rows;           // 画面の行（折り返した後）
static long nrows, caprows;
static int cols_for_rows;   // rows を作ったときの桁数

static const char *name;
static char pattern[256];
static bool search_back;

static int char_len(unsigned char c)
{
    return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

// text[p] から始まる 1 文字の表示幅（col はタブのための今の桁）
static int char_width(size_t p, int col, int *len)
{
    unsigned char c = (unsigned char)text[p];
    *len = char_len(c);
    if (p + (size_t)*len > text_len)
        *len = 1;
    if (c == '\t')
        return 8 - col % 8;
    if (c < 0x20 || c == 0x7F)
        return 2;   // ^X の形で出る
    if (c < 0x80)
        return 1;
    unsigned cp = c & (0x3F >> (*len - 1));
    for (int k = 1; k < *len; k++)
        cp = cp << 6 | ((unsigned char)text[p + k] & 0x3F);
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

static void add_row(long line, size_t from, size_t to)
{
    if (nrows == caprows) {
        caprows = caprows ? caprows * 2 : 1024;
        rows = realloc(rows, sizeof(Row) * (size_t)caprows);
    }
    rows[nrows++] = (Row){line, from, to};
}

// 行を、今の桁数で画面の行に分ける
static void build_rows(void)
{
    nrows = 0;
    cols_for_rows = COLS;
    for (long l = 0; l < nlines; l++) {
        size_t p = line_start[l], end = line_start[l + 1];
        if (end > p && text[end - 1] == '\n')
            end--;
        if (end > p && text[end - 1] == '\r')
            end--;
        size_t from = p;
        int col = 0;
        while (p < end) {
            int len, w = char_width(p, col, &len);
            if (col + w > COLS && col > 0) {
                add_row(l, from, p);
                from = p;
                col = 0;
                continue;
            }
            col += w;
            p += (size_t)len;
        }
        add_row(l, from, end);
    }
}

static void load(FILE *f)
{
    size_t cap = 64 * 1024;
    text = malloc(cap);
    size_t n;
    while (text && (n = fread(text + text_len, 1, cap - text_len, f)) > 0) {
        text_len += n;
        if (text_len == cap)
            text = realloc(text, cap *= 2);
    }
    long cap_lines = 1024;
    line_start = malloc(sizeof(size_t) * (size_t)cap_lines);
    for (size_t p = 0; p < text_len;) {
        if (nlines + 2 >= cap_lines)
            line_start = realloc(line_start, sizeof(size_t) * (size_t)(cap_lines *= 2));
        line_start[nlines++] = p;
        const char *nl = memchr(text + p, '\n', text_len - p);
        p = nl ? (size_t)(nl - text) + 1 : text_len;
    }
    line_start[nlines] = text_len;
}

// 画面の行 r のうち、検索の文字に合うバイトか
static bool in_match(size_t p, const Row *r)
{
    size_t m = strlen(pattern);
    if (!m)
        return false;
    size_t start = line_start[r->line];
    size_t lo = p >= m ? p - m + 1 : 0;
    if (lo < start)
        lo = start;
    for (size_t q = lo; q <= p; q++)
        if (q + m <= text_len && !memcmp(text + q, pattern, m))
            return true;
    return false;
}

static int page(void)
{
    return LINES > 1 ? LINES - 1 : 1;
}

static void draw(long top, const char *msg)
{
    erase();
    for (int y = 0; y < page() && top + y < nrows; y++) {
        const Row *r = &rows[top + y];
        move(y, 0);
        int col = 0;
        for (size_t p = r->from; p < r->to;) {
            int len, w = char_width(p, col, &len);
            bool hl = in_match(p, r);
            if (hl)
                attron(A_REVERSE);
            if (text[p] == '\t') {
                for (int k = 0; k < w; k++)
                    addch(' ');
            } else {
                for (int k = 0; k < len; k++)
                    addch((unsigned char)text[p + k]);
            }
            if (hl)
                attroff(A_REVERSE);
            col += w;
            p += (size_t)len;
        }
    }
    for (int y = (int)(nrows - top); y < page(); y++)
        mvaddch(y, 0, '~');
    // 状態の行
    move(LINES - 1, 0);
    attron(A_REVERSE);
    if (msg) {
        printw("%s", msg);
    } else {
        long last = top + page() < nrows ? top + page() : nrows;
        long line = last > 0 ? rows[last - 1].line + 1 : 0;
        bool end = top + page() >= nrows;
        printw("%s %ld/%ld %s", name, line, nlines, end ? "(END)" : "");
        if (!end && nlines)
            printw("%ld%%", line * 100 / nlines);
    }
    attroff(A_REVERSE);
    clrtoeol();
    refresh();
}

// 状態の行で 1 行を読む（Enter で決める、Esc・Ctrl-C で取り消し）
static bool prompt(char c, char *buf, size_t size)
{
    size_t n = 0;
    buf[0] = '\0';
    for (;;) {
        move(LINES - 1, 0);
        clrtoeol();
        printw("%c%s", c, buf);
        refresh();
        int k = getch();
        if (k == '\r' || k == '\n' || k == KEY_ENTER)
            return true;
        if (k == 27 || k == 3)
            return false;
        if ((k == KEY_BACKSPACE || k == 127 || k == 8)) {
            if (n == 0)
                return false;
            while (n > 0 && ((unsigned char)buf[--n] & 0xC0) == 0x80)
                ;
            buf[n] = '\0';
        } else if (k >= 0x20 && k < 0x100 && n + 1 < size) {
            buf[n++] = (char)k;
            buf[n] = '\0';
        }
    }
}

// top から（向きに）探して、見つかった画面の行。なければ -1
static long find(long top, bool back)
{
    size_t m = strlen(pattern);
    if (!m)
        return -1;
    for (long r = back ? top - 1 : top + 1; r >= 0 && r < nrows; r += back ? -1 : 1)
        for (size_t p = rows[r].from; p < rows[r].to; p++)
            if (p + m <= text_len && !memcmp(text + p, pattern, m))
                return r;
    return -1;
}

int main(int argc, char **argv)
{
    // less [+g|+G] [--] [FILE]: +g は先頭から（既定）、+G は終わりから（GNU の less と同じ）。- で始まる
    // オプションは受け取って使わない（この less にはない）。-- の後は、- や + で始まってもファイルの名前
    FILE *f = stdin;
    name = "(stdin)";
    const char *file = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            if (i + 1 < argc)
                file = argv[i + 1];
            break;
        }
        if (argv[i][0] == '+')
            start_at_end = !strcmp(argv[i], "+G");
        else if (argv[i][0] != '-' || !argv[i][1])
            file = argv[i];
    }
    if (file) {
        f = fopen(file, "r");
        if (!f) {
            fprintf(stderr, "less: %s: No such file or directory\n", file);
            return 1;
        }
        name = file;
    } else if (isatty(0)) {
        fprintf(stderr, "usage: less FILE（または ... | less）\n");
        return 1;
    }
    load(f);
    if (f != stdin)
        fclose(f);
    if (!text || !line_start)
        return 1;
    if (!isatty(1)) {
        // 出力が画面でなければ、中身をそのまま書いて終わる（Linux の less と同じ。curses を始めると、
        // 見えない画面でキーを待ち続ける）
        fwrite(text, 1, text_len, stdout);
        return 0;
    }

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    build_rows();
    long top = start_at_end ? nrows : 0;    // 終わりからなら、下の max_top に切り詰める
    const char *msg = nullptr;
    for (;;) {
        long max_top = nrows - page() > 0 ? nrows - page() : 0;
        if (top > max_top)
            top = max_top;
        if (top < 0)
            top = 0;
        draw(top, msg);
        msg = nullptr;
        int k = getch();
        switch (k) {
        case 'q': case 'Q':
            endwin();
            return 0;
        case ' ': case 'f': case KEY_NPAGE: case 6:
            top += page();
            break;
        case 'b': case KEY_PPAGE: case 2:
            top -= page();
            break;
        case 'd': case 4:
            top += page() / 2;
            break;
        case 'u': case 21:
            top -= page() / 2;
            break;
        case 'j': case 'e': case KEY_DOWN: case '\r': case '\n': case KEY_ENTER: case 14:
            top++;
            break;
        case 'k': case 'y': case KEY_UP: case 16:
            top--;
            break;
        case 'g': case '<': case KEY_HOME:
            top = 0;
            break;
        case 'G': case '>': case KEY_END:
            top = max_top;
            break;
        case 12:    // Ctrl-L
            clearok(curscr, TRUE);
            break;
        case '/': case '?': {
            char buf[256];
            if (prompt((char)k, buf, sizeof(buf))) {
                if (buf[0])
                    snprintf(pattern, sizeof(pattern), "%s", buf);
                search_back = k == '?';
                long r = find(top, search_back);
                if (r >= 0)
                    top = r;
                else
                    msg = "Pattern not found";
            }
            break;
        }
        case 'n': case 'N': {
            long r = find(top, (k == 'N') != search_back);
            if (r >= 0)
                top = r;
            else
                msg = pattern[0] ? "Pattern not found" : "No previous search";
            break;
        }
        case KEY_RESIZE:
            if (COLS != cols_for_rows) {
                long line = top < nrows ? rows[top].line : 0;
                build_rows();
                for (top = 0; top < nrows && rows[top].line < line; top++)
                    ;
            }
            break;
        }
    }
}
