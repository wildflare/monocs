// エラトステネスのふるい（配列と整数演算）
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  int n = 20000000, count = 0;
  char *sieve = calloc(n + 1, 1);
  for (int rep = 0; rep < 3; rep++) {
    count = 0;
    for (int i = 2; i <= n; i++) sieve[i] = 1;
    for (int i = 2; (long)i * i <= n; i++)
      if (sieve[i])
        for (int j = i * i; j <= n; j += i) sieve[j] = 0;
    for (int i = 2; i <= n; i++) count += sieve[i];
  }
  printf("%d\n", count);
  return 0;
}
