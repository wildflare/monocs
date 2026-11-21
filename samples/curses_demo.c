// curses の例: 枠と色と日本語を描き、押したキーとマウスを表示する（q で終わる）
//   cc -o curses_demo samples/curses_demo.c && ./curses_demo
// Monocs の curses は ncurses 互換の小さなもの（/include/curses.h）。raw() の間は Ctrl-C もキーとして届き、
// アプリを止めるのは Ctrl-\。
#include <curses.h>
#include <stdio.h>

int main(void)
{
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_YELLOW, -1);
        init_pair(2, COLOR_WHITE, COLOR_BLUE);
    }

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    mvaddch(0, 0, ACS_ULCORNER);
    hline(ACS_HLINE, cols - 2);
    mvaddch(0, cols - 1, ACS_URCORNER);
    for (int y = 1; y < rows - 1; y++) {
        mvaddch(y, 0, ACS_VLINE);
        mvaddch(y, cols - 1, ACS_VLINE);
    }
    mvaddch(rows - 1, 0, ACS_LLCORNER);
    hline(ACS_HLINE, cols - 2);
    mvaddch(rows - 1, cols - 1, ACS_LRCORNER);

    attron(COLOR_PAIR(2) | A_BOLD);
    mvprintw(1, 2, " Monocs curses %dx%d ", cols, rows);
    attroff(COLOR_PAIR(2) | A_BOLD);
    attron(COLOR_PAIR(1));
    mvaddstr(3, 2, "日本語も書けます（全角は 2 桁）");
    attroff(COLOR_PAIR(1));
    mvaddstr(4, 2, "キーを押すと表示します。q で終わります");

    int n = 0;
    for (;;) {
        move(6, 2);
        clrtoeol();
        mvaddch(6, cols - 1, ACS_VLINE);
        move(6, 2);
        refresh();
        wint_t wc;
        int r = get_wch(&wc);
        if (r == ERR)
            continue;
        move(6, 2);
        if (r == KEY_CODE_YES && wc == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK)
                mvprintw(8, 2, "mouse x=%d y=%d bstate=%#lx      ", ev.x, ev.y, (unsigned long)ev.bstate);
        } else if (r == KEY_CODE_YES) {
            mvprintw(8, 2, "key %s (%#o)                       ", keyname((int)wc), (unsigned)wc);
            if (wc == KEY_RESIZE) {
                getmaxyx(stdscr, rows, cols);
                mvprintw(1, 2, " Monocs curses %dx%d ", cols, rows);
            }
        } else {
            if (wc == 'q')
                break;
            char buf[8];
            if (wc < 0x80)
                snprintf(buf, sizeof(buf), "%s", keyname((int)wc));
            else
                snprintf(buf, sizeof(buf), "U+%04X", (unsigned)wc);
            mvprintw(8, 2, "char %s                              ", buf);
        }
        mvprintw(9, 2, "%d", ++n);
    }
    endwin();
    printf("curses_demo: %d keys\n", n);
    return 0;
}
