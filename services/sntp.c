// SNTP: NTP サーバーに日時を問い合わせて、Monocs の時計を合わせるサービス（RFC 4330）。
// Zero 2 W には RTC がないので、起動したら Pi 5（USB イーサネットの 192.168.7.1）の chrony に聞く
// （Pi 5 の準備は tools/pi5/setup-chrony.sh）。Pi 5 がいなければ、内蔵 Wi-Fi の先のサーバー（名前でよい）に聞く。
// /services.txt の 1 行で設定を変えられる:
//
//   /services/sntp.c [SERVER...] [-i 秒] [-z +0900]
//     SERVER  問い合わせる先（既定 192.168.7.1。4 つまで。答えがなければ次のサーバーに聞く。名前（pool.ntp.org
//             など）は DNS で引く。引けるまでは送れないので、次の番で送る）
//     -i 秒   合わせ直す間隔（既定 3600）
//     -z      タイムゾーンも設定する（UTC からの差。なければ変えない。nos.py や date -z でも設定できる）
//
// 合うまで（起動の直後は USB のネットワークがまだつながっていない）は 5 秒おきに問い合わせる。
// 時刻の差は、問い合わせと答えの 4 つの時刻から求める（RFC 4330 の 5 節）:
//   差 = ((T2 - T1) + (T3 - T4)) / 2   T1 送った時刻、T2 サーバーが受け取った時刻、T3 サーバーが送った時刻、
//                                      T4 受け取った時刻（T1・T4 は Monocs の時計、T2・T3 はサーバーの時計）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <monocs.h>

#define NTP_PORT        123
#define NTP_UNIX_DIFF   2208988800ULL   // 1900-01-01 から 1970-01-01 までの秒（NTP の時刻の始まり）
#define RETRY_SEC       5               // 合うまで、問い合わせ直す間隔
#define REPLY_MS        2000            // 答えを待つ時間

#define MAX_SERVERS     4
static const char *servers[MAX_SERVERS] = {"192.168.7.1"};
static int nservers = 1;
static long interval = 3600;
static int sock = -1;
static int64_t next_us;         // 次に問い合わせる時刻（起動からのマイクロ秒）
static int64_t sent_us;         // 問い合わせた時刻（起動から。0 なら答えを待っていない）
static uint8_t sent_ts[8];      // 問い合わせに入れた T1（答えの originate と同じはず）
static int64_t t1_us;           // T1（Monocs の時計。UNIX 時間のマイクロ秒）
static bool synced;

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t real_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// UNIX 時間のマイクロ秒 ⇔ NTP の時刻（秒 32 ビット・小数 32 ビット、ビッグエンディアン）
static void to_ntp(int64_t unix_us, uint8_t *p)
{
    uint64_t sec = (uint64_t)(unix_us / 1000000) + NTP_UNIX_DIFF;
    uint64_t frac = ((uint64_t)(unix_us % 1000000) << 32) / 1000000;
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(sec >> (24 - 8 * i));
        p[4 + i] = (uint8_t)(frac >> (24 - 8 * i));
    }
}

static int64_t from_ntp(const uint8_t *p)
{
    uint64_t sec = 0, frac = 0;
    for (int i = 0; i < 4; i++) {
        sec = sec << 8 | p[i];
        frac = frac << 8 | p[4 + i];
    }
    return (int64_t)(sec - NTP_UNIX_DIFF) * 1000000 + (int64_t)((frac * 1000000) >> 32);
}

static void send_request(void)
{
    uint8_t pkt[48] = {0};
    pkt[0] = 0x23;              // LI 0、版 4、モード 3（クライアント）
    t1_us = real_us();
    to_ntp(t1_us, pkt + 40);    // Transmit Timestamp（答えの Originate Timestamp に写される）
    memcpy(sent_ts, pkt + 40, 8);
    // すべてのサーバーに同時に聞き、最初に来た答えで合わせる（2026-11-17。前は 1 つずつ順に聞き、つながって
    // いない Pi 5（USB）の時間切れを待ってから次へ移ったので、起動から合うまで 30〜40 秒かかった）
    bool sent = false;
    for (int i = 0; i < nservers; i++)
        sent |= monocs_udp_sendto(sock, servers[i], NTP_PORT, pkt, sizeof(pkt)) == (int)sizeof(pkt);
    if (sent)
        sent_us = mono_us();
    else    // まだネットワークがない・名前がまだ引けていない
        next_us = mono_us() + (synced ? RETRY_SEC : 1) * 1000000LL;
}

// 答えを調べて、時計を合わせる。合わせたら true
static bool handle_reply(const uint8_t *p, int n)
{
    int64_t t4 = real_us();
    if (n < 48 || (p[0] & 7) != 4 || p[1] == 0 || memcmp(p + 24, sent_ts, 8) != 0)
        return false;   // サーバーの答えでない、kiss-o'-death（階層 0）、自分の問い合わせへの答えでない
    int64_t t2 = from_ntp(p + 32), t3 = from_ntp(p + 40);
    int64_t offset = ((t2 - t1_us) + (t3 - t4)) / 2;
    struct timespec ts;
    int64_t now = real_us() + offset;
    ts.tv_sec = (time_t)(now / 1000000);
    ts.tv_nsec = (long)(now % 1000000) * 1000;
    // 合わせたことは表示しない（シェルで打っている行に割り込むため）。date で見られる
    clock_settime(CLOCK_REALTIME, &ts);
    synced = true;
    return true;
}

int service_start(int argc, char **argv)
{
    int nargs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            interval = atol(argv[++i]);
            if (interval < 16)
                interval = 16;  // サーバーに迷惑をかけない
        } else if (!strcmp(argv[i], "-z") && i + 1 < argc) {
            const char *z = argv[++i];
            int sign = z[0] == '-' ? -1 : 1;
            if (z[0] == '+' || z[0] == '-')
                z++;
            int hhmm = atoi(z);
            monocs_set_timezone(sign * (hhmm / 100 * 60 + hhmm % 100));
        } else if (argv[i][0] != '-' && nargs < MAX_SERVERS) {
            servers[nargs++] = argv[i];
            nservers = nargs;
        } else {
            printf("sntp: usage: sntp.c [SERVER...] [-i SECONDS] [-z +0900]\n");
            return 1;
        }
    }
    sock = monocs_udp_open(0);
    if (sock < 0) {
        printf("sntp: cannot open a UDP socket\n");
        return 1;
    }
    next_us = mono_us();
    return 0;
}

void service_stop(void)
{
    if (sock >= 0)
        monocs_udp_close(sock);
    sock = -1;
}

int service_poll(void)
{
    int64_t now = mono_us();
    if (sent_us) {
        uint8_t buf[64];
        int n = monocs_udp_recvfrom(sock, buf, sizeof(buf), nullptr, nullptr, 0);
        if (n > 0 && handle_reply(buf, n)) {
            sent_us = 0;
            next_us = now + interval * 1000000LL;
            return 1;
        }
        if (now - sent_us > REPLY_MS * 1000LL) {
            sent_us = 0;    // 答えがない: 少し待って聞き直す（合うまでは早めに）
            next_us = now + (synced ? 60 : 2) * 1000000LL;
        }
        return n > 0;
    }
    if (now >= next_us) {
        send_request();
        return 1;
    }
    return 0;
}
