// SIGTERM を無視して、5 秒眠ってから 5 で終わる（timeout -k の試験）
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    signal(SIGTERM, SIG_IGN);
    sleep(5);
    printf("ignore finished\n");
    return 5;
}
