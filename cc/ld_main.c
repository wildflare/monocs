// slimld: Monocs のカーネル用の静的リンカ（Linux で使うコマンド。本体は ld.c）
//
//   slimld [-v] [-Map FILE] -o kernel8.img FILE.o... LIB.a...
#include "ld.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  const char **inputs = calloc(argc + 1, sizeof(char *));
  int n = 0;
  SlimldOptions opt = {0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc)
      opt.output = argv[++i];
    else if (!strcmp(argv[i], "-Map") && i + 1 < argc)
      opt.map = argv[++i];
    else if (!strcmp(argv[i], "-v"))
      opt.verbose = true;
    else if (argv[i][0] == '-') {
      fprintf(stderr, "slimld: unknown option: %s\n", argv[i]);
      return 2;
    } else
      inputs[n++] = argv[i];
  }
  if (!opt.output || !n) {
    fprintf(stderr, "usage: slimld [-v] [-Map FILE] -o kernel8.img FILE.o... LIB.a...\n");
    return 2;
  }
  opt.inputs = inputs;
  int r = slimld_link(&opt);
  free(inputs);
  return r;
}
