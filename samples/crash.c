// 例外からシェルへ戻ることを確かめる（NULL を書き換える）
#include <stdio.h>

int main(void)
{
    printf("writing to NULL...\n");
    *(volatile int *)0 = 42;
    printf("not reached\n");
    return 0;
}
