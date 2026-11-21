#include <stdio.h>
int main(void) {
    for (int i = 0; i < 3200; i++)
        printf("%05d 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU\n", i);
    return 0;
}
