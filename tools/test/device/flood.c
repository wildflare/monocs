// USB シリアルの試験（hwcheck.sh）: 約 10MB を出力する。途中で Pi 5 が読むのをやめても、後で USB シリアルが戻ること
#include <stdio.h>
int main(void){for(int i=0;i<200000;i++)printf("line %d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n",i);return 0;}
