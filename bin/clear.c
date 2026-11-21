// clear: 画面を消す（スクロールで戻れる分も。Linux の clear と同じ）。Monocs に付いてくるアプリ（/bin）
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    printf("\x1b[H\x1b[2J\x1b[3J");     // 画面と、スクロールで戻れる分も消す（Linux の clear と同じ）
    return 0;
}
