// ほかのコアから C ライブラリを呼ぶ例（マルチコア）
// 4 つのコアが同時に malloc・free、snprintf、ファイルの書き込みと読み出しをして、結果を確かめる。
// 共有する変数は monocs_mutex で守る
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs.h>

#define ROUNDS 2000

static monocs_mutex lock = MONOCS_MUTEX_INIT;
static int lines_total;  // すべてのコアが書いた行の数（lock で守る）

struct job {
    int core;
    int errors;
};

static void work(void *arg)
{
    struct job *j = arg;
    char path[32];
    snprintf(path, sizeof(path), "/mc%d.txt", j->core);

    // malloc と free を混ぜて繰り返し、書いた内容が壊れていないか確かめる
    void *keep[16] = {0};
    for (int i = 0; i < ROUNDS; i++) {
        int k = i % 16;
        if (keep[k]) {
            unsigned char *p = keep[k];
            size_t n = p[0] * 8 + 1;
            for (size_t m = 1; m < n; m++)
                if (p[m] != (unsigned char)(j->core + m))
                    j->errors++;
            free(keep[k]);
        }
        size_t n = (i * 37 % 200) * 8 + 1;
        unsigned char *p = malloc(n);
        if (!p) {
            j->errors++;
            continue;
        }
        p[0] = (unsigned char)(n / 8);
        for (size_t m = 1; m < n; m++)
            p[m] = (unsigned char)(j->core + m);
        keep[k] = p;
    }
    for (int k = 0; k < 16; k++)
        free(keep[k]);

    // ファイルに書いて読み戻す
    FILE *f = fopen(path, "w");
    if (!f) {
        j->errors++;
        return;
    }
    for (int i = 0; i < 200; i++) {
        fprintf(f, "core %d line %d\n", j->core, i);
        monocs_mutex_lock(&lock);
        lines_total++;
        monocs_mutex_unlock(&lock);
    }
    fclose(f);
    f = fopen(path, "r");
    if (!f) {
        j->errors++;
        return;
    }
    char line[64], want[64];
    for (int i = 0; i < 200; i++) {
        snprintf(want, sizeof(want), "core %d line %d\n", j->core, i);
        if (!fgets(line, sizeof(line), f) || strcmp(line, want) != 0)
            j->errors++;
    }
    fclose(f);
    remove(path);
    printf("core %d: done\n", j->core);
}

int main(void)
{
    int cores = monocs_cores();
    struct job jobs[4] = {0};
    for (int i = 1; i < cores; i++) {
        jobs[i].core = i;
        monocs_core_start(i, work, &jobs[i]);
    }
    work(&jobs[0]);
    int errors = jobs[0].errors;
    for (int i = 1; i < cores; i++) {
        if (!monocs_core_wait(i))
            errors++;
        errors += jobs[i].errors;
    }
    if (lines_total != 200 * cores)
        errors++;
    printf("%d cores, lines: %d, errors: %d\n", cores, lines_total, errors);
    return errors != 0;
}
