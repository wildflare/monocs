// C の Web のページ: ライフゲーム（Conway）。r: 並べ直す、q: 目次へ戻る
#include <curses.h>
#include <cweb.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXW 300
#define MAXH 100
static unsigned char cell[2][MAXH][MAXW];

static void seed(int h, int w)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            cell[0][y][x] = rand() % 4 == 0;
}

int main(void)
{
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    srand((unsigned)time(nullptr));
    int h = LINES - 1 < MAXH ? LINES - 1 : MAXH, w = COLS < MAXW ? COLS : MAXW;
    seed(h, w);
    timeout(80);
    for (int gen = 0, cur = 0;; gen++, cur ^= 1) {
        erase();
        int alive = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (cell[cur][y][x]) {
                    mvaddch(y, x, '#');
                    alive++;
                }
        mvprintw(h, 0, " Life  generation %d  alive %d   r: reseed  q: back ", gen, alive);
        refresh();
        int c = getch();
        if (c == 'q')
            break;
        if (c == 'r') {
            seed(h, w);
            cur = 1;        // 次の回で cur ^= 1 して 0（並べ直したもの）を描く
            gen = -1;
            memcpy(cell[1], cell[0], sizeof cell[0]);
            continue;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if ((dy || dx) && cell[cur][(y + dy + h) % h][(x + dx + w) % w])
                            n++;
                cell[cur ^ 1][y][x] = n == 3 || (n == 2 && cell[cur][y][x]);
            }
    }
    endwin();
    cweb_back();
}
