// passwd [-d] [ユーザー名]: 内蔵 Wi-Fi から HTTP・telnet を使うときのユーザー名とパスワードを決める。
// Monocs に付いてくるアプリ（/bin）。
//   passwd            ユーザー名は今のもの（なければ monocs）のまま、パスワードを決め直す
//   passwd 名前       ユーザー名も変える（ユーザーは 1 人だけ）
//   passwd -d         消す（Wi-Fi からの HTTP・telnet を断るようになる）
// 保存するのは /etc/htdigest の 1 行（ユーザー名:Monocs:MD5。パスワードそのものは保存しない。monocs_auth.h）。
// USB から（ケーブルの先の Pi 5）は、パスワードなしで使える（今までどおり）。
// 打ったパスワードは表示しない（Linux の passwd と同じ。termios で ECHO を外す）。長さは問わない（空でもよい。
// 2026-11-21 にユーザーの希望で 8 文字以上の決まりを外した）。
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <monocs_auth.h>

static bool ask(const char *prompt, char *buf, int size)
{
    struct termios old, t;
    bool tty = tcgetattr(0, &old) == 0;
    if (tty) {
        t = old;
        t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSAFLUSH, &t);
    }
    fputs(prompt, stderr);
    fflush(stderr);
    bool ok = fgets(buf, size, stdin) != nullptr;
    if (tty) {
        tcsetattr(0, TCSAFLUSH, &old);
        fputc('\n', stderr);
    }
    if (ok)
        buf[strcspn(buf, "\r\n")] = 0;
    return ok;
}

int main(int argc, char **argv)
{
    char user[64] = "monocs", ha1[33], old_user[64];
    if (monocs_auth_load(old_user, ha1))
        strcpy(user, old_user);
    if (argc > 1 && !strcmp(argv[1], "-d")) {
        if (unlink(MONOCS_AUTH_FILE) != 0 && access(MONOCS_AUTH_FILE, F_OK) == 0) {
            perror("passwd: " MONOCS_AUTH_FILE);
            return 1;
        }
        printf("passwd: password removed (access over Wi-Fi is now refused)\n");
        return 0;
    }
    if (argc > 1) {
        if (argv[1][0] == '-' || strlen(argv[1]) >= sizeof user || strchr(argv[1], ':') || strchr(argv[1], '"')) {
            fprintf(stderr, "Usage: passwd [-d] [USER]   (USER: up to 63 characters, without ':' and '\"')\n");
            return 1;
        }
        strcpy(user, argv[1]);
    }
    printf("Changing the password for %s (HTTP and telnet over Wi-Fi).\n", user);
    char p1[128], p2[128];
    if (!ask("New password: ", p1, sizeof p1) || !ask("Retype new password: ", p2, sizeof p2))
        return 1;
    if (strcmp(p1, p2)) {
        fprintf(stderr, "passwd: Sorry, passwords do not match.\n");
        return 1;
    }
    monocs_auth_ha1(user, p1, ha1);
    memset(p1, 0, sizeof p1);
    memset(p2, 0, sizeof p2);
    mkdir("/etc", 0755);
    FILE *f = fopen(MONOCS_AUTH_FILE ".tmp", "w");
    if (!f || fprintf(f, "%s:%s:%s\n", user, MONOCS_AUTH_REALM, ha1) < 0 || fclose(f) != 0 ||
        rename(MONOCS_AUTH_FILE ".tmp", MONOCS_AUTH_FILE) != 0) {
        perror("passwd: " MONOCS_AUTH_FILE);
        return 1;
    }
    printf("passwd: password updated successfully\n");
    return 0;
}
