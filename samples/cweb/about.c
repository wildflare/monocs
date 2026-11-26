// C の Web のページ: 説明。b か q で目次へ戻る
#include <curses.h>
#include <cweb.h>

static const char *const text[] = {
    "C の Web とは",
    "",
    "Web のページの代わりに、C のプログラムを URL でつなぐ小さな Web です。",
    "ページを開くと、Monocs がその C のソースを取ってきて、その場でコンパイルし、",
    "サンドボックス（EL0）の中で動かします。",
    "",
    "・ページは画面・キー・時刻・乱数しか使えません。ファイルもネットワークも触れないので、",
    "  見ている人のことを外に送れません（広告やトラッキングが作りの上で入りにくい）。",
    "・リンクに #sha256= を書けば、移った先の中身を確かめます。最初のページのハッシュを",
    "  信頼すれば、たどる先も作った人が意図したとおりのページだとわかります。",
    "・検索はわざとありません。見つけ方はリンクと、知り合いからの「これ面白いよ」です。",
    "",
    "ページの書き方: #include <cweb.h> して、",
    "  cweb_go(\"next.c\") で次へ、cweb_back() で戻る（見る人は Ctrl-C でも戻れる）。",
    "ほかの Monocs に見せるには /root/share に置いて、",
    "  cweb http://その Monocs のアドレス/root/share/… で開きます。",
};

int main(void)
{
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    erase();
    box(stdscr, 0, 0);
    for (int i = 0; i < (int)(sizeof text / sizeof text[0]) && i + 2 < LINES - 2; i++)
        mvprintw(i + 1, 3, "%s", text[i]);
    mvprintw(LINES - 2, 3, "b: 戻る");
    refresh();
    int c;
    while ((c = getch()) != 'b' && c != 'q')
        ;
    endwin();
    cweb_back();
}
