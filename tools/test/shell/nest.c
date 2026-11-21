// system() と timeout の入れ子（tools/test/shell.sh）
//   inner: system() の中の timeout が、子（spin.c）だけを止め、親は続く
//   outer: 外の timeout（timeout 2 cc -o nest nest.c && ./nest outer）が、子と親の両方を止める（after は出ない）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "inner") == 0) {
        int st = system("timeout 1 ./spin");
        printf("inner status %d, parent goes on\n", st >> 8);
        return 0;
    }
    system("./spin");
    printf("after\n");
    return 0;
}
