// curses（ncurses 互換の小さなもの）。アプリには OS API 表で渡す（os/api/api.def、ヘッダーは
// os/api/include/curses.h）。端末のエディタなど、ncurses を使う Linux 向けのプログラムをそのまま動かすため。
//
// 端末は xterm 互換（picocom・telnet の向こうの端末エミュレーター）とし、terminfo は使わない。
// 送るエスケープシーケンスとキーのシーケンスは、Pi 5 の terminfo（infocmp -1 xterm-256color）と同じ:
//   smcup ?1049h、rmcup ?1049l、smkx ?1h ESC =、rmkx ?1l ESC >、cup ESC[行;桁H、clear ESC[H ESC[2J、
//   色 ESC[38;5;n m、マウス ?1000h・?1002h・?1003h と SGR 形式 ?1006h（kmous=ESC[<）
//
// 画面は 2 つ持つ: stdscr（アプリが描いた内容）と curscr（端末に出ている内容）。wrefresh で違うところだけを
// 送る。1 マスは Unicode の 1 文字（全角は 2 マスで、右半分は ch = 0）と属性。
// 入力は、コンソールの受信バッファ（uart.c。UART・USB・telnet）のバイトを、キーの表（xterm の
// シーケンスと define_key で足したもの）で KEY_* に直す。ESC の後は ESCDELAY ミリ秒まで続きを待つ。

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../api/include/curses.h"
#include "curses_os.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../kernel/contap.h"
#include "../kernel/term.h"

int wcwidth(wchar_t c);     // picolibc（wchar.h では XSI のときだけ宣言される）。アプリの wcwidth と同じもの

typedef struct {
    uint32_t ch;        // Unicode。0 は全角の右半分、CELL_DIRTY は端末の中身がわからない
    uint32_t comb;      // 幅 0 の文字（結合文字）を 1 つまで
    attr_t attr;        // 属性と色の組（A_ALTCHARSET は含めない）
    uint8_t width;      // 1 か 2（右半分は 0）
} Cell;

#define CELL_DIRTY  0xFFFFFFFFu

struct monocs_window {
    int rows, cols;
    int cy, cx;
    attr_t attr;        // これから書く文字の属性
    attr_t bkgd;        // 消したマスの属性（wbkgdset）
    Cell *cells;
    int delay;          // wgetch の待ち（ミリ秒。-1 は来るまで、0 は待たない）
    bool keypad;
    bool clear;         // 次の wrefresh で画面全体を描き直す
    bool leave;         // leaveok
    uint8_t mb[4];      // addch に 1 バイトずつ渡された UTF-8 の途中
    int mb_len, mb_need;
};

static struct monocs_window std_win, cur_win;
WINDOW *stdscr, *curscr;
int LINES, COLS;
int COLORS, COLOR_PAIRS;
int ESCDELAY = 1000;    // ncurses の既定

static bool started;    // initscr を呼んだ（アプリの終わりで片付ける）
static bool active;     // initscr（または endwin の後の wrefresh）から endwin まで
static bool raw_mode;   // raw()（endwin で戻し、また始めるときに戻す）
static bool keypad_sent;
static int cursor_vis = 1;
static bool colors_on;
static short pair_fg[256], pair_bg[256];
static mmask_t mouse_mask;
static int mouse_interval = 166;
static int term_y = -1, term_x = -1;    // 端末のカーソル（わからなければ -1）
static attr_t term_attr;
static bool dirty;      // 前の wrefresh から stdscr が変わった

// ---- 送るもの ----

static char *obuf;
static size_t olen, ocap;

static void out(const char *s, size_t n)
{
    if (olen + n > ocap) {
        size_t cap = ocap ? ocap * 2 : 4096;
        while (cap < olen + n)
            cap *= 2;
        char *p = realloc(obuf, cap);
        if (!p)
            return;
        obuf = p;
        ocap = cap;
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void outs(const char *s)
{
    out(s, strlen(s));
}

static void outf(const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        out(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static void flush_out(void)
{
    if (olen) {
        fwrite(obuf, 1, olen, stdout);
        olen = 0;
    }
    fflush(stdout);
}

static void out_utf8(uint32_t c)
{
    char b[4];
    int n;
    if (c < 0x80) {
        b[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | c >> 6);
        b[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xE0 | c >> 12);
        b[1] = (char)(0x80 | (c >> 6 & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | c >> 18);
        b[1] = (char)(0x80 | (c >> 12 & 0x3F));
        b[2] = (char)(0x80 | (c >> 6 & 0x3F));
        b[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    out(b, n);
}

// 色の番号（0〜255）を SGR に。base は 30（前景）か 40（背景）
static void out_color(int c, int base)
{
    if (c < 0)
        outf(";%d", base + 9);              // 既定の色（39・49）
    else if (c < 8)
        outf(";%d", base + c);
    else if (c < 16)
        outf(";%d", base + 60 + c - 8);     // 明るい色（90〜97・100〜107）
    else
        outf(";%d;5;%d", base + 8, c);      // 256 色（38;5;n・48;5;n）
}

static void out_attr(attr_t a)
{
    outs("\x1b[0");
    if (a & A_BOLD)
        outs(";1");
    if (a & A_DIM)
        outs(";2");
    if (a & A_ITALIC)
        outs(";3");
    if (a & A_UNDERLINE)
        outs(";4");
    if (a & A_BLINK)
        outs(";5");
    if (a & (A_REVERSE | A_STANDOUT))
        outs(";7");
    if (a & A_INVIS)
        outs(";8");
    int p = PAIR_NUMBER(a);
    if (colors_on && p > 0) {
        if (pair_fg[p] >= 0)
            out_color(pair_fg[p], 30);
        if (pair_bg[p] >= 0)
            out_color(pair_bg[p], 40);
    }
    outs("m");
    term_attr = a;
}

// ---- 窓 ----

static Cell blank(attr_t a)
{
    return (Cell){' ', 0, a, 1};
}

static bool resize_window(struct monocs_window *w, int rows, int cols, Cell fill)
{
    if (rows < 1)
        rows = 1;
    if (cols < 1)
        cols = 1;
    Cell *c = malloc(sizeof(Cell) * (size_t)rows * (size_t)cols);
    if (!c)
        return false;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) {
            Cell v = fill;
            if (w->cells && y < w->rows && x < w->cols)
                v = w->cells[y * w->cols + x];
            c[y * cols + x] = v;
        }
    // 切れた全角の左半分は空白にする
    for (int y = 0; y < rows; y++)
        if (c[y * cols + cols - 1].width == 2)
            c[y * cols + cols - 1] = blank(c[y * cols + cols - 1].attr);
    free(w->cells);
    w->cells = c;
    w->rows = rows;
    w->cols = cols;
    if (w->cy >= rows)
        w->cy = rows - 1;
    if (w->cx >= cols)
        w->cx = cols - 1;
    return true;
}

static Cell *cell(struct monocs_window *w, int y, int x)
{
    return &w->cells[y * w->cols + x];
}

// (y, x) に書く前に、そこにかかっている全角の文字を空白にする
static void break_wide(struct monocs_window *w, int y, int x)
{
    Cell *c = cell(w, y, x);
    if (c->width == 0 && x > 0)
        *cell(w, y, x - 1) = blank(cell(w, y, x - 1)->attr);
    if (c->width == 2 && x + 1 < w->cols)
        *cell(w, y, x + 1) = blank(c->attr);
}

static int advance(struct monocs_window *w, int n)
{
    w->cx += n;
    if (w->cx < w->cols)
        return OK;
    w->cx = 0;
    if (++w->cy < w->rows)
        return OK;
    // 右下の角を越えた（スクロールはしない）
    w->cy = w->rows - 1;
    w->cx = w->cols - 1;
    return ERR;
}

static int put_char(struct monocs_window *w, uint32_t cp, attr_t a)
{
    int width = wcwidth((wchar_t)cp);
    if (width == 0) {
        // 結合文字は前の文字に付ける
        int x = w->cx - 1;
        if (x >= 0 && cell(w, w->cy, x)->width == 0)
            x--;
        if (x >= 0)
            cell(w, w->cy, x)->comb = cp;
        dirty = true;
        return OK;
    }
    if (width < 0)
        width = 1;
    if (width == 2 && w->cx == w->cols - 1) {
        // 行末に全角が入らない: 空白で埋めて次の行へ（ncurses と同じ）
        break_wide(w, w->cy, w->cx);
        *cell(w, w->cy, w->cx) = blank(a);
        if (advance(w, 1) == ERR)
            return ERR;
    }
    break_wide(w, w->cy, w->cx);
    *cell(w, w->cy, w->cx) = (Cell){cp, 0, a, (uint8_t)width};
    if (width == 2) {
        break_wide(w, w->cy, w->cx + 1);
        *cell(w, w->cy, w->cx + 1) = (Cell){0, 0, a, 0};
    }
    dirty = true;
    return advance(w, width);
}

// A_ALTCHARSET の文字（VT100 の罫線）を Unicode に
static uint32_t acs_char(int c)
{
    switch (c) {
    case 'q': return 0x2500;    // ─
    case 'x': return 0x2502;    // │
    case 'l': return 0x250C;    // ┌
    case 'k': return 0x2510;    // ┐
    case 'm': return 0x2514;    // └
    case 'j': return 0x2518;    // ┘
    case 't': return 0x251C;    // ├
    case 'u': return 0x2524;    // ┤
    case 'w': return 0x252C;    // ┬
    case 'v': return 0x2534;    // ┴
    case 'n': return 0x253C;    // ┼
    case '`': return 0x25C6;    // ◆
    case 'a': return 0x2592;    // ▒
    case 'f': return 0x00B0;    // °
    case 'g': return 0x00B1;    // ±
    case '~': return 0x00B7;    // ·
    case ',': return 0x2190;    // ←
    case '+': return 0x2192;    // →
    case '.': return 0x2193;    // ↓
    case '-': return 0x2191;    // ↑
    case '0': return 0x2588;    // █
    default:  return (uint32_t)c;
    }
}

// 文字の属性: 窓の属性に文字の属性を足す（色は文字のものが優先）
static attr_t char_attr(struct monocs_window *w, chtype ch)
{
    attr_t a = w->attr | (ch & A_ATTRIBUTES & ~A_COLOR);
    if (ch & A_COLOR)
        a = (a & ~A_COLOR) | (ch & A_COLOR);
    return a & ~A_ALTCHARSET;
}

int waddch(WINDOW *w, chtype ch)
{
    if (!w)
        return ERR;
    attr_t a = char_attr(w, ch);
    unsigned c = ch & A_CHARTEXT;
    if (ch & A_ALTCHARSET)
        return put_char(w, acs_char((int)c), a);
    if (w->mb_need) {
        if ((c & 0xC0) == 0x80) {
            w->mb[w->mb_len++] = (uint8_t)c;
            if (w->mb_len < w->mb_need)
                return OK;
            uint32_t cp = w->mb[0] & (0x7F >> w->mb_need);
            for (int i = 1; i < w->mb_len; i++)
                cp = cp << 6 | (w->mb[i] & 0x3F);
            w->mb_need = 0;
            return put_char(w, cp, a);
        }
        w->mb_need = 0;
        put_char(w, 0xFFFD, a);     // 途中で切れた UTF-8
    }
    if (c >= 0x80) {
        int need = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (!need)
            return put_char(w, 0xFFFD, a);
        w->mb[0] = (uint8_t)c;
        w->mb_len = 1;
        w->mb_need = need;
        return OK;
    }
    switch (c) {
    case '\n':
        wclrtoeol(w);
        w->cx = 0;
        if (w->cy + 1 >= w->rows)
            return ERR;
        w->cy++;
        dirty = true;
        return OK;
    case '\r':
        w->cx = 0;
        dirty = true;
        return OK;
    case '\b':
        if (w->cx > 0)
            w->cx--;
        dirty = true;
        return OK;
    case '\t':
        do {
            if (put_char(w, ' ', a) == ERR)
                return ERR;
        } while (w->cx % 8);
        return OK;
    }
    if (c < 0x20 || c == 0x7F) {
        // 制御文字は ^X で表す（ncurses と同じ）
        put_char(w, '^', a);
        return put_char(w, c == 0x7F ? '?' : c + 0x40, a);
    }
    return put_char(w, c, a);
}

int waddnstr(WINDOW *w, const char *s, int n)
{
    if (!w || !s)
        return ERR;
    int r = OK;
    for (int i = 0; (n < 0 || i < n) && s[i]; i++)
        if (waddch(w, (unsigned char)s[i]) == ERR)
            r = ERR;
    return r;
}

int waddstr(WINDOW *w, const char *s)
{
    return waddnstr(w, s, -1);
}

int vw_printw(WINDOW *w, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    return waddnstr(w, buf, -1);
}

int wprintw(WINDOW *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(w, fmt, ap);
    va_end(ap);
    return r;
}

int printw(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(stdscr, fmt, ap);
    va_end(ap);
    return r;
}

int mvprintw(int y, int x, const char *fmt, ...)
{
    if (wmove(stdscr, y, x) == ERR)
        return ERR;
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(stdscr, fmt, ap);
    va_end(ap);
    return r;
}

int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...)
{
    if (wmove(w, y, x) == ERR)
        return ERR;
    va_list ap;
    va_start(ap, fmt);
    int r = vw_printw(w, fmt, ap);
    va_end(ap);
    return r;
}

int wmove(WINDOW *w, int y, int x)
{
    if (!w || y < 0 || x < 0 || y >= w->rows || x >= w->cols)
        return ERR;
    w->cy = y;
    w->cx = x;
    w->mb_need = 0;
    dirty = true;
    return OK;
}

int wclrtoeol(WINDOW *w)
{
    if (!w)
        return ERR;
    if (w->cx > 0)
        break_wide(w, w->cy, w->cx);
    for (int x = w->cx; x < w->cols; x++)
        *cell(w, w->cy, x) = blank(w->bkgd);
    dirty = true;
    return OK;
}

int wclrtobot(WINDOW *w)
{
    if (!w)
        return ERR;
    wclrtoeol(w);
    for (int y = w->cy + 1; y < w->rows; y++)
        for (int x = 0; x < w->cols; x++)
            *cell(w, y, x) = blank(w->bkgd);
    return OK;
}

int werase(WINDOW *w)
{
    if (!w)
        return ERR;
    for (int i = 0; i < w->rows * w->cols; i++)
        w->cells[i] = blank(w->bkgd);
    w->cy = w->cx = 0;
    dirty = true;
    return OK;
}

int wclear(WINDOW *w)
{
    werase(w);
    if (w)
        w->clear = true;
    return OK;
}

static int draw_line(WINDOW *w, chtype ch, int n, bool vertical, chtype def)
{
    if (!w)
        return ERR;
    if (!(ch & A_CHARTEXT))
        ch |= def;
    int y = w->cy, x = w->cx;
    for (int i = 0; i < n && y < w->rows && x < w->cols; i++) {
        w->cy = y;
        w->cx = x;
        waddch(w, ch);
        if (vertical)
            y++;
        else
            x++;
    }
    return OK;
}

int whline(WINDOW *w, chtype ch, int n)
{
    if (!w)
        return ERR;
    int y = w->cy, x = w->cx;
    draw_line(w, ch, n, false, ACS_HLINE);
    w->cy = y;      // 線はカーソルを動かさない
    w->cx = x;
    return OK;
}

int wvline(WINDOW *w, chtype ch, int n)
{
    if (!w)
        return ERR;
    int y = w->cy, x = w->cx;
    draw_line(w, ch, n, true, ACS_VLINE);
    w->cy = y;
    w->cx = x;
    return OK;
}

// 枠（ncurses と同じ。2026-11-26）: 0 を渡したところは既定の罫線の文字（ACS_*）。カーソルは動かさない
int wborder(WINDOW *w, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br)
{
    if (!w || w->rows < 1 || w->cols < 1)
        return ERR;
    int y = w->cy, x = w->cx, r = w->rows - 1, c = w->cols - 1;
    w->cy = 0, w->cx = 1;
    draw_line(w, ts, c - 1, false, ACS_HLINE);
    w->cy = r, w->cx = 1;
    draw_line(w, bs, c - 1, false, ACS_HLINE);
    w->cy = 1, w->cx = 0;
    draw_line(w, ls, r - 1, true, ACS_VLINE);
    w->cy = 1, w->cx = c;
    draw_line(w, rs, r - 1, true, ACS_VLINE);
    static const chtype def[4] = { ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER };
    chtype corner[4] = { tl, tr, bl, br };
    int cy[4] = { 0, 0, r, r }, cx[4] = { 0, c, 0, c };
    for (int i = 0; i < 4; i++) {
        w->cy = cy[i], w->cx = cx[i];
        waddch(w, (corner[i] & A_CHARTEXT) ? corner[i] : corner[i] | def[i]);
    }
    w->cy = y;
    w->cx = x;
    return OK;
}

int box(WINDOW *w, chtype verch, chtype horch)
{
    return wborder(w, verch, verch, horch, horch, 0, 0, 0, 0);
}

chtype winch(WINDOW *w)
{
    if (!w)
        return (chtype)ERR;
    Cell *c = cell(w, w->cy, w->cx);
    return (c->ch < 0x80 ? c->ch : '?') | c->attr;
}

// ---- 属性と色 ----

int wattron(WINDOW *w, int a)
{
    return wattr_on(w, (attr_t)a, nullptr);
}

int wattroff(WINDOW *w, int a)
{
    return wattr_off(w, (attr_t)a, nullptr);
}

int wattrset(WINDOW *w, int a)
{
    if (!w)
        return ERR;
    w->attr = (attr_t)a;
    return OK;
}

int wattr_on(WINDOW *w, attr_t a, void *)
{
    if (!w)
        return ERR;
    if (a & A_COLOR)
        w->attr = (w->attr & ~A_COLOR) | (a & A_COLOR);
    w->attr |= a & ~A_COLOR;
    return OK;
}

int wattr_off(WINDOW *w, attr_t a, void *)
{
    if (!w)
        return ERR;
    if (a & A_COLOR)
        w->attr &= ~A_COLOR;
    w->attr &= ~(a & ~A_COLOR);
    return OK;
}

int wattr_get(WINDOW *w, attr_t *a, short *pair, void *)
{
    if (!w)
        return ERR;
    if (a)
        *a = w->attr;
    if (pair)
        *pair = (short)PAIR_NUMBER(w->attr);
    return OK;
}

int wattr_set(WINDOW *w, attr_t a, short pair, void *)
{
    if (!w)
        return ERR;
    w->attr = (a & ~A_COLOR) | COLOR_PAIR(pair);
    return OK;
}

int wcolor_set(WINDOW *w, short pair, void *)
{
    if (!w)
        return ERR;
    w->attr = (w->attr & ~A_COLOR) | COLOR_PAIR(pair);
    return OK;
}

int wbkgdset(WINDOW *w, chtype ch)
{
    if (!w)
        return ERR;
    w->bkgd = ch & A_ATTRIBUTES & ~A_ALTCHARSET;
    return OK;
}

int has_colors(void)
{
    return true;
}

int can_change_color(void)
{
    return false;
}

int start_color(void)
{
    colors_on = true;
    COLORS = 256;
    COLOR_PAIRS = 256;
    for (int i = 0; i < 256; i++)
        pair_fg[i] = pair_bg[i] = -1;
    return OK;
}

int use_default_colors(void)
{
    return OK;     // 組 0 はいつも端末の既定の色（-1）
}

int assume_default_colors(int, int)
{
    return OK;
}

int init_pair(short pair, short fg, short bg)
{
    if (pair < 1 || pair >= 256 || fg >= 256 || bg >= 256)
        return ERR;
    pair_fg[pair] = fg;
    pair_bg[pair] = bg;
    dirty = true;
    return OK;
}

int pair_content(short pair, short *fg, short *bg)
{
    if (pair < 0 || pair >= 256)
        return ERR;
    *fg = pair ? pair_fg[pair] : -1;
    *bg = pair ? pair_bg[pair] : -1;
    return OK;
}

// ---- 画面への反映 ----

// 端末のそのマスの中身はわからない（次の wrefresh で必ず描く）
static void mark_dirty(Cell *c)
{
    *c = (Cell){CELL_DIRTY, 0, 0, 1};
}

static void move_term(int y, int x)
{
    if (term_y == y && term_x == x)
        return;
    outf("\x1b[%d;%dH", y + 1, x + 1);
    term_y = y;
    term_x = x;
}

static void enter_screen(void)
{
    outs("\x1b[?1049h");                    // 代わりの画面（smcup）
    if (keypad_sent)
        outs("\x1b[?1h\x1b=");              // smkx
    outs(cursor_vis ? "\x1b[?25h" : "\x1b[?25l");
    if (mouse_mask)
        outs(mouse_mask & REPORT_MOUSE_POSITION ? "\x1b[?1000h\x1b[?1003h\x1b[?1006h" : "\x1b[?1000h\x1b[?1006h");
    cur_win.clear = true;
    active = true;
    uart_set_raw(raw_mode);
}

int wrefresh(WINDOW *w)
{
    if (!started || w != stdscr)
        return w == curscr ? OK : ERR;
    if (!active)
        enter_screen();
    if (std_win.rows != cur_win.rows || std_win.cols != cur_win.cols) {
        resize_window(&cur_win, std_win.rows, std_win.cols, blank(0));
        cur_win.clear = true;
    }
    if (std_win.clear || cur_win.clear) {
        outs("\x1b[0m\x1b[H\x1b[2J");
        term_attr = 0;
        term_y = term_x = 0;
        for (int i = 0; i < cur_win.rows * cur_win.cols; i++)
            cur_win.cells[i] = blank(0);
        std_win.clear = cur_win.clear = false;
        dirty = true;
    }
    if (dirty) {
        int rows = std_win.rows, cols = std_win.cols;
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                Cell *s = cell(&std_win, y, x), *t = cell(&cur_win, y, x);
                if (s->ch == t->ch && s->comb == t->comb && s->attr == t->attr && s->width == t->width)
                    continue;
                if (s->width == 0) {
                    // 全角の右半分だけが違う: 左半分から描き直す（描くと右半分も写すので、戻るのは 1 回）
                    if (x > 0 && cell(&cur_win, y, x - 1)->ch != CELL_DIRTY) {
                        mark_dirty(cell(&cur_win, y, x - 1));
                        x -= 2;
                    }
                    continue;
                }
                if (t->width == 0 && x > 0) {
                    // 端末の全角の右半分に書くと左半分も消える: どちらも中身がわからないことにして、左から描く
                    mark_dirty(cell(&cur_win, y, x - 1));
                    mark_dirty(t);
                    x -= 2;
                    continue;
                }
                if (s->width == 2 && x + 1 >= cols)
                    continue;
                move_term(y, x);
                if (s->attr != term_attr)
                    out_attr(s->attr);
                out_utf8(s->ch);
                if (s->comb)
                    out_utf8(s->comb);
                // 端末の全角の左半分に書くと右半分も消える
                if (t->width == 2 && s->width == 1 && x + 1 < cols)
                    mark_dirty(cell(&cur_win, y, x + 1));
                if (s->width == 2 && x + 2 < cols && cell(&cur_win, y, x + 1)->width == 2)
                    mark_dirty(cell(&cur_win, y, x + 2));
                *t = *s;
                if (s->width == 2) {
                    *cell(&cur_win, y, x + 1) = *cell(&std_win, y, x + 1);
                    x++;
                }
                term_x = x + 1;
                if (term_x >= cols)
                    term_x = -1;    // 右端の後のカーソルは端末によって違う
            }
        }
        dirty = false;
    }
    if (!std_win.leave && cursor_vis)
        move_term(std_win.cy, std_win.cx);
    flush_out();
    return OK;
}

int wnoutrefresh(WINDOW *)
{
    return OK;
}

int doupdate(void)
{
    return wrefresh(stdscr);
}

int redrawwin(WINDOW *)
{
    cur_win.clear = true;
    return OK;
}

int clearok(WINDOW *w, bool bf)
{
    if (w == curscr || w == stdscr)
        cur_win.clear = bf;
    return OK;
}

int leaveok(WINDOW *w, bool bf)
{
    if (w)
        w->leave = bf;
    return OK;
}

int curs_set(int v)
{
    int prev = cursor_vis;
    cursor_vis = v;
    if (active) {
        outs(v ? "\x1b[?25h" : "\x1b[?25l");
        flush_out();
    }
    return prev;
}

int beep(void)
{
    outs("\a");
    flush_out();
    return OK;
}

int flash(void)
{
    return OK;
}

// ---- 始まりと終わり ----

WINDOW *initscr(void)
{
    if (started) {
        wrefresh(stdscr);
        return stdscr;
    }
    int rows, cols;
    fflush(stdout);
    term_size(&rows, &cols);
    std_win = (struct monocs_window){.delay = -1};
    cur_win = (struct monocs_window){.delay = -1};
    if (!resize_window(&std_win, rows, cols, blank(0)) || !resize_window(&cur_win, rows, cols, blank(0)))
        return nullptr;
    stdscr = &std_win;
    curscr = &cur_win;
    LINES = rows;
    COLS = cols;
    COLORS = 0;
    COLOR_PAIRS = 0;
    colors_on = false;
    raw_mode = false;
    keypad_sent = false;
    cursor_vis = 1;
    mouse_mask = 0;
    term_attr = 0;
    term_y = term_x = -1;
    started = true;
    enter_screen();
    dirty = true;
    flush_out();
    return stdscr;
}

static void leave_screen(void)
{
    outs("\x1b[0m");
    outf("\x1b[%d;1H", std_win.rows);
    outs("\x1b[?25h");
    if (mouse_mask)
        outs("\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l");
    if (keypad_sent)
        outs("\x1b[?1l\x1b>");              // rmkx
    outs("\x1b[?1049l");                    // 元の画面に戻る（rmcup）
    flush_out();
    term_y = term_x = -1;
    term_attr = 0;
    active = false;
    uart_set_raw(false);
}

int endwin(void)
{
    if (!active)
        return ERR;
    leave_screen();
    return OK;
}

bool isendwin(void)
{
    return !active;
}

static void reset_input(void);

// nl（既定）なら Enter を '\n' で返す（nl・nonl の下）
static bool nl_mode = true;

void curses_app_exit(void)
{
    nl_mode = true;
    if (!started)
        return;
    if (active) {
        // endwin を呼ばずに終わった: アプリが自分で入れた端末のモード（括弧付き貼り付け、modifyOtherKeys、
        // マウス）も戻す
        outs("\x1b[?2004l\x1b[>4;0m\x1b[?1002l");
        leave_screen();
    }
    free(std_win.cells);
    free(cur_win.cells);
    std_win = cur_win = (struct monocs_window){0};
    stdscr = curscr = nullptr;
    free(obuf);
    obuf = nullptr;
    olen = ocap = 0;
    reset_input();
    ESCDELAY = 1000;
    mouse_interval = 166;
    started = false;
}

void curses_emergency_restore(void)
{
    if (!active)
        return;
    uart_puts("\x1b[0m\x1b[?25h\x1b[?2004l\x1b[>4;0m\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l"
              "\x1b[?1l\x1b>\x1b[?1049l");
    active = false;
    uart_set_raw(false);
}

int raw(void)
{
    raw_mode = true;
    if (active)
        uart_set_raw(true);
    return OK;
}

int noraw(void)
{
    raw_mode = false;
    uart_set_raw(false);
    return OK;
}

int cbreak(void)
{
    return noraw();
}

int nocbreak(void)
{
    return noraw();
}

// curses はキーを UART から直接読む（termios を通らない。os/libc/termios.c）ので、画面はエコーせず、行の編集もない。
// echo などは何もしない
int echo(void) { return OK; }
int noecho(void) { return OK; }

// nl（既定）: Enter（端末が送る '\r'）を '\n' にして返す。nonl() で '\r' のまま（ncurses と同じ。ncurses は入力の
// ICRNL と出力の改行の扱いを切り替える。Monocs の curses は出力の改行を自分で動かすので、入力だけ）
int nl(void) { nl_mode = true; return OK; }
int nonl(void) { nl_mode = false; return OK; }
int meta(WINDOW *, bool) { return OK; }
int intrflush(WINDOW *, bool) { return OK; }
int scrollok(WINDOW *, bool) { return OK; }
int idlok(WINDOW *, bool) { return OK; }
int notimeout(WINDOW *, bool) { return OK; }

int keypad(WINDOW *w, bool bf)
{
    if (!w)
        return ERR;
    w->keypad = bf;
    if (bf && !keypad_sent && active) {
        outs("\x1b[?1h\x1b=");      // smkx: カーソルキーを ESC O A などで送らせる
        flush_out();
    }
    keypad_sent = keypad_sent || bf;
    return OK;
}

int nodelay(WINDOW *w, bool bf)
{
    if (!w)
        return ERR;
    w->delay = bf ? 0 : -1;
    return OK;
}

void wtimeout(WINDOW *w, int delay)
{
    if (w)
        w->delay = delay < 0 ? -1 : delay;
}

int set_escdelay(int ms)
{
    if (ms < 0)
        return ERR;
    ESCDELAY = ms;
    return OK;
}

int napms(int ms)
{
    if (ms > 0)
        timer_delay_us((uint64_t)ms * 1000);
    return OK;
}

int getcurx(const WINDOW *w) { return w ? w->cx : ERR; }
int getcury(const WINDOW *w) { return w ? w->cy : ERR; }
int getmaxx(const WINDOW *w) { return w ? w->cols : ERR; }
int getmaxy(const WINDOW *w) { return w ? w->rows : ERR; }
int getbegx(const WINDOW *w) { return w ? 0 : ERR; }
int getbegy(const WINDOW *w) { return w ? 0 : ERR; }

int resizeterm(int lines, int cols)
{
    if (!started || lines < 1 || cols < 1)
        return ERR;
    resize_window(&std_win, lines, cols, blank(std_win.bkgd));
    resize_window(&cur_win, lines, cols, blank(0));
    LINES = lines;
    COLS = cols;
    cur_win.clear = true;
    dirty = true;
    return OK;
}

int resize_term(int lines, int cols)
{
    if (lines == 0 || cols == 0)
        return OK;  // PDCurses の「今の大きさに合わせる」。大きさは KEY_RESIZE の前に合わせてある
    return resizeterm(lines, cols);
}

bool is_term_resized(int lines, int cols)
{
    return lines != LINES || cols != COLS;
}

// ---- 入力 ----

typedef struct {
    char *seq;
    int code;
} KeyDef;

static KeyDef *keys;
static int nkeys, capkeys;
static bool keys_ready;

static unsigned char pend[64];  // 読んだがまだ使っていないバイト（先頭から）
static int npend;
static int ungot[64];           // ungetch したもの（後から入れたものが先）
static int nungot;
static MEVENT mevents[32];      // KEY_MOUSE で返したマウスのイベント（getmouse で取り出す）
static int nmevents;
static int last_button;         // X10 形式の「離した」がどのボタンかを覚えておく

#define READ_TIMEOUT    (-1)
#define READ_RESIZE     (-2)

static void reset_input(void)
{
    for (int i = 0; i < nkeys; i++)
        free(keys[i].seq);
    free(keys);
    keys = nullptr;
    nkeys = capkeys = 0;
    keys_ready = false;
    npend = nungot = nmevents = 0;
}

static void add_key(const char *seq, int code)
{
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(keys[i].seq, seq)) {
            keys[i].code = code;
            return;
        }
    if (nkeys == capkeys) {
        int cap = capkeys ? capkeys * 2 : 128;
        KeyDef *k = realloc(keys, sizeof(KeyDef) * cap);
        if (!k)
            return;
        keys = k;
        capkeys = cap;
    }
    size_t n = strlen(seq) + 1;
    char *s = malloc(n);
    if (!s)
        return;
    memcpy(s, seq, n);
    keys[nkeys++] = (KeyDef){s, code};
}

// xterm のキー（Pi 5 の infocmp -1 xterm-256color）。smkx の形（ESC O A）と、送っていない端末の形（ESC [ A）の両方
static void init_keys(void)
{
    if (keys_ready)
        return;
    keys_ready = true;
    static const struct { const char *seq; int code; } xterm[] = {
        {"\x1bOA", KEY_UP}, {"\x1bOB", KEY_DOWN}, {"\x1bOC", KEY_RIGHT}, {"\x1bOD", KEY_LEFT},
        {"\x1b[A", KEY_UP}, {"\x1b[B", KEY_DOWN}, {"\x1b[C", KEY_RIGHT}, {"\x1b[D", KEY_LEFT},
        {"\x1bOH", KEY_HOME}, {"\x1bOF", KEY_END}, {"\x1b[H", KEY_HOME}, {"\x1b[F", KEY_END},
        {"\x1b[1~", KEY_HOME}, {"\x1b[4~", KEY_END}, {"\x1b[7~", KEY_HOME}, {"\x1b[8~", KEY_END},
        {"\x1b[2~", KEY_IC}, {"\x1b[3~", KEY_DC}, {"\x1b[5~", KEY_PPAGE}, {"\x1b[6~", KEY_NPAGE},
        {"\x1b[Z", KEY_BTAB}, {"\x7f", KEY_BACKSPACE},
        {"\x1bOM", KEY_ENTER}, {"\x1bOE", KEY_BEG}, {"\x1b[E", KEY_B2},
        {"\x1bOw", KEY_A1}, {"\x1bOy", KEY_A3}, {"\x1bOu", KEY_B2}, {"\x1bOq", KEY_C1}, {"\x1bOs", KEY_C3},
        {"\x1b[3;2~", KEY_SDC}, {"\x1b[1;2F", KEY_SEND}, {"\x1b[1;2H", KEY_SHOME}, {"\x1b[2;2~", KEY_SIC},
        {"\x1b[1;2D", KEY_SLEFT}, {"\x1b[6;2~", KEY_SNEXT}, {"\x1b[5;2~", KEY_SPREVIOUS},
        {"\x1b[1;2C", KEY_SRIGHT}, {"\x1b[1;2B", KEY_SF}, {"\x1b[1;2A", KEY_SR},
        {"\x1bOP", KEY_F(1)}, {"\x1bOQ", KEY_F(2)}, {"\x1bOR", KEY_F(3)}, {"\x1bOS", KEY_F(4)},
        {"\x1b[11~", KEY_F(1)}, {"\x1b[12~", KEY_F(2)}, {"\x1b[13~", KEY_F(3)}, {"\x1b[14~", KEY_F(4)},
        {"\x1b[15~", KEY_F(5)}, {"\x1b[17~", KEY_F(6)}, {"\x1b[18~", KEY_F(7)}, {"\x1b[19~", KEY_F(8)},
        {"\x1b[20~", KEY_F(9)}, {"\x1b[21~", KEY_F(10)}, {"\x1b[23~", KEY_F(11)}, {"\x1b[24~", KEY_F(12)},
    };
    for (size_t i = 0; i < sizeof(xterm) / sizeof(xterm[0]); i++)
        add_key(xterm[i].seq, xterm[i].code);
    // F13〜F60: Shift（;2）、Ctrl（;5）、Ctrl+Shift（;6）、Alt（;3）付きの F1〜F12
    static const int mods[] = {2, 5, 6, 3};
    static const char *const fkey[] = {"P", "Q", "R", "S", "15~", "17~", "18~", "19~", "20~", "21~", "23~", "24~"};
    for (int m = 0; m < 4; m++)
        for (int f = 0; f < 12; f++) {
            char seq[16];
            if (f < 4)
                snprintf(seq, sizeof(seq), "\x1b[1;%d%s", mods[m], fkey[f]);
            else
                snprintf(seq, sizeof(seq), "\x1b[%.2s;%d~", fkey[f], mods[m]);
            add_key(seq, KEY_F(13 + m * 12 + f));
        }
}

int define_key(const char *def, int code)
{
    init_keys();
    if (!def) {
        for (int i = 0; i < nkeys;)
            if (keys[i].code == code) {
                free(keys[i].seq);
                keys[i] = keys[--nkeys];
            } else {
                i++;
            }
        return OK;
    }
    if (code <= 0) {
        for (int i = 0; i < nkeys; i++)
            if (!strcmp(keys[i].seq, def)) {
                free(keys[i].seq);
                keys[i] = keys[--nkeys];
                break;
            }
        return OK;
    }
    add_key(def, code);
    return OK;
}

int key_defined(const char *def)
{
    init_keys();
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(keys[i].seq, def))
            return keys[i].code;
    return 0;
}

int has_key(int code)
{
    init_keys();
    for (int i = 0; i < nkeys; i++)
        if (keys[i].code == code)
            return true;
    return false;
}

// telnet（NAWS）で知らされた大きさが変わったか
static bool resize_pending(void)
{
    int r, c;
    return started && contap_size(&r, &c) && (r != LINES || c != COLS);
}

// 1 バイト読む。ms: -1 は来るまで、0 は待たない
static int read_byte(int ms)
{
    if (npend) {
        int b = pend[0];
        memmove(pend, pend + 1, (size_t)--npend);
        return b;
    }
    uint64_t end = ms > 0 ? timer_now_us() + (uint64_t)ms * 1000 : 0;
    for (;;) {
        if (uart_has_input())
            return (unsigned char)uart_getc();
        if (resize_pending())
            return READ_RESIZE;
        if (ms == 0 || (ms > 0 && timer_now_us() >= end))
            return READ_TIMEOUT;
        uart_wait_input();      // 次の割り込み（文字・タイマー）まで眠る
    }
}

static void unread(const unsigned char *b, int n)
{
    if (n <= 0)
        return;
    if (npend + n > (int)sizeof(pend))
        n = (int)sizeof(pend) - npend;
    memmove(pend + n, pend, (size_t)npend);
    memcpy(pend, b, (size_t)n);
    npend += n;
}

// マウスのイベントを作る。b はボタンの番号（xterm の形式）、press は押した（SGR の M）か
static bool mouse_event(int b, int x, int y, bool release)
{
    mmask_t ev = 0;
    int button = b & 3;
    if (b & 64) {
        if (button > 1)
            return false;       // 横のホイール
        ev = button == 0 ? BUTTON4_PRESSED : BUTTON5_PRESSED;
    } else if (b & 32) {
        ev = REPORT_MOUSE_POSITION;     // 動かした（押したまま、または ?1003h のとき）
    } else if (release || button == 3) {
        int n = button == 3 ? last_button : button + 1;
        if (!n)
            return false;
        ev = NCURSES_MOUSE_MASK(n, NCURSES_BUTTON_RELEASED);
    } else {
        last_button = button + 1;
        ev = NCURSES_MOUSE_MASK(button + 1, NCURSES_BUTTON_PRESSED);
    }
    if (b & 4)
        ev |= BUTTON_SHIFT;
    if (b & 8)
        ev |= BUTTON_ALT;
    if (b & 16)
        ev |= BUTTON_CTRL;
    if (!(ev & mouse_mask & ~(BUTTON_SHIFT | BUTTON_ALT | BUTTON_CTRL)))
        return false;           // mousemask で頼まれていない
    if (nmevents == (int)(sizeof(mevents) / sizeof(mevents[0])))
        memmove(mevents, mevents + 1, sizeof(MEVENT) * --nmevents);
    mevents[nmevents++] = (MEVENT){.id = 0, .x = x - 1, .y = y - 1, .z = 0, .bstate = ev};
    return true;
}

// ESC [ < b ; x ; y M（または m）の残り
static bool read_sgr_mouse(void)
{
    int v[3] = {0, 0, 0}, i = 0;
    for (;;) {
        int c = read_byte(ESCDELAY > 100 ? ESCDELAY : 100);
        if (c < 0)
            return false;
        if (c >= '0' && c <= '9')
            v[i] = v[i] * 10 + (c - '0');
        else if (c == ';' && i < 2)
            i++;
        else if (c == 'M' || c == 'm')
            return mouse_event(v[0], v[1], v[2], c == 'm');
        else
            return false;
    }
}

// ESC [ M b x y（X10 形式。値に 32 を足した 1 バイトずつ）
static bool read_x10_mouse(void)
{
    int v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = read_byte(ESCDELAY > 100 ? ESCDELAY : 100);
        if (v[i] < 0)
            return false;
        v[i] -= 32;
    }
    return mouse_event(v[0], v[1], v[2], false);
}

static int do_resize(void)
{
    int r, c;
    if (contap_size(&r, &c))
        resizeterm(r, c);
    return KEY_RESIZE;
}

// キーを 1 つ読む（keypad なら ESC で始まるシーケンスを KEY_* に）。ERR は時間切れ
static int read_key(struct monocs_window *w)
{
    for (;;) {
        int b = read_byte(w->delay);
        if (b == READ_TIMEOUT)
            return ERR;
        if (b == READ_RESIZE)
            return do_resize();
        if (!w->keypad)
            return b;
        unsigned char buf[32];
        int n = 0;
        buf[n++] = (unsigned char)b;
        bool mouse = false;
        for (;;) {
            if (n == 3 && buf[0] == 0x1B && buf[1] == '[' && (buf[2] == '<' || buf[2] == 'M')) {
                bool ok = buf[2] == '<' ? read_sgr_mouse() : read_x10_mouse();
                if (ok)
                    return KEY_MOUSE;
                mouse = true;   // 頼まれていないイベント・壊れたもの: 読み捨てて次のキーへ
                break;
            }
            int exact = -1;
            bool longer = false;
            for (int i = 0; i < nkeys; i++) {
                const char *s = keys[i].seq;
                if (strncmp(s, (const char *)buf, (size_t)n) != 0 || (int)strlen(s) < n)
                    continue;
                if ((int)strlen(s) == n)
                    exact = keys[i].code;
                else
                    longer = true;
            }
            if (!longer) {
                if (exact >= 0)
                    return exact;
                break;
            }
            int c = n < (int)sizeof(buf) ? read_byte(ESCDELAY) : READ_TIMEOUT;
            if (c < 0) {
                if (exact >= 0)
                    return exact;
                break;
            }
            buf[n++] = (unsigned char)c;
        }
        if (mouse) {
            if (w->delay == 0 && !npend && !uart_has_input())
                return ERR;
            continue;
        }
        // 表にない: 最初のバイトを返し、残りは次に読む（ncurses と同じ）
        unread(buf + 1, n - 1);
        return buf[0];
    }
}

int wgetch(WINDOW *w)
{
    if (!w)
        return ERR;
    init_keys();
    if (started && (dirty || !active))
        wrefresh(stdscr);
    if (nungot)
        return ungot[--nungot];
    int c = read_key(w);
    return c == '\r' && nl_mode ? '\n' : c;
}

int wget_wch(WINDOW *w, wint_t *wch)
{
    int c = wgetch(w);
    if (c == ERR)
        return ERR;
    if (c > 0xFF) {
        *wch = (wint_t)c;
        return KEY_CODE_YES;
    }
    if (c < 0x80) {
        *wch = (wint_t)c;
        return OK;
    }
    // UTF-8 の続きのバイト
    int need = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
    if (need < 0) {
        *wch = 0xFFFD;
        return OK;
    }
    uint32_t cp = (uint32_t)c & (0x3F >> need);
    for (int i = 0; i < need; i++) {
        int b = nungot ? ungot[--nungot] : read_byte(ESCDELAY > 100 ? ESCDELAY : 100);
        if (b < 0 || (b & 0xC0) != 0x80) {
            if (b >= 0) {
                unsigned char u = (unsigned char)b;
                unread(&u, 1);
            }
            *wch = 0xFFFD;
            return OK;
        }
        cp = cp << 6 | ((uint32_t)b & 0x3F);
    }
    *wch = (wint_t)cp;
    return OK;
}

int ungetch(int ch)
{
    if (nungot == (int)(sizeof(ungot) / sizeof(ungot[0])))
        return ERR;
    ungot[nungot++] = ch;
    return OK;
}

int flushinp(void)
{
    npend = nungot = nmevents = 0;
    while (uart_has_input())
        uart_getc();
    return OK;
}

mmask_t mousemask(mmask_t newmask, mmask_t *oldmask)
{
    if (oldmask)
        *oldmask = mouse_mask;
    mouse_mask = newmask;
    if (active) {
        outs("\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l");
        if (newmask)
            outs(newmask & REPORT_MOUSE_POSITION ? "\x1b[?1000h\x1b[?1003h\x1b[?1006h" : "\x1b[?1000h\x1b[?1006h");
        flush_out();
    }
    return newmask;
}

int mouseinterval(int erval)
{
    int prev = mouse_interval;
    if (erval >= 0)
        mouse_interval = erval;     // クリックは作らないので、覚えておくだけ
    return prev;
}

bool has_mouse(void)
{
    return true;
}

int getmouse(MEVENT *ev)
{
    if (!nmevents || !ev)
        return ERR;
    *ev = mevents[0];
    memmove(mevents, mevents + 1, sizeof(MEVENT) * (size_t)--nmevents);
    return OK;
}

int ungetmouse(MEVENT *ev)
{
    if (!ev || nmevents == (int)(sizeof(mevents) / sizeof(mevents[0])))
        return ERR;
    memmove(mevents + 1, mevents, sizeof(MEVENT) * (size_t)nmevents++);
    mevents[0] = *ev;
    return ungetch(KEY_MOUSE);
}

const char *keyname(int c)
{
    static const struct { int code; const char *name; } names[] = {
        {KEY_DOWN, "KEY_DOWN"}, {KEY_UP, "KEY_UP"}, {KEY_LEFT, "KEY_LEFT"}, {KEY_RIGHT, "KEY_RIGHT"},
        {KEY_HOME, "KEY_HOME"}, {KEY_END, "KEY_END"}, {KEY_BACKSPACE, "KEY_BACKSPACE"},
        {KEY_DC, "KEY_DC"}, {KEY_IC, "KEY_IC"}, {KEY_NPAGE, "KEY_NPAGE"}, {KEY_PPAGE, "KEY_PPAGE"},
        {KEY_ENTER, "KEY_ENTER"}, {KEY_BTAB, "KEY_BTAB"}, {KEY_SF, "KEY_SF"}, {KEY_SR, "KEY_SR"},
        {KEY_SLEFT, "KEY_SLEFT"}, {KEY_SRIGHT, "KEY_SRIGHT"}, {KEY_SHOME, "KEY_SHOME"}, {KEY_SEND, "KEY_SEND"},
        {KEY_SDC, "KEY_SDC"}, {KEY_SIC, "KEY_SIC"}, {KEY_SNEXT, "KEY_SNEXT"}, {KEY_SPREVIOUS, "KEY_SPREVIOUS"},
        {KEY_MOUSE, "KEY_MOUSE"}, {KEY_RESIZE, "KEY_RESIZE"}, {KEY_B2, "KEY_B2"}, {KEY_BEG, "KEY_BEG"},
    };
    static char buf[16];
    if (c < 0)
        return nullptr;
    if (c < 0x20 || c == 0x7F) {
        snprintf(buf, sizeof(buf), "^%c", c == 0x7F ? '?' : c + 0x40);
        return buf;
    }
    if (c < 0x80) {
        snprintf(buf, sizeof(buf), "%c", c);
        return buf;
    }
    if (c < 0x100) {
        snprintf(buf, sizeof(buf), "M-%c", c - 0x80);
        return buf;
    }
    if (c >= KEY_F0 && c < KEY_F0 + 64) {
        snprintf(buf, sizeof(buf), "KEY_F(%d)", c - KEY_F0);
        return buf;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].code == c)
            return names[i].name;
    return "UNKNOWN KEY";
}

// ---- 入れ子のアプリ（system()）: 外側のアプリの curses の状態を退避する ----
// 子のアプリ（less・mi など）も curses を使うので、子は空の状態から始め、終わったら外側のものに戻す。
// 外側のアプリはふつう endwin してから system() を呼び、戻ったら refresh する（ncurses の作法。endwin の後の
// wrefresh は画面全体を描き直す）。ここに挙げるのは、このファイルの状態のすべて（足したら、ここにも足す）
#define CURSES_STATE(X)                                                                                            \
    X(std_win) X(cur_win) X(stdscr) X(curscr) X(LINES) X(COLS) X(COLORS) X(COLOR_PAIRS) X(ESCDELAY) X(started)     \
    X(active) X(raw_mode) X(keypad_sent) X(cursor_vis) X(colors_on) X(pair_fg) X(pair_bg) X(mouse_mask)           \
    X(mouse_interval) X(term_y) X(term_x) X(term_attr) X(dirty) X(obuf) X(olen) X(ocap) X(keys) X(nkeys)          \
    X(capkeys) X(keys_ready) X(pend) X(npend) X(ungot) X(nungot) X(mevents) X(nmevents) X(last_button) X(nl_mode)

#define CURSES_FIELD(v) typeof(v) v;
#define CURSES_SAVE(v) memcpy(&s->v, &v, sizeof v);
#define CURSES_LOAD(v) memcpy(&v, &s->v, sizeof v);

typedef struct {
    CURSES_STATE(CURSES_FIELD)
} CursesState;

#define CURSES_NEST_MAX 8
static CursesState saved_states[CURSES_NEST_MAX];
static int nsaved;

bool curses_push(void)
{
    if (nsaved == CURSES_NEST_MAX)
        return false;
    CursesState *s = &saved_states[nsaved++];
    CURSES_STATE(CURSES_SAVE)
    // 子は initscr の前の状態から（静的な変数の初期値と同じ）
    std_win = cur_win = (struct monocs_window){0};
    stdscr = curscr = nullptr;
    started = active = raw_mode = keypad_sent = colors_on = dirty = false;
    cursor_vis = 1;
    mouse_mask = 0;
    mouse_interval = 166;
    term_y = term_x = -1;
    term_attr = 0;
    ESCDELAY = 1000;
    obuf = nullptr;
    olen = ocap = 0;
    keys = nullptr;
    nkeys = capkeys = 0;
    keys_ready = false;
    npend = nungot = nmevents = last_button = 0;
    nl_mode = true;
    return true;
}

void curses_pop(void)
{
    if (nsaved == 0)
        return;
    curses_app_exit();      // 子が残したもの（endwin を呼ばずに終わった画面など）を片付けてから
    CursesState *s = &saved_states[--nsaved];
    CURSES_STATE(CURSES_LOAD)
}

// ncurses の def_prog_mode・reset_prog_mode: 端末のモードを覚えて戻す（system() の前後で使う）。
// この curses は、endwin の後の refresh で raw・keypad・マウスを入れ直すので、覚えることはない
int def_prog_mode(void)
{
    return OK;
}

int reset_prog_mode(void)
{
    return started ? OK : ERR;
}
