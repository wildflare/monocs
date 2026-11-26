// C の Web のページ: ヘビのゲーム。矢印キー（か hjkl）で動かし、* を食べると伸びる。q か、ぶつかった後のキーで目次へ戻る
#include <curses.h>
#include <cweb.h>
#include <stdlib.h>
#include <time.h>

#define MAXLEN 1024

int main(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    srand((unsigned)time(nullptr));
    int h = LINES - 2, w = COLS - 2;        // 枠の内側
    static int sy[MAXLEN], sx[MAXLEN];
    int len = 4, dy = 0, dx = 1, score = 0;
    for (int i = 0; i < len; i++)
        sy[i] = h / 2, sx[i] = w / 2 - i;
    int fy = rand() % h, fx = rand() % w;
    timeout(120);
    for (;;) {
        erase();
        box(stdscr, 0, 0);
        mvprintw(0, 2, " Snake  score %d  q: back ", score);
        mvaddch(fy + 1, fx + 1, '*');
        for (int i = 0; i < len; i++)
            mvaddch(sy[i] + 1, sx[i] + 1, i ? 'o' : '@');
        refresh();
        int c = getch();
        if (c == 'q')
            break;
        if ((c == KEY_UP || c == 'k') && dy == 0) dy = -1, dx = 0;
        if ((c == KEY_DOWN || c == 'j') && dy == 0) dy = 1, dx = 0;
        if ((c == KEY_LEFT || c == 'h') && dx == 0) dy = 0, dx = -1;
        if ((c == KEY_RIGHT || c == 'l') && dx == 0) dy = 0, dx = 1;
        int ny = sy[0] + dy, nx = sx[0] + dx;
        bool dead = ny < 0 || ny >= h || nx < 0 || nx >= w;
        for (int i = 0; i < len - 1 && !dead; i++)
            dead = sy[i] == ny && sx[i] == nx;
        if (dead) {
            mvprintw(h / 2, w / 2 - 8, " GAME OVER (%d) ", score);
            refresh();
            timeout(-1);
            napms(500);
            flushinp();
            getch();
            break;
        }
        bool eat = ny == fy && nx == fx;
        if (eat && len < MAXLEN)
            len++, score += 10, fy = rand() % h, fx = rand() % w;
        for (int i = len - 1; i > 0; i--)
            sy[i] = sy[i - 1], sx[i] = sx[i - 1];
        sy[0] = ny, sx[0] = nx;
    }
    endwin();
    cweb_back();
}
