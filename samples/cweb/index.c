// C の Web の最初のページ（docs/cweb.md）。cweb /usr/share/samples/cweb/index.c
// ページはサンドボックスで動く C のプログラム。cweb_go でリンクへ移り、cweb_back で戻る
#include <curses.h>
#include <cweb.h>
#include <string.h>

static const struct { const char *title, *desc, *link; } pages[] = {
    { "Snake",          "ヘビのゲーム（矢印キー）",                   "snake.c" },
    { "Life",           "ライフゲーム（セル・オートマトン）",         "life.c" },
    { "About C Web",    "C の Web とは",                              "about.c" },
};
#define N ((int)(sizeof pages / sizeof pages[0]))

int main(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_BLACK, COLOR_CYAN);
    int sel = 0;
    for (;;) {
        erase();
        box(stdscr, 0, 0);
        attron(COLOR_PAIR(1) | A_BOLD);
        mvprintw(1, 3, "Monocs C Web");
        attroff(COLOR_PAIR(1) | A_BOLD);
        mvprintw(2, 3, "ページは C のプログラム。サンドボックスで動き、OS とファイルには触れません");
        for (int i = 0; i < N; i++) {
            if (i == sel)
                attron(COLOR_PAIR(2));
            mvprintw(4 + 2 * i, 5, " %-14s ", pages[i].title);
            if (i == sel)
                attroff(COLOR_PAIR(2));
            mvprintw(4 + 2 * i, 23, "%s", pages[i].desc);
        }
        mvprintw(LINES - 2, 3, "↑↓: 選ぶ  Enter: 開く  q: 終わる  （どのページでも Ctrl-C で戻る、Ctrl-\\ で終わる）");
        refresh();
        int c = getch();
        if (c == KEY_UP || c == 'k')
            sel = (sel + N - 1) % N;
        else if (c == KEY_DOWN || c == 'j')
            sel = (sel + 1) % N;
        else if (c == '\n' || c == KEY_ENTER || c == ' ') {
            endwin();
            cweb_go(pages[sel].link);
        } else if (c == 'q') {
            endwin();
            return 0;
        }
    }
}
