// telnet サーバー（ポート 23）: シェルのコンソールを telnet でも使えるようにする。サービスとして動く
// （カーネルの外のプログラム。svc start /services/telnetd.c、起動時は /services.txt。os/kernel/svc.c）。
// コンソールの写し（monocs_console_*。os/kernel/contap.c）で、コンソールに出た文字を受け取り、
// 打った文字をシェルに渡す。出力は UART・USB・telnet のすべてに出る。同時につなげるのは 1 つまで。
//
// 出典: RFC 854（TELNET）、RFC 857（ECHO）、RFC 858（SUPPRESS-GO-AHEAD）、RFC 1073（NAWS: 画面の大きさ）、
//       RFC 1184（Enter は CR LF か CR NUL で届く）
// つないだときに、こちらが文字を表示し返す（WILL ECHO）、Go Ahead を使わない（WILL SGA）を伝えて、
// クライアントを 1 文字ずつ送る形にし、画面の大きさを知らせてもらう（DO NAWS）。
//
// 守り: USB から届いた接続（ケーブルの先の Pi 5）は、今までどおりすぐシェル。内蔵 Wi-Fi から届いた接続は、
// Linux の login のようにユーザー名とパスワードを聞く（/etc/htdigest。passwd コマンドで作る。monocs_auth.h）。
// パスワードがなければ断る。1 回の接続で 3 回まで、続けて 5 回まちがえたら 30 秒は Wi-Fi からの接続を断る。
// telnet は暗号化しないので、打ったパスワードも、その後の画面も、家の Wi-Fi の中をそのまま流れる。
#include <stdio.h>
#include <string.h>
#include <monocs.h>
#include <monocs_auth.h>


#define PORT        23

#define IAC         255
#define DONT        254
#define DO          253
#define WONT        252
#define WILL        251
#define SB          250
#define BRK         243
#define IP          244
#define SE          240
#define OPT_ECHO    1
#define OPT_SGA     3
#define OPT_NAWS    31

static int listener = -1;
static int session = -1;

// ログイン（Wi-Fi から届いた接続だけ）
#define LOGIN_TRIES     3           // 1 回の接続でまちがえてよい回数
#define MAX_FAILS       5           // 続けてまちがえてよい回数（接続をまたいで）
#define LOCK_US         30000000ULL
#define FAIL_DELAY_US   1500000ULL  // まちがえた後、次を聞くまで
#define LOGIN_IDLE_US   60000000ULL // ログインの途中で何も打たれなければ閉じる
enum { L_SHELL, L_USER, L_PASS, L_DELAY };
static int login_state;             // L_SHELL: ログイン済み（コンソールにつながっている）
static char login_user[64], login_pass[128];
static int login_len, login_tries, fails;
static uint64_t login_until, locked_until;
static bool closing;                // シェルで exit が打たれた
static int naws_cols, naws_rows;    // ログインの途中で届いた画面の大きさ（シェルにつないだときに伝える）

// 送るもの（まだ送れていない分）。コンソールの写しを IAC を 2 重にして入れる
static unsigned char out[8192];
static int out_len, out_pos;

static void send_now(const void *p, int n)
{
    monocs_tcp_send(session, p, n, 1000);
}

static void send_cmd(int cmd, int opt)
{
    unsigned char b[3] = { IAC, (unsigned char)cmd, (unsigned char)opt };
    send_now(b, 3);
}

// ---- 受信（RFC 854 のコマンドを取り除き、文字をシェルに渡す） ----

enum { S_DATA, S_CR, S_IAC, S_OPT, S_SB, S_SB_IAC };
static int rx_state, rx_cmd;
static unsigned char sb_buf[16];
static int sb_len;

// 相手からの WILL・WONT・DO・DONT
static void negotiate(int cmd, int opt)
{
    if (cmd == DO && opt != OPT_ECHO && opt != OPT_SGA)
        send_cmd(WONT, opt);    // こちらに頼む: ECHO と SGA は引き受けている。ほかは断る
    else if (cmd == WILL && opt != OPT_NAWS && opt != OPT_SGA)
        send_cmd(DONT, opt);    // 相手がやると言う: NAWS と SGA は頼んだもの。ほかは断る
}

static void subnegotiation(void)
{
    // NAWS: 桁（2 バイト）、行（2 バイト）。ビッグエンディアン
    if (sb_len == 5 && sb_buf[0] == OPT_NAWS) {
        naws_rows = sb_buf[3] << 8 | sb_buf[4];
        naws_cols = sb_buf[1] << 8 | sb_buf[2];
        if (login_state == L_SHELL)
            monocs_console_set_size(naws_rows, naws_cols);
    }
}

static void end_session(void);
static bool open_shell(void);

static void say(const char *s)
{
    send_now(s, (int)strlen(s));
}

// ログインの途中に届いた文字（1 行ずつ: ユーザー名は表示し返す、パスワードは表示しない）
static void login_input(const char *p, int n)
{
    for (int i = 0; i < n && session >= 0 && login_state != L_SHELL; i++) {
        char c = p[i];
        if (login_state == L_DELAY)
            continue;       // まちがえた後の待ちの間に打たれたものは捨てる
        char *buf = login_state == L_USER ? login_user : login_pass;
        int size = login_state == L_USER ? (int)sizeof login_user : (int)sizeof login_pass;
        login_until = timer_now_us() + LOGIN_IDLE_US;
        if (c == 0x03 || c == 0x04) {       // Ctrl-C・Ctrl-D: やめる
            end_session();
        } else if (c == 0x7f || c == 0x08) {
            if (login_len > 0) {
                login_len--;
                if (login_state == L_USER)
                    say("\b \b");
            }
        } else if (c == '\r') {
            buf[login_len] = 0;
            login_len = 0;
            say("\r\n");
            if (login_state == L_USER) {
                login_state = L_PASS;
                say("Password: ");
                continue;
            }
            char user[64], ha1[33], typed[33];
            bool ok = monocs_auth_load(user, ha1);
            monocs_auth_ha1(login_user, login_pass, typed);
            memset(login_pass, 0, sizeof login_pass);
            ok = ok && (monocs_auth_equal(login_user, user) & monocs_auth_equal(typed, ha1));
            if (ok) {
                fails = 0;
                if (!open_shell())
                    return;
            } else {
                if (++fails >= MAX_FAILS) {
                    fails = 0;
                    locked_until = timer_now_us() + LOCK_US;
                }
                login_tries++;
                login_state = L_DELAY;
                login_until = timer_now_us() + FAIL_DELAY_US;
            }
        } else if ((unsigned char)c >= 0x20 && login_len + 1 < size) {
            buf[login_len++] = c;
            if (login_state == L_USER)
                send_now(&c, 1);
        }
    }
}

// 受け取った文字を、シェル（コンソール）かログインに渡す
static void deliver(const char *p, int n)
{
    if (login_state == L_SHELL)
        monocs_console_input(p, n);
    else
        login_input(p, n);
}

static void receive(const unsigned char *p, int n)
{
    char in[512];
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = p[i];
        switch (rx_state) {
        case S_CR:  // CR の後の LF・NUL は捨てる（Enter は CR だけにする）
            rx_state = S_DATA;
            if (c == '\n' || c == 0)
                break;
            [[fallthrough]];
        case S_DATA:
            if (c == IAC) {
                rx_state = S_IAC;
            } else {
                in[k++] = (char)c;
                if (c == '\r')
                    rx_state = S_CR;
            }
            break;
        case S_IAC:
            rx_state = S_DATA;
            if (c == IAC)
                in[k++] = (char)IAC;
            else if (c == IP || c == BRK)
                in[k++] = 0x03;     // 割り込み（Ctrl-C と同じ）
            else if (c >= WILL && c <= DONT) {
                rx_cmd = c;
                rx_state = S_OPT;
            } else if (c == SB) {
                sb_len = 0;
                rx_state = S_SB;
            }
            break;
        case S_OPT:
            negotiate(rx_cmd, c);
            rx_state = S_DATA;
            break;
        case S_SB:
            if (c == IAC)
                rx_state = S_SB_IAC;
            else if (sb_len < (int)sizeof(sb_buf))
                sb_buf[sb_len++] = c;
            break;
        case S_SB_IAC:
            if (c == SE) {
                subnegotiation();
                rx_state = S_DATA;
            } else {
                if (sb_len < (int)sizeof(sb_buf))
                    sb_buf[sb_len++] = c;   // IAC IAC
                rx_state = S_SB;
            }
            break;
        }
        if (k == (int)sizeof(in)) {
            deliver(in, k);
            k = 0;
        }
    }
    if (k)
        deliver(in, k);
}

// ---- つなぐ・切る ----

static void end_session(void)
{
    if (login_state == L_SHELL)
        monocs_console_close();
    monocs_tcp_close(session);
    session = -1;
    memset(login_pass, 0, sizeof login_pass);
}

// コンソールにつなぐ（USB からはすぐ、Wi-Fi からはログインの後）。だめなら接続を閉じて false
static bool open_shell(void)
{
    if (monocs_console_open() != 0) {
        say("Monocs: the console is in use\r\n");
        login_state = L_USER;   // コンソールは開いていない（end_session が閉じないように）
        end_session();
        return false;
    }
    login_state = L_SHELL;
    closing = false;
    if (naws_rows > 0 && naws_cols > 0)
        monocs_console_set_size(naws_rows, naws_cols);
    say("Monocs telnet\r\n");
    monocs_console_input("\r", 1);   // シェルにプロンプトを出させる
    return true;
}

static void begin_session(int h)
{
    bool wifi = monocs_tcp_netif(h) != MONOCS_NETIF_USB;
    session = h;
    rx_state = S_DATA;
    out_len = out_pos = 0;
    naws_rows = naws_cols = 0;
    login_state = L_USER;
    login_len = login_tries = 0;
    if (wifi) {
        char user[64], ha1[33];
        const char *no = !monocs_auth_load(user, ha1) ? "Monocs: no password is set for access over Wi-Fi (run passwd on Monocs)\r\n"
                         : timer_now_us() < locked_until ? "Monocs: too many failures; try again later\r\n" : nullptr;
        if (no) {
            say(no);
            end_session();
            return;
        }
    }
    // 1 文字ずつ送る形にし、画面の大きさを知らせてもらう
    send_cmd(WILL, OPT_ECHO);
    send_cmd(WILL, OPT_SGA);
    send_cmd(DO, OPT_SGA);
    send_cmd(DO, OPT_NAWS);
    if (!wifi) {
        open_shell();
        return;
    }
    login_until = timer_now_us() + LOGIN_IDLE_US;
    say("Monocs login: ");
}

// ---- サービス ----

int service_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    listener = monocs_tcp_listen(PORT);   // USB と Wi-Fi のどちらからも（Wi-Fi からのものはログインさせる）
    if (listener < 0) {
        printf("telnetd: cannot listen on port %d\n", PORT);
        return 1;
    }
    return 0;
}

void service_stop(void)
{
    if (session >= 0)
        end_session();
}

int service_poll(void)
{
    int work = 0;
    int h = monocs_tcp_accept(listener, 0);
    if (h >= 0) {
        if (session >= 0) {     // 同時につなげるのは 1 つまで
            static const char busy[] = "Monocs: another telnet session is active\r\n";
            monocs_tcp_send(h, busy, sizeof(busy) - 1, 1000);
            monocs_tcp_close(h);
        } else {
            begin_session(h);
        }
        work = 1;
    }
    if (session < 0)
        return work;

    // 受信
    unsigned char in[1024];
    int n = monocs_tcp_recv(session, in, sizeof(in), 0);
    if (n == 0 || n == -2) {    // 相手が閉じた
        end_session();
        return 1;
    }
    if (n > 0) {
        receive(in, n);
        work = 1;
        if (session < 0)
            return 1;
    }
    if (login_state != L_SHELL) {   // ログインの途中: コンソールにはまだつながっていない
        if (timer_now_us() >= login_until) {
            if (login_state != L_DELAY || login_tries >= LOGIN_TRIES || timer_now_us() < locked_until) {
                if (login_state == L_DELAY)
                    say("Login incorrect\r\n");
                end_session();      // 何も打たれない・まちがえすぎ
            } else {
                say("Login incorrect\r\n\r\nMonocs login: ");
                login_state = L_USER;
                login_len = 0;
                login_until = timer_now_us() + LOGIN_IDLE_US;
            }
            return 1;
        }
        return work;
    }

    // シェルで exit が打たれた: 残りの出力を送り切ってから閉じる
    if (monocs_console_hangup())
        closing = true;
    // 送信: コンソールの写しを読み、IAC を 2 重にして送る
    if (out_pos == out_len) {
        unsigned char raw[sizeof(out) / 2];
        int m = monocs_console_read(raw, sizeof(raw));
        if (m == 0 && closing) {
            end_session();
            return 1;
        }
        out_len = out_pos = 0;
        for (int i = 0; i < m; i++) {
            out[out_len++] = raw[i];
            if (raw[i] == IAC)
                out[out_len++] = IAC;
        }
    }
    if (out_pos < out_len) {
        int s = monocs_tcp_send(session, out + out_pos, out_len - out_pos, 0);
        if (s < 0) {
            end_session();
            return 1;
        }
        out_pos += s;
        work = 1;
    }
    return work;
}
