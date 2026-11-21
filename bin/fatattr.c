// fatattr [+-ahrs]... FILE...: FAT の属性を見る・変える。Monocs に付いてくるアプリ（/bin）。Linux の fatattr
// （fatattr パッケージ）と同じ書き方: + で立てる、- で消す。a アーカイブ、h 隠し、r 読み出し専用、s システム。
// 属性を書かなければ、今の属性を表示する（「ahrs」の並び。立っていないところは -）。
// 隠し属性のファイルは、ls では -a のときだけ出る（Windows と同じ）。
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <monocs.h>

static int bit(char c)
{
    return c == 'a' ? MONOCS_FAT_ARCHIVE : c == 'h' ? MONOCS_FAT_HIDDEN : c == 'r' ? MONOCS_FAT_READONLY
         : c == 's' ? MONOCS_FAT_SYSTEM : 0;
}

int main(int argc, char **argv)
{
    int set = 0, clear = 0, nfiles = 0, status = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '+' || a[0] == '-') && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                int b = bit(*p);
                if (!b) {
                    fprintf(stderr, "fatattr: unknown attribute '%c' (a, h, r, s)\n", *p);
                    return 2;
                }
                if (a[0] == '+')
                    set |= b;
                else
                    clear |= b;
            }
            continue;
        }
        nfiles++;
    }
    if (nfiles == 0) {
        fprintf(stderr, "Usage: fatattr [+-ahrs]... FILE...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '+' || a[0] == '-') && a[1])
            continue;
        int r = monocs_fat_attr(a, set, clear);
        if (r < 0) {
            fprintf(stderr, "fatattr: %s: %s\n", a, strerror(errno));
            status = 1;
            continue;
        }
        printf("%c%c%c%c  %s%s\n", r & MONOCS_FAT_ARCHIVE ? 'a' : '-', r & MONOCS_FAT_HIDDEN ? 'h' : '-',
               r & MONOCS_FAT_READONLY ? 'r' : '-', r & MONOCS_FAT_SYSTEM ? 's' : '-', a, r & MONOCS_FAT_DIR ? "/" : "");
    }
    return status;
}
