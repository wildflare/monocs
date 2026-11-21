// SIGWINCH の試験（compat_dev.sh が telnet でつないで、画面の大きさを変える）
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
static volatile sig_atomic_t n;
static void on_winch(int s) { (void)s; n++; }
int main(void)
{
    signal(SIGWINCH, on_winch);
    printf("winch ready\n");
    fflush(stdout);
    for (int i = 0; i < 100 && !n; i++)
        usleep(100000);
    struct winsize ws;
    ioctl(1, TIOCGWINSZ, &ws);
    printf("winch got %d size %dx%d\n", (int)n, ws.ws_row, ws.ws_col);
    return 0;
}
