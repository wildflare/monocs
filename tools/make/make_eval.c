// Linux で os/slimcc/makefile.c を試す: make_eval DIR MAKEFILE EXPR... は、Makefile を読んだ後に式を展開して
// 1 行ずつ表示する（test.sh が GNU make の $(info 式) と比べる）。make_eval -info DIR は MakeInfo を表示する
#include "../../os/slimcc/makefile.c"

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-info")) {
        MakeInfo mi;
        if (!make_read(argv[2], &mi)) {
            printf("no makefile\n");
            return 1;
        }
        printf("target: %s\n", mi.target);
        for (int i = 0; i < mi.nflags; i++)
            printf("flag: %s\n", mi.flags[i]);
        for (int i = 0; i < mi.nsrcs; i++)
            printf("src: %s\n", mi.srcs[i]);
        make_free(&mi);
        return 0;
    }
    if (argc < 3)
        return 2;
    char *text = read_file(argv[2]);
    if (!text)
        return 1;
    Make m = {0};
    snprintf(m.dir, sizeof m.dir, "%s", argv[1]);
    parse(&m, text);
    free(text);
    for (int i = 3; i < argc; i++) {
        char *v = expand(&m, argv[i]);
        printf("%s\n", v);
        free(v);
    }
    free_make(&m);
    return 0;
}
