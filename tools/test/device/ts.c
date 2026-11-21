#include <stdio.h>
#include <monocs.h>
int main(void){int r=0,c=0;bool ok=monocs_term_size(&r,&c);printf("ok=%d rows=%d cols=%d\n",ok,r,c);return 0;}
