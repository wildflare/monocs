// host 名前...: 名前から IP アドレスを引く（DNS）。Monocs に付いてくるアプリ（/bin）。出力は Linux の host と同じ形
#include <stdio.h>
#include <monocs.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: host NAME...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char ip[16];
        if (monocs_resolve(argv[i], ip, sizeof ip, 5000) == 0) {
            printf("%s has address %s\n", argv[i], ip);
        } else {
            printf("Host %s not found\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
