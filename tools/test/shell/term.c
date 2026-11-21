// SIGTERM を関数で受けて 3 で終わる（timeout の試験）
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void on_term(int sig)
{
    (void)sig;
    printf("got TERM\n");
    fflush(stdout);
    exit(3);
}

int main(void)
{
    signal(SIGTERM, on_term);
    for (int i = 0; i < 100; i++)
        sleep(1);
    return 0;
}
