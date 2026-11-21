// open_memstream の試験（os/libc/memstream.c）。Linux（glibc）と同じ出力になること
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long fnv(const char *p, size_t n)
{
    unsigned long h = 1469598103934665603UL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)p[i]) * 1099511628211UL;
    return h;
}

int main(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    fputs("hello", f);
    fflush(f);
    printf("1 len %zu [%s]\n", len, buf);
    fprintf(f, " %d %s %.2f", 42, "x", 1.5);
    long res = ftell(f) + 1;
    fprintf(f, "\t%*s\n", 10, "");          // 行を予約する（codegen.c の resrvln と同じ）
    fputc('A', f);
    fwrite("BCDEF", 1, 5, f);
    long cur = ftell(f);
    fseek(f, res, SEEK_SET);                // 予約した行に書く（insrtln）
    fputs("mov x0", f);
    fseek(f, cur, SEEK_SET);
    fputs("|end", f);
    fflush(f);
    printf("2 len %zu ftell %ld [%s] nul %d\n", len, ftell(f), buf, buf[len] == 0);
    for (int i = 0; i < 100000; i++)
        fprintf(f, "line %d %s\n", i, i % 3 ? "abc" : "defghij");
    fclose(f);
    printf("3 len %zu hash %lx nul %d\n", len, fnv(buf, len), buf[len] == 0);
    free(buf);

    // 終わりより先へ動かして書く: 間は 0
    f = open_memstream(&buf, &len);
    fputs("ab", f);
    fseek(f, 6, SEEK_SET);
    fputs("cd", f);
    fclose(f);
    printf("4 len %zu:", len);
    for (size_t i = 0; i < len; i++)
        printf(" %02x", (unsigned char)buf[i]);
    printf("\n");
    free(buf);

    // 空のまま閉じる、大きな fwrite
    f = open_memstream(&buf, &len);
    fclose(f);
    printf("5 len %zu [%s]\n", len, buf);
    free(buf);
    f = open_memstream(&buf, &len);
    char *big = malloc(300000);
    for (int i = 0; i < 300000; i++)
        big[i] = 'a' + i % 26;
    fwrite(big, 1, 300000, f);
    fwrite(big, 1, 7, f);
    fclose(f);
    printf("6 len %zu hash %lx\n", len, fnv(buf, len));
    free(big);
    free(buf);
    return 0;
}
