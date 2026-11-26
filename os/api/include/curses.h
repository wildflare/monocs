// Monocs の curses（ncurses 互換の小さなもの。実装は os/curses/curses.c、関数は OS API 表で渡す）
//
// 端末（picocom・telnet などの xterm 互換）にエスケープシーケンスを送って描く。使えるのは stdscr だけ
// （newwin などの窓はない）。文字は UTF-8（ncursesw と同じく、addch に 1 バイトずつ渡してもよい）。
// 定数（キー、属性、マウス）の値は ncurses 6（NCURSES_MOUSE_VERSION 2）と同じ。
// ncurses と違うところ:
//   - マウスのクリック・ダブルクリックは作らない（いつも mouseinterval(0) と同じ。押した・離したが届く）
//   - 画面の大きさの変更（KEY_RESIZE）は telnet（NAWS）のときだけわかる
//   - raw() の間は Ctrl-C がキーとして届き、Ctrl-\ でアプリが止まる（Monocs のシェルに戻る）
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>     // ncurses と同じく読み込む（Linux のプログラムは、curses.h だけで printf などを使うことがある）
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define NCURSES_VERSION         "6.4"
#define NCURSES_VERSION_MAJOR   6
#define NCURSES_VERSION_MINOR   4
#define NCURSES_MOUSE_VERSION   2
#define NCURSES_WIDECHAR        1
#define MONOCS_CURSES            1

#ifndef TRUE
#define TRUE    1
#define FALSE   0
#endif
#define OK      0
#define ERR     (-1)

typedef unsigned int chtype;
typedef chtype attr_t;
typedef unsigned long mmask_t;
typedef struct monocs_window WINDOW;

typedef struct {
    short id;
    int x, y, z;
    mmask_t bstate;
} MEVENT;

extern WINDOW *stdscr;
extern WINDOW *curscr;
extern int LINES, COLS;
extern int COLORS, COLOR_PAIRS;
extern int ESCDELAY;

// ---- 属性（chtype の上位。下位 8 ビットは文字） ----
#define NCURSES_ATTR_SHIFT  8
#define NCURSES_BITS(mask, shift)   ((chtype)(mask) << ((shift) + NCURSES_ATTR_SHIFT))
#define A_NORMAL        0U
#define A_ATTRIBUTES    NCURSES_BITS(~0U, 0)
#define A_CHARTEXT      (NCURSES_BITS(1U, 0) - 1U)
#define A_COLOR         NCURSES_BITS(0xFFU, 0)
#define A_STANDOUT      NCURSES_BITS(1U, 8)
#define A_UNDERLINE     NCURSES_BITS(1U, 9)
#define A_REVERSE       NCURSES_BITS(1U, 10)
#define A_BLINK         NCURSES_BITS(1U, 11)
#define A_DIM           NCURSES_BITS(1U, 12)
#define A_BOLD          NCURSES_BITS(1U, 13)
#define A_ALTCHARSET    NCURSES_BITS(1U, 14)
#define A_INVIS         NCURSES_BITS(1U, 15)
#define A_PROTECT       NCURSES_BITS(1U, 16)
#define A_ITALIC        NCURSES_BITS(1U, 23)
#define COLOR_PAIR(n)   (NCURSES_BITS((n), 0) & A_COLOR)
#define PAIR_NUMBER(a)  ((int)(((chtype)(a) & A_COLOR) >> NCURSES_ATTR_SHIFT))
#define WA_NORMAL       A_NORMAL
#define WA_STANDOUT     A_STANDOUT
#define WA_UNDERLINE    A_UNDERLINE
#define WA_REVERSE      A_REVERSE
#define WA_BLINK        A_BLINK
#define WA_DIM          A_DIM
#define WA_BOLD         A_BOLD
#define WA_ITALIC       A_ITALIC

#define COLOR_BLACK     0
#define COLOR_RED       1
#define COLOR_GREEN     2
#define COLOR_YELLOW    3
#define COLOR_BLUE      4
#define COLOR_MAGENTA   5
#define COLOR_CYAN      6
#define COLOR_WHITE     7

// 罫線（UTF-8 の罫線素片で描く）
#define NCURSES_ACS(c)  (A_ALTCHARSET | (chtype)(unsigned char)(c))
#define ACS_ULCORNER    NCURSES_ACS('l')
#define ACS_LLCORNER    NCURSES_ACS('m')
#define ACS_URCORNER    NCURSES_ACS('k')
#define ACS_LRCORNER    NCURSES_ACS('j')
#define ACS_LTEE        NCURSES_ACS('t')
#define ACS_RTEE        NCURSES_ACS('u')
#define ACS_BTEE        NCURSES_ACS('v')
#define ACS_TTEE        NCURSES_ACS('w')
#define ACS_HLINE       NCURSES_ACS('q')
#define ACS_VLINE       NCURSES_ACS('x')
#define ACS_PLUS        NCURSES_ACS('n')
#define ACS_DIAMOND     NCURSES_ACS('`')
#define ACS_CKBOARD     NCURSES_ACS('a')
#define ACS_DEGREE      NCURSES_ACS('f')
#define ACS_PLMINUS     NCURSES_ACS('g')
#define ACS_BULLET      NCURSES_ACS('~')
#define ACS_LARROW      NCURSES_ACS(',')
#define ACS_RARROW      NCURSES_ACS('+')
#define ACS_DARROW      NCURSES_ACS('.')
#define ACS_UARROW      NCURSES_ACS('-')
#define ACS_BLOCK       NCURSES_ACS('0')

// ---- キー（ncurses と同じ値） ----
#define KEY_CODE_YES    0400
#define KEY_MIN         0401
#define KEY_BREAK       0401
#define KEY_DOWN        0402
#define KEY_UP          0403
#define KEY_LEFT        0404
#define KEY_RIGHT       0405
#define KEY_HOME        0406
#define KEY_BACKSPACE   0407
#define KEY_F0          0410
#define KEY_F(n)        (KEY_F0 + (n))
#define KEY_DL          0510
#define KEY_IL          0511
#define KEY_DC          0512
#define KEY_IC          0513
#define KEY_EIC         0514
#define KEY_CLEAR       0515
#define KEY_EOS         0516
#define KEY_EOL         0517
#define KEY_SF          0520
#define KEY_SR          0521
#define KEY_NPAGE       0522
#define KEY_PPAGE       0523
#define KEY_STAB        0524
#define KEY_CTAB        0525
#define KEY_CATAB       0526
#define KEY_ENTER       0527
#define KEY_SRESET      0530
#define KEY_RESET       0531
#define KEY_PRINT       0532
#define KEY_LL          0533
#define KEY_A1          0534
#define KEY_A3          0535
#define KEY_B2          0536
#define KEY_C1          0537
#define KEY_C3          0540
#define KEY_BTAB        0541
#define KEY_BEG         0542
#define KEY_CANCEL      0543
#define KEY_CLOSE       0544
#define KEY_COMMAND     0545
#define KEY_COPY        0546
#define KEY_CREATE      0547
#define KEY_END         0550
#define KEY_EXIT        0551
#define KEY_FIND        0552
#define KEY_HELP        0553
#define KEY_MARK        0554
#define KEY_MESSAGE     0555
#define KEY_MOVE        0556
#define KEY_NEXT        0557
#define KEY_OPEN        0560
#define KEY_OPTIONS     0561
#define KEY_PREVIOUS    0562
#define KEY_REDO        0563
#define KEY_REFERENCE   0564
#define KEY_REFRESH     0565
#define KEY_REPLACE     0566
#define KEY_RESTART     0567
#define KEY_RESUME      0570
#define KEY_SAVE        0571
#define KEY_SBEG        0572
#define KEY_SCANCEL     0573
#define KEY_SCOMMAND    0574
#define KEY_SCOPY       0575
#define KEY_SCREATE     0576
#define KEY_SDC         0577
#define KEY_SDL         0600
#define KEY_SELECT      0601
#define KEY_SEND        0602
#define KEY_SEOL        0603
#define KEY_SEXIT       0604
#define KEY_SFIND       0605
#define KEY_SHELP       0606
#define KEY_SHOME       0607
#define KEY_SIC         0610
#define KEY_SLEFT       0611
#define KEY_SMESSAGE    0612
#define KEY_SMOVE       0613
#define KEY_SNEXT       0614
#define KEY_SOPTIONS    0615
#define KEY_SPREVIOUS   0616
#define KEY_SPRINT      0617
#define KEY_SREDO       0620
#define KEY_SREPLACE    0621
#define KEY_SRIGHT      0622
#define KEY_SRSUME      0623
#define KEY_SSAVE       0624
#define KEY_SSUSPEND    0625
#define KEY_SUNDO       0626
#define KEY_SUSPEND     0627
#define KEY_UNDO        0630
#define KEY_MOUSE       0631
#define KEY_RESIZE      0632
#define KEY_MAX         0777

// ---- マウス（NCURSES_MOUSE_VERSION 2: ボタンごとに 5 ビット） ----
#define NCURSES_MOUSE_MASK(b, m)    ((mmask_t)(m) << (((b) - 1) * 5))
#define NCURSES_BUTTON_RELEASED     001UL
#define NCURSES_BUTTON_PRESSED      002UL
#define NCURSES_BUTTON_CLICKED      004UL
#define NCURSES_DOUBLE_CLICKED      010UL
#define NCURSES_TRIPLE_CLICKED      020UL
#define BUTTON1_RELEASED        NCURSES_MOUSE_MASK(1, NCURSES_BUTTON_RELEASED)
#define BUTTON1_PRESSED         NCURSES_MOUSE_MASK(1, NCURSES_BUTTON_PRESSED)
#define BUTTON1_CLICKED         NCURSES_MOUSE_MASK(1, NCURSES_BUTTON_CLICKED)
#define BUTTON1_DOUBLE_CLICKED  NCURSES_MOUSE_MASK(1, NCURSES_DOUBLE_CLICKED)
#define BUTTON1_TRIPLE_CLICKED  NCURSES_MOUSE_MASK(1, NCURSES_TRIPLE_CLICKED)
#define BUTTON2_RELEASED        NCURSES_MOUSE_MASK(2, NCURSES_BUTTON_RELEASED)
#define BUTTON2_PRESSED         NCURSES_MOUSE_MASK(2, NCURSES_BUTTON_PRESSED)
#define BUTTON2_CLICKED         NCURSES_MOUSE_MASK(2, NCURSES_BUTTON_CLICKED)
#define BUTTON2_DOUBLE_CLICKED  NCURSES_MOUSE_MASK(2, NCURSES_DOUBLE_CLICKED)
#define BUTTON2_TRIPLE_CLICKED  NCURSES_MOUSE_MASK(2, NCURSES_TRIPLE_CLICKED)
#define BUTTON3_RELEASED        NCURSES_MOUSE_MASK(3, NCURSES_BUTTON_RELEASED)
#define BUTTON3_PRESSED         NCURSES_MOUSE_MASK(3, NCURSES_BUTTON_PRESSED)
#define BUTTON3_CLICKED         NCURSES_MOUSE_MASK(3, NCURSES_BUTTON_CLICKED)
#define BUTTON3_DOUBLE_CLICKED  NCURSES_MOUSE_MASK(3, NCURSES_DOUBLE_CLICKED)
#define BUTTON3_TRIPLE_CLICKED  NCURSES_MOUSE_MASK(3, NCURSES_TRIPLE_CLICKED)
#define BUTTON4_RELEASED        NCURSES_MOUSE_MASK(4, NCURSES_BUTTON_RELEASED)
#define BUTTON4_PRESSED         NCURSES_MOUSE_MASK(4, NCURSES_BUTTON_PRESSED)
#define BUTTON4_CLICKED         NCURSES_MOUSE_MASK(4, NCURSES_BUTTON_CLICKED)
#define BUTTON4_DOUBLE_CLICKED  NCURSES_MOUSE_MASK(4, NCURSES_DOUBLE_CLICKED)
#define BUTTON4_TRIPLE_CLICKED  NCURSES_MOUSE_MASK(4, NCURSES_TRIPLE_CLICKED)
#define BUTTON5_RELEASED        NCURSES_MOUSE_MASK(5, NCURSES_BUTTON_RELEASED)
#define BUTTON5_PRESSED         NCURSES_MOUSE_MASK(5, NCURSES_BUTTON_PRESSED)
#define BUTTON5_CLICKED         NCURSES_MOUSE_MASK(5, NCURSES_BUTTON_CLICKED)
#define BUTTON5_DOUBLE_CLICKED  NCURSES_MOUSE_MASK(5, NCURSES_DOUBLE_CLICKED)
#define BUTTON5_TRIPLE_CLICKED  NCURSES_MOUSE_MASK(5, NCURSES_TRIPLE_CLICKED)
#define BUTTON_CTRL             NCURSES_MOUSE_MASK(6, 0001UL)
#define BUTTON_SHIFT            NCURSES_MOUSE_MASK(6, 0002UL)
#define BUTTON_ALT              NCURSES_MOUSE_MASK(6, 0004UL)
#define REPORT_MOUSE_POSITION   NCURSES_MOUSE_MASK(6, 0010UL)
#define ALL_MOUSE_EVENTS        (REPORT_MOUSE_POSITION - 1)
#define BUTTON_RELEASE(e, x)        ((e) & NCURSES_MOUSE_MASK(x, 001))
#define BUTTON_PRESS(e, x)          ((e) & NCURSES_MOUSE_MASK(x, 002))
#define BUTTON_CLICK(e, x)          ((e) & NCURSES_MOUSE_MASK(x, 004))
#define BUTTON_DOUBLE_CLICK(e, x)   ((e) & NCURSES_MOUSE_MASK(x, 010))
#define BUTTON_TRIPLE_CLICK(e, x)   ((e) & NCURSES_MOUSE_MASK(x, 020))

// ---- 関数 ----
WINDOW *initscr(void);
int endwin(void);
int def_prog_mode(void);   // system() の前後で（ncurses と同じ使い方。endwin・system・reset_prog_mode・refresh）
int reset_prog_mode(void);
bool isendwin(void);

int raw(void);
int noraw(void);
int cbreak(void);
int nocbreak(void);
int echo(void);
int noecho(void);
int nl(void);
int nonl(void);
int keypad(WINDOW *win, bool bf);
int meta(WINDOW *win, bool bf);
int intrflush(WINDOW *win, bool bf);
int nodelay(WINDOW *win, bool bf);
void wtimeout(WINDOW *win, int delay);
int notimeout(WINDOW *win, bool bf);
int leaveok(WINDOW *win, bool bf);
int scrollok(WINDOW *win, bool bf);
int idlok(WINDOW *win, bool bf);
int clearok(WINDOW *win, bool bf);
int curs_set(int visibility);
int set_escdelay(int ms);
int napms(int ms);
int beep(void);
int flash(void);

int has_colors(void);
int can_change_color(void);
int start_color(void);
int use_default_colors(void);
int assume_default_colors(int fg, int bg);
int init_pair(short pair, short fg, short bg);
int pair_content(short pair, short *fg, short *bg);

int wrefresh(WINDOW *win);
int wnoutrefresh(WINDOW *win);
int doupdate(void);
int redrawwin(WINDOW *win);
int wmove(WINDOW *win, int y, int x);
int waddch(WINDOW *win, chtype ch);
int waddnstr(WINDOW *win, const char *str, int n);
int waddstr(WINDOW *win, const char *str);
int wprintw(WINDOW *win, const char *fmt, ...);
int vw_printw(WINDOW *win, const char *fmt, va_list ap);
int printw(const char *fmt, ...);
int mvprintw(int y, int x, const char *fmt, ...);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int wclrtoeol(WINDOW *win);
int wclrtobot(WINDOW *win);
int werase(WINDOW *win);
int wclear(WINDOW *win);
int whline(WINDOW *win, chtype ch, int n);
int wvline(WINDOW *win, chtype ch, int n);
int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl, chtype tr, chtype bl, chtype br);
int box(WINDOW *win, chtype verch, chtype horch);   // 枠（0 で既定の罫線。ncurses と同じ。2026-11-26）
int wattron(WINDOW *win, int attrs);
int wattroff(WINDOW *win, int attrs);
int wattrset(WINDOW *win, int attrs);
int wattr_on(WINDOW *win, attr_t attrs, void *opts);
int wattr_off(WINDOW *win, attr_t attrs, void *opts);
int wattr_get(WINDOW *win, attr_t *attrs, short *pair, void *opts);
int wattr_set(WINDOW *win, attr_t attrs, short pair, void *opts);
int wcolor_set(WINDOW *win, short pair, void *opts);
int wbkgdset(WINDOW *win, chtype ch);
chtype winch(WINDOW *win);

int getcurx(const WINDOW *win);
int getcury(const WINDOW *win);
int getmaxx(const WINDOW *win);
int getmaxy(const WINDOW *win);
int getbegx(const WINDOW *win);
int getbegy(const WINDOW *win);

int wgetch(WINDOW *win);
int wget_wch(WINDOW *win, wint_t *wch);
int ungetch(int ch);
int flushinp(void);
int define_key(const char *definition, int keycode);
int key_defined(const char *definition);
const char *keyname(int c);
int has_key(int keycode);

mmask_t mousemask(mmask_t newmask, mmask_t *oldmask);
int mouseinterval(int erval);
int getmouse(MEVENT *event);
int ungetmouse(MEVENT *event);
bool has_mouse(void);

int resizeterm(int lines, int columns);
int resize_term(int lines, int columns);
bool is_term_resized(int lines, int columns);

// stdscr を使う形（ncurses と同じくマクロ）
#define refresh()               wrefresh(stdscr)
#define move(y, x)              wmove(stdscr, (y), (x))
#define addch(ch)               waddch(stdscr, (ch))
#define addstr(s)               waddnstr(stdscr, (s), -1)
#define addnstr(s, n)           waddnstr(stdscr, (s), (n))
#define mvaddch(y, x, ch)       (wmove(stdscr, (y), (x)) == ERR ? ERR : waddch(stdscr, (ch)))
#define mvaddstr(y, x, s)       (wmove(stdscr, (y), (x)) == ERR ? ERR : waddnstr(stdscr, (s), -1))
#define mvaddnstr(y, x, s, n)   (wmove(stdscr, (y), (x)) == ERR ? ERR : waddnstr(stdscr, (s), (n)))
#define mvwaddch(w, y, x, ch)   (wmove((w), (y), (x)) == ERR ? ERR : waddch((w), (ch)))
#define mvwaddstr(w, y, x, s)   (wmove((w), (y), (x)) == ERR ? ERR : waddnstr((w), (s), -1))
#define clrtoeol()              wclrtoeol(stdscr)
#define clrtobot()              wclrtobot(stdscr)
#define erase()                 werase(stdscr)
#define clear()                 wclear(stdscr)
#define hline(ch, n)            whline(stdscr, (ch), (n))
#define vline(ch, n)            wvline(stdscr, (ch), (n))
#define border(ls, rs, ts, bs, tl, tr, bl, br) wborder(stdscr, (ls), (rs), (ts), (bs), (tl), (tr), (bl), (br))
#define mvwhline(w, y, x, ch, n) (wmove((w), (y), (x)) == ERR ? ERR : whline((w), (ch), (n)))
#define mvwvline(w, y, x, ch, n) (wmove((w), (y), (x)) == ERR ? ERR : wvline((w), (ch), (n)))
#define mvhline(y, x, ch, n)    (wmove(stdscr, (y), (x)) == ERR ? ERR : whline(stdscr, (ch), (n)))
#define mvvline(y, x, ch, n)    (wmove(stdscr, (y), (x)) == ERR ? ERR : wvline(stdscr, (ch), (n)))
#define attron(a)               wattron(stdscr, (a))
#define attroff(a)              wattroff(stdscr, (a))
#define attrset(a)              wattrset(stdscr, (a))
#define attr_on(a, o)           wattr_on(stdscr, (a), (o))
#define attr_off(a, o)          wattr_off(stdscr, (a), (o))
#define attr_get(a, p, o)       wattr_get(stdscr, (a), (p), (o))
#define attr_set(a, p, o)       wattr_set(stdscr, (a), (p), (o))
#define color_set(p, o)         wcolor_set(stdscr, (p), (o))
#define bkgdset(ch)             wbkgdset(stdscr, (ch))
#define standout()              wattron(stdscr, A_STANDOUT)
#define standend()              wattrset(stdscr, A_NORMAL)
#define inch()                  winch(stdscr)
#define getch()                 wgetch(stdscr)
#define get_wch(wch)            wget_wch(stdscr, (wch))
#define mvgetch(y, x)           (wmove(stdscr, (y), (x)) == ERR ? ERR : wgetch(stdscr))
#define timeout(delay)          wtimeout(stdscr, (delay))
#define getmaxyx(w, y, x)       ((y) = getmaxy(w), (x) = getmaxx(w))
#define getyx(w, y, x)          ((y) = getcury(w), (x) = getcurx(w))
#define getbegyx(w, y, x)       ((y) = getbegy(w), (x) = getbegx(w))
