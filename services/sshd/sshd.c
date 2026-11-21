// sshd: SSH サーバー（ポート 22）。シェルのコンソールを、暗号化された接続で使えるようにする。サービスとして動く
// （svc start /services/sshd、起動時は /services.txt。os/kernel/svc.c）。telnet のサービスと同じく、
// コンソールの写し（monocs_console_*）でシェルにつなぐ。同時につなげるのは 3 つまで（対話のシェルは telnet と合わせて 1 つ。
// exec はコンソールの写しを使わないので、対話のシェルと同時に使える。exec どうしは 1 つずつ順に動く）。
//
//   ssh ユーザー名@アドレス        Windows・Linux・macOS に付いている ssh でそのままつながる
//   ssh ユーザー名@アドレス 'コマンド'   シェルの 1 行を動かして、出力と終了の値を返す（exec。os/kernel/rexec.c。
//                                  標準入力も渡る: ssh monocs 'cat > /work/f' < f。端末ではないので ls は 1 行に 1 つ）
//   sftp・scp・sshfs               ファイルの読み書き（subsystem "sftp"。sftp.c）
//
// 認証:
//   - USB から届いた接続（ケーブルの先の Pi 5）は、パスワードなしで入れる（telnet・HTTP と同じ扱い）
//   - 内蔵 Wi-Fi から届いた接続は、パスワード（/etc/htdigest。passwd コマンド。telnet・HTTP と同じもの）か、
//     公開鍵（/root/.ssh/authorized_keys。Linux の ~/.ssh/authorized_keys と同じ形。HOME は /root）
//     公開鍵は ECDSA（nistp256）と RSA。Ed25519 の鍵はまだ使えない（ssh-keygen -t ecdsa で作る）
//   - 続けて 5 回まちがえたら、30 秒は Wi-Fi からの接続を断る
//
// 対応する方式は 1 つずつ（どれも OpenSSH が既定で受けるもの）:
//   鍵交換 curve25519-sha256、ホスト鍵 ecdsa-sha2-nistp256、暗号 aes128-ctr、MAC hmac-sha2-256、圧縮なし
// ホスト鍵は最初に動かしたときに作って /etc/ssh/monocs_host_key に置く（P-256 の秘密の数 32 バイト）。
//
// できないこと: コマンドの実行（ssh host コマンド）、scp・sftp、ポートの転送、複数のチャンネル。
//
// 暗号の部品は BearSSL（/lib/bearssl。SHA-256・HMAC・AES-CTR・X25519・ECDSA・RSA の検証）。SSH の手順は自作:
//   RFC 4253（トランスポート: 版のやりとり、パケット、鍵交換、鍵の導出）、RFC 4252（認証）、RFC 4254（接続）、
//   RFC 5656（ECDSA の鍵と署名の形）、RFC 8731（curve25519-sha256）、RFC 8332（rsa-sha2-256・512）、
//   RFC 8308（EXT_INFO の server-sig-algs）、OpenSSH の PROTOCOL（strict KEX: kex-strict-s-v00@openssh.com）
// 専門家の検査を受けた実装ではない。家の LAN の中で telnet の代わりに使うためのもの。
//
// サービスのコア（コア 3）が service_poll を繰り返し呼ぶ。待たない形で少しずつ進める。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <monocs.h>
#include <monocs_auth.h>
#include <inc/bearssl.h>
#include "sftp.h"

#ifndef SSHD_PORT
#define SSHD_PORT       22
#endif
#ifndef HOST_KEY_DIR
#define HOST_KEY_DIR    "/etc/ssh"
#endif
#define HOST_KEY_FILE   HOST_KEY_DIR "/monocs_host_key"
#ifndef AUTHORIZED_KEYS
#define AUTHORIZED_KEYS "/root/.ssh/authorized_keys"
#endif
#define VERSION         "SSH-2.0-Monocs_1.0"

#define MAX_PACKET      35000       // 受け取るパケットの上限（RFC 4253 6.1 の 35000）
#define RX_SIZE         (MAX_PACKET + 64)
#define TX_SIZE         (128 * 1024)
#define CON_CHUNK       8192        // コンソールの出力を 1 つのパケットにする最大
#ifndef CON_HOLD_US
#define CON_HOLD_US     5000        // 流れ続けているとき、ためて待つ長さ
#endif
#define CON_STREAM_US   20000       // 前に送ってからこの内なら「流れ続けている」
#define CHAN_WINDOW     (1024 * 1024)   // 相手がこちらに送ってよい量（使った分を足し直す）
#define CHAN_PACKET     32768
#define LOGIN_GRACE_US  60000000ULL     // 認証が済むまでの時間
#define MAX_AUTH_TRIES  6
#define MAX_FAILS       5
#define LOCK_US         30000000ULL

// メッセージの番号（RFC 4253 12、RFC 4252 6、RFC 4254 9、RFC 5656 7.1）
enum {
    MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4, MSG_SERVICE_REQUEST = 5,
    MSG_SERVICE_ACCEPT = 6, MSG_EXT_INFO = 7, MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_KEX_ECDH_INIT = 30,
    MSG_KEX_ECDH_REPLY = 31, MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52,
    MSG_USERAUTH_PK_OK = 60, MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_FAILURE = 82, MSG_CHANNEL_OPEN = 90,
    MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92, MSG_CHANNEL_WINDOW_ADJUST = 93,
    MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95, MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97,
    MSG_CHANNEL_REQUEST = 98, MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100,
};

// ---- 組み立て・読み取り（SSH のデータの形。RFC 4251 5）----
typedef struct {
    uint8_t *p;
    int len, cap;
    bool over;
} Buf;

static void put(Buf *b, const void *data, int n)
{
    if (n < 0 || b->len + n > b->cap) {
        b->over = true;
        return;
    }
    memcpy(b->p + b->len, data, (size_t)n);
    b->len += n;
}

static void put_u8(Buf *b, int v)
{
    uint8_t c = (uint8_t)v;
    put(b, &c, 1);
}

static void put_u32(Buf *b, uint32_t v)
{
    uint8_t c[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(b, c, 4);
}

static void put_str(Buf *b, const void *data, int n)
{
    put_u32(b, (uint32_t)n);
    put(b, data, n);
}

static void put_cstr(Buf *b, const char *s)
{
    put_str(b, s, (int)strlen(s));
}

// 符号なしの大きな数（上位から n バイト）を mpint に（前の 0 を取り、先頭のビットが立っていれば 0 を足す）
static void put_mpint(Buf *b, const uint8_t *v, int n)
{
    while (n > 0 && v[0] == 0) {
        v++;
        n--;
    }
    bool pad = n > 0 && (v[0] & 0x80);
    put_u32(b, (uint32_t)(n + pad));
    if (pad)
        put_u8(b, 0);
    put(b, v, n);
}

typedef struct {
    const uint8_t *p;
    int len, pos;
    bool err;
} Rd;

static int get_u8(Rd *r)
{
    if (r->pos + 1 > r->len) {
        r->err = true;
        return 0;
    }
    return r->p[r->pos++];
}

static uint32_t get_u32(Rd *r)
{
    if (r->pos + 4 > r->len) {
        r->err = true;
        return 0;
    }
    const uint8_t *q = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
}

static const uint8_t *get_str(Rd *r, int *n)
{
    uint32_t len = get_u32(r);
    if (r->err || len > (uint32_t)(r->len - r->pos)) {
        r->err = true;
        *n = 0;
        return (const uint8_t *)"";
    }
    const uint8_t *q = r->p + r->pos;
    r->pos += (int)len;
    *n = (int)len;
    return q;
}

static bool str_is(const uint8_t *s, int n, const char *want)
{
    return (int)strlen(want) == n && !memcmp(s, want, (size_t)n);
}

// コンマ区切りの一覧に name があるか
static bool list_has(const uint8_t *list, int n, const char *name)
{
    int nl = (int)strlen(name);
    for (int i = 0; i <= n - nl;) {
        int j = i;
        while (j < n && list[j] != ',')
            j++;
        if (j - i == nl && !memcmp(list + i, name, (size_t)nl))
            return true;
        i = j + 1;
    }
    return false;
}

// ---- 暗号の部品 ----
static br_hmac_drbg_context rng;

static void rng_seed(void)
{
    uint8_t seed[48];
    getentropy(seed, sizeof seed);
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
}

static void rng_bytes(void *p, int n)
{
    br_hmac_drbg_generate(&rng, p, (size_t)n);
}

// 片方向の暗号と MAC（aes128-ctr、hmac-sha2-256）
typedef struct {
    bool on;
    br_aes_ct64_ctr_keys aes;
    uint8_t ctr[16];            // 128 ビットのカウンタ（上位から。ブロックごとに 1 増える。RFC 4344 4）
    br_hmac_key_context mac;
    uint32_t seq;               // パケットの通し番号（暗号を始める前から数える）
} Cipher;

// data（16 の倍数）を暗号化・復号する。BearSSL の CTR は「12 バイトの IV + 32 ビットのカウンタ」なので、
// 32 ビットがあふれるところで分けて、上の 96 ビットに繰り上げる
static void ctr_crypt(Cipher *c, uint8_t *data, int len)
{
    while (len > 0) {
        uint32_t cc = (uint32_t)c->ctr[12] << 24 | c->ctr[13] << 16 | c->ctr[14] << 8 | c->ctr[15];
        uint64_t room = (0x100000000ULL - cc) * 16;     // あふれるまでに使えるバイト数
        int n = (uint64_t)len < room ? len : (int)room;
        uint32_t next = br_aes_ct64_ctr_run(&c->aes, c->ctr, cc, data, (size_t)n);
        c->ctr[12] = (uint8_t)(next >> 24);
        c->ctr[13] = (uint8_t)(next >> 16);
        c->ctr[14] = (uint8_t)(next >> 8);
        c->ctr[15] = (uint8_t)next;
        if (next == 0)      // あふれた: 上の 96 ビットに 1 を足す
            for (int i = 11; i >= 0 && ++c->ctr[i] == 0; i--)
                ;
        data += n;
        len -= n;
    }
}

static void mac_compute(Cipher *c, const uint8_t *packet, int len, uint8_t out[32])
{
    br_hmac_context h;
    uint8_t seq[4] = {(uint8_t)(c->seq >> 24), (uint8_t)(c->seq >> 16), (uint8_t)(c->seq >> 8), (uint8_t)c->seq};
    br_hmac_init(&h, &c->mac, 0);
    br_hmac_update(&h, seq, 4);
    br_hmac_update(&h, packet, (size_t)len);
    br_hmac_out(&h, out);
}

static bool equal_ct(const uint8_t *a, const uint8_t *b, int n)
{
    unsigned diff = 0;
    for (int i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

// ---- ホスト鍵（ECDSA、P-256）----
static br_ec_private_key host_sk;
static uint8_t host_sk_buf[BR_EC_KBUF_PRIV_MAX_SIZE];
static uint8_t host_pub[65];        // 04 || X || Y
static uint8_t host_blob[128];      // 公開鍵の形（string "ecdsa-sha2-nistp256", string "nistp256", string Q）
static int host_blob_len;

static bool load_host_key(void)
{
    uint8_t raw[32];
    bool have = false;
    FILE *f = fopen(HOST_KEY_FILE, "rb");
    if (f) {
        have = fread(raw, 1, 32, f) == 32;
        fclose(f);
    }
    if (have) {
        memcpy(host_sk_buf, raw, 32);
        host_sk.curve = BR_EC_secp256r1;
        host_sk.x = host_sk_buf;
        host_sk.xlen = 32;
    } else {
        const br_prng_class **prng = &rng.vtable;
        if (br_ec_keygen(prng, &br_ec_p256_m31, &host_sk, host_sk_buf, BR_EC_secp256r1) != 32)
            return false;
        mkdir("/etc", 0755);
        mkdir(HOST_KEY_DIR, 0755);
        f = fopen(HOST_KEY_FILE ".tmp", "wb");
        bool ok = f && fwrite(host_sk.x, 1, 32, f) == 32;
        ok = f && fclose(f) == 0 && ok;
        if (!ok || rename(HOST_KEY_FILE ".tmp", HOST_KEY_FILE) != 0) {
            printf("sshd: cannot write %s\n", HOST_KEY_FILE);
            return false;
        }
    }
    memset(raw, 0, sizeof raw);
    br_ec_public_key pk;
    if (br_ec_compute_pub(&br_ec_p256_m31, &pk, host_pub, &host_sk) != 65)
        return false;
    Buf b = {host_blob, 0, sizeof host_blob, false};
    put_cstr(&b, "ecdsa-sha2-nistp256");
    put_cstr(&b, "nistp256");
    put_str(&b, host_pub, 65);
    host_blob_len = b.len;
    return !b.over;
}

// ---- 接続の状態 ----
enum { P_VERSION, P_KEXINIT, P_KEX_ECDH, P_NEWKEYS, P_SERVICE, P_AUTH, P_OPEN };

static int listener = -1;
// 接続ごとの状態（2026-11-16 から同時に SSHD_CONNS 個まで。対話のシェルを開いたまま、ほかの ssh（nos.py など）も
// つなげるように）。service_poll が接続ごとに C を切り替えて、今までと同じ関数で扱う
#define SSHD_CONNS  3
typedef struct {
    int conn;                       // TCP の接続（-1 = 空き）
    int phase;
    bool from_usb, authed, rekeying, strict_kex, ext_info, ignore_next_kex, kex_done, junk_before_kexinit;
    uint64_t started_us;
    uint8_t rx[RX_SIZE];
    int rx_len;
    bool rx_head_done;              // 今のパケットの最初のブロックを復号した
    uint8_t tx[TX_SIZE];
    int tx_len, tx_pos;
    Cipher c2s, s2c;                // 受け取る向き・送る向き
    Cipher next_c2s, next_s2c;      // 鍵交換で決めた、NEWKEYS から使う鍵
    char client_version[256];
    uint8_t client_kexinit[4096], server_kexinit[512];
    int client_kexinit_len, server_kexinit_len;
    uint8_t session_id[32];
    bool have_session_id;
    int auth_tries;
    // チャンネル（接続ごとに 1 つだけ）
    bool chan_open, chan_shell, chan_eof_sent, hangup_pending;
    // コンソールの出力をためる入れ物（頭の 9 バイトは CHANNEL_DATA の頭）。流れ続けているときは少しためて
    // 大きなパケットで送る（pump_console）
    uint8_t con_msg[9 + CON_CHUNK];
    int con_len;
    uint64_t con_first_us, con_sent_us;
    uint32_t peer_chan, peer_window, peer_max_packet, my_window;
    int pty_rows, pty_cols;
    // exec（ssh host コマンド。シェルの 1 行を monocs_rexec_* で動かしてもらう。同時に動かせるのは 1 つなので、ほかの
    // 接続の exec が動いていれば、終わるまで待ってから始める）。相手から届いた標準入力は、行が受け取るまで exec_in
    // （exec の間だけ確保する。CHAN_WINDOW バイト）にためる（受け取った分だけ窓を足し直すので、窓の大きさを超えない）
    bool chan_exec, exec_started, exec_in_eof, exec_eof_sent, exec_told;
    bool stop_app, app_stopped;
    Sftp *sftp;                     // subsystem "sftp"（sftp.c）
    bool sftp_eof;     // 環境変数 MONOCS_STOP_APP=1 を受けた: 待たせているコンソールのアプリを止める
    uint64_t exec_since;            // exec を頼まれた時刻（待っていることを知らせる）
    char *exec_cmd;
    uint8_t *exec_in;
    uint32_t exec_in_len;
} Conn;

static Conn conns[SSHD_CONNS];
static Conn *C = &conns[0];         // 今扱っている接続
static int fails;                   // 続けてまちがえた回数（接続をまたいで数える）
static uint64_t locked_until;

static void sftp_end(void)
{
    sftp_free(C->sftp);
    C->sftp = nullptr;
}

static void exec_end(void)
{
    if (C->chan_exec && C->exec_started)
        monocs_rexec_release();
    free(C->exec_cmd);
    free(C->exec_in);
    C->exec_cmd = nullptr;
    C->exec_in = nullptr;
    C->chan_exec = C->exec_started = false;
}

static void end_conn(void)
{
    if (C->chan_shell)
        monocs_console_close();
    exec_end();
    sftp_end();
    if (C->conn >= 0)
        monocs_tcp_close(C->conn);
    C->conn = -1;
    C->chan_open = C->chan_shell = false;
    memset(&C->c2s, 0, sizeof C->c2s);
    memset(&C->s2c, 0, sizeof C->s2c);
    memset(&C->next_c2s, 0, sizeof C->next_c2s);
    memset(&C->next_s2c, 0, sizeof C->next_s2c);
}

// ---- 送る ----
// payload をパケットにして（詰め物、MAC、暗号化）、送る待ちに足す。RFC 4253 6
static void send_packet(const uint8_t *payload, int len)
{
    int block = C->s2c.on ? 16 : 8;
    int pad = block - (5 + len) % block;
    if (pad < 4)
        pad += block;
    int total = 5 + len + pad, maclen = C->s2c.on ? 32 : 0;
    if (C->conn < 0 || C->tx_len + total + maclen > TX_SIZE) {
        end_conn();     // 送る待ちがあふれた（相手が受け取らない）
        return;
    }
    uint8_t *p = C->tx + C->tx_len;
    uint32_t plen = (uint32_t)(1 + len + pad);
    p[0] = (uint8_t)(plen >> 24);
    p[1] = (uint8_t)(plen >> 16);
    p[2] = (uint8_t)(plen >> 8);
    p[3] = (uint8_t)plen;
    p[4] = (uint8_t)pad;
    memcpy(p + 5, payload, (size_t)len);
    rng_bytes(p + 5 + len, pad);
    if (C->s2c.on) {
        mac_compute(&C->s2c, p, total, p + total);     // MAC は暗号化する前のものに付ける
        ctr_crypt(&C->s2c, p, total);
    }
    C->s2c.seq++;
    C->tx_len += total + maclen;
}

static void send_buf(Buf *b)
{
    if (b->over)
        end_conn();
    else
        send_packet(b->p, b->len);
}

static void disconnect(int reason, const char *msg)
{
    uint8_t tmp[300];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, MSG_DISCONNECT);
    put_u32(&b, (uint32_t)reason);
    put_cstr(&b, msg);
    put_cstr(&b, "");
    if (C->conn >= 0 && !b.over) {
        send_packet(b.p, b.len);
        if (C->conn >= 0)
            monocs_tcp_send(C->conn, C->tx + C->tx_pos, C->tx_len - C->tx_pos, 500);
    }
    end_conn();
}

// ---- 鍵交換（RFC 4253 7、RFC 8731）----
static void send_kexinit(void)
{
    Buf b = {C->server_kexinit, 0, sizeof C->server_kexinit, false};
    uint8_t cookie[16];
    rng_bytes(cookie, 16);
    put_u8(&b, MSG_KEXINIT);
    put(&b, cookie, 16);
    put_cstr(&b, C->rekeying ? "curve25519-sha256,curve25519-sha256@libssh.org"
                          : "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com");
    put_cstr(&b, "ecdsa-sha2-nistp256");
    put_cstr(&b, "aes128-ctr");         // 暗号（相手 → こちら）
    put_cstr(&b, "aes128-ctr");         // 暗号（こちら → 相手）
    put_cstr(&b, "hmac-sha2-256");      // MAC
    put_cstr(&b, "hmac-sha2-256");
    put_cstr(&b, "none");               // 圧縮
    put_cstr(&b, "none");
    put_cstr(&b, "");                   // 言語
    put_cstr(&b, "");
    put_u8(&b, 0);                      // first_kex_packet_follows
    put_u32(&b, 0);
    C->server_kexinit_len = b.len;
    send_buf(&b);
}

static void handle_kexinit(const uint8_t *payload, int len)
{
    if (len > (int)sizeof C->client_kexinit) {
        disconnect(2, "KEXINIT too long");
        return;
    }
    memcpy(C->client_kexinit, payload, (size_t)len);
    C->client_kexinit_len = len;
    Rd r = {payload, len, 17, false};   // 番号（1）と cookie（16）の後
    int n[10];
    const uint8_t *l[10];
    for (int i = 0; i < 10; i++)
        l[i] = get_str(&r, &n[i]);
    bool follows = get_u8(&r) != 0;
    if (r.err) {
        disconnect(2, "bad KEXINIT");
        return;
    }
    bool kex_ok = list_has(l[0], n[0], "curve25519-sha256") || list_has(l[0], n[0], "curve25519-sha256@libssh.org");
    if (!kex_ok || !list_has(l[1], n[1], "ecdsa-sha2-nistp256") || !list_has(l[2], n[2], "aes128-ctr") ||
        !list_has(l[3], n[3], "aes128-ctr") || !list_has(l[4], n[4], "hmac-sha2-256") ||
        !list_has(l[5], n[5], "hmac-sha2-256") || !list_has(l[6], n[6], "none") || !list_has(l[7], n[7], "none")) {
        disconnect(3, "no common algorithm (curve25519-sha256, ecdsa-sha2-nistp256, aes128-ctr, hmac-sha2-256)");
        return;
    }
    if (!C->rekeying) {
        C->strict_kex = list_has(l[0], n[0], "kex-strict-c-v00@openssh.com");
        C->ext_info = list_has(l[0], n[0], "ext-info-c");
        if (C->strict_kex && C->junk_before_kexinit) {
            disconnect(3, "strict KEX violation");
            return;
        }
    }
    // 相手が当て推量で次のパケットを送っていて、それが外れなら 1 つ捨てる（RFC 4253 7）。当たりは、
    // 相手の一覧の先頭がこちらの選ぶもの（鍵交換と、ホスト鍵）のとき
    bool first_kex = (n[0] >= 17 && !memcmp(l[0], "curve25519-sha256", 17) && (n[0] == 17 || l[0][17] == ',' || l[0][17] == '@'));
    bool first_key = n[1] >= 19 && !memcmp(l[1], "ecdsa-sha2-nistp256", 19) && (n[1] == 19 || l[1][19] == ',');
    C->ignore_next_kex = follows && !(first_kex && first_key);
    if (C->rekeying && C->phase == P_OPEN)
        send_kexinit();     // 相手から始めた鍵の交換し直し: こちらの KEXINIT を返す
    C->phase = P_KEX_ECDH;
}

static void derive(br_sha256_context *base, char letter, uint8_t out[32])
{
    br_sha256_context h = *base;    // K || H まで入れてある
    br_sha256_update(&h, &letter, 1);
    br_sha256_update(&h, C->session_id, 32);
    br_sha256_out(&h, out);
}

static void setup_cipher(Cipher *c, const uint8_t iv[32], const uint8_t key[32], const uint8_t mac[32])
{
    c->on = true;
    br_aes_ct64_ctr_init(&c->aes, key, 16);
    memcpy(c->ctr, iv, 16);
    br_hmac_key_init(&c->mac, &br_sha256_vtable, mac, 32);
}

static void handle_ecdh_init(const uint8_t *payload, int len)
{
    Rd r = {payload, len, 1, false};
    int qc_len;
    const uint8_t *qc = get_str(&r, &qc_len);
    if (r.err || qc_len != 32) {
        disconnect(3, "bad KEX_ECDH_INIT");
        return;
    }
    // こちらの一時の鍵と、共有の秘密（X25519。BearSSL は数を上位から受け取り、決まったビットは自分で直す）
    uint8_t priv[32], qs[32], shared[32], point[32];
    rng_bytes(priv, 32);
    if (br_ec_c25519_m31.mulgen(qs, priv, 32, BR_EC_curve25519) != 32) {
        disconnect(3, "key exchange failed");
        return;
    }
    memcpy(point, qc, 32);
    uint32_t ok = br_ec_c25519_m31.mul(point, 32, priv, 32, BR_EC_curve25519);
    memcpy(shared, point, 32);
    memset(priv, 0, sizeof priv);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++)
        zero |= shared[i];
    if (!ok || zero == 0) {     // 全部 0 は受け付けない（RFC 8731 3）
        disconnect(3, "key exchange failed");
        return;
    }
    // 交換のハッシュ H = SHA-256(V_C, V_S, I_C, I_S, K_S, Q_C, Q_S, K)。K は 32 バイトを上位からの数として mpint に
    static uint8_t hb[8192];
    Buf b = {hb, 0, sizeof hb, false};
    put_cstr(&b, C->client_version);
    put_cstr(&b, VERSION);
    put_str(&b, C->client_kexinit, C->client_kexinit_len);
    put_str(&b, C->server_kexinit, C->server_kexinit_len);
    put_str(&b, host_blob, host_blob_len);
    put_str(&b, qc, 32);
    put_str(&b, qs, 32);
    int k_off = b.len;
    put_mpint(&b, shared, 32);
    if (b.over) {
        disconnect(3, "key exchange failed");
        return;
    }
    uint8_t H[32];
    br_sha256_context sha;
    br_sha256_init(&sha);
    br_sha256_update(&sha, b.p, (size_t)b.len);
    br_sha256_out(&sha, H);
    if (!C->have_session_id) {
        memcpy(C->session_id, H, 32);      // 最初の H が、この接続の識別（鍵を交換し直しても変わらない）
        C->have_session_id = true;
    }
    // ホスト鍵で H に署名する（ECDSA は SHA-256(H) に。RFC 5656 3.1.2: 署名は mpint r、mpint s）
    uint8_t hh[32], sig[64];
    br_sha256_init(&sha);
    br_sha256_update(&sha, H, 32);
    br_sha256_out(&sha, hh);
    if (br_ecdsa_i31_sign_raw(&br_ec_p256_m31, &br_sha256_vtable, hh, &host_sk, sig) != 64) {
        disconnect(3, "signing failed");
        return;
    }
    uint8_t rs[80], sigblob[128], reply[400];
    Buf rsb = {rs, 0, sizeof rs, false};
    put_mpint(&rsb, sig, 32);
    put_mpint(&rsb, sig + 32, 32);
    Buf sb = {sigblob, 0, sizeof sigblob, false};
    put_cstr(&sb, "ecdsa-sha2-nistp256");
    put_str(&sb, rs, rsb.len);
    Buf rb = {reply, 0, sizeof reply, false};
    put_u8(&rb, MSG_KEX_ECDH_REPLY);
    put_str(&rb, host_blob, host_blob_len);
    put_str(&rb, qs, 32);
    put_str(&rb, sigblob, sb.len);
    send_buf(&rb);
    // 鍵の導出（RFC 4253 7.2）: HASH(K || H || 文字 || session_id)。A・B = IV、C・D = 暗号の鍵、E・F = MAC の鍵
    br_sha256_init(&sha);
    br_sha256_update(&sha, b.p + k_off, (size_t)(b.len - k_off));
    br_sha256_update(&sha, H, 32);
    uint8_t iv[32], key[32], mac[32];
    derive(&sha, 'A', iv);
    derive(&sha, 'C', key);
    derive(&sha, 'E', mac);
    setup_cipher(&C->next_c2s, iv, key, mac);
    derive(&sha, 'B', iv);
    derive(&sha, 'D', key);
    derive(&sha, 'F', mac);
    setup_cipher(&C->next_s2c, iv, key, mac);
    memset(iv, 0, sizeof iv);
    memset(key, 0, sizeof key);
    memset(mac, 0, sizeof mac);
    memset(shared, 0, sizeof shared);
    memset(hb, 0, sizeof hb);
    // こちらの NEWKEYS。この後に送るものから新しい鍵
    uint8_t nk = MSG_NEWKEYS;
    send_packet(&nk, 1);
    uint32_t seq = C->strict_kex ? 0 : C->s2c.seq;    // strict KEX では通し番号を 0 に戻す
    C->s2c = C->next_s2c;
    C->s2c.seq = seq;
    memset(&C->next_s2c, 0, sizeof C->next_s2c);
    C->phase = P_NEWKEYS;
}

static void handle_newkeys(void)
{
    uint32_t seq = C->strict_kex ? 0 : C->c2s.seq;
    C->c2s = C->next_c2s;
    C->c2s.seq = seq;
    memset(&C->next_c2s, 0, sizeof C->next_c2s);
    if (C->rekeying) {
        C->rekeying = false;
        C->phase = P_OPEN;
        return;
    }
    C->kex_done = true;
    C->phase = P_SERVICE;
    if (C->ext_info) {     // RSA の鍵で rsa-sha2-256・512 を使ってもらうために知らせる（RFC 8308）
        uint8_t tmp[128];
        Buf b = {tmp, 0, sizeof tmp, false};
        put_u8(&b, MSG_EXT_INFO);
        put_u32(&b, 1);
        put_cstr(&b, "server-sig-algs");
        put_cstr(&b, "ecdsa-sha2-nistp256,rsa-sha2-256,rsa-sha2-512");
        send_buf(&b);
    }
}

// ---- 認証（RFC 4252）----
static void auth_failure(bool count)
{
    if (count) {
        C->auth_tries++;
        if (!C->from_usb && ++fails >= MAX_FAILS) {
            fails = 0;
            locked_until = timer_now_us() + LOCK_US;
        }
    }
    if (C->auth_tries >= MAX_AUTH_TRIES) {
        disconnect(2, "Too many authentication failures");
        return;
    }
    uint8_t tmp[64];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, MSG_USERAUTH_FAILURE);
    put_cstr(&b, "publickey,password");
    put_u8(&b, 0);
    send_buf(&b);
}

static void auth_success(void)
{
    uint8_t m = MSG_USERAUTH_SUCCESS;
    send_packet(&m, 1);
    C->authed = true;
    fails = 0;
    C->phase = P_OPEN;
}

// base64 の 1 行を戻す。バイト数（だめなら -1）
static int base64_decode(const char *s, int n, uint8_t *out, int cap)
{
    int bits = 0, nb = 0, len = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (c == '=')
            break;
        if (v < 0)
            return -1;
        bits = (bits << 6 | v) & 0xFFFF;    // 使うのは下の 12 ビットまで
        nb += 6;
        if (nb >= 8) {
            nb -= 8;
            if (len >= cap)
                return -1;
            out[len++] = (uint8_t)(bits >> nb);
        }
    }
    return len;
}

// 公開鍵（blob）が /root/.ssh/authorized_keys にあるか（行は「種類 base64 注釈」。前のオプションには対応しない）
static bool key_authorized(const uint8_t *blob, int blob_len)
{
    FILE *f = fopen(AUTHORIZED_KEYS, "r");
    if (!f)
        return false;
    static char line[4096];
    static uint8_t dec[2048];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == 0)
            continue;
        char *sp = strchr(p, ' ');
        if (!sp)
            continue;
        char *b64 = sp + 1;
        int n = (int)strcspn(b64, " \t\r\n");
        int len = base64_decode(b64, n, dec, sizeof dec);
        found = len == blob_len && equal_ct(dec, blob, len);
    }
    fclose(f);
    return found;
}

// 署名を確かめる。data は署名されたもの。alg は署名の方式、blob は公開鍵、sig は署名（string 方式, string 中身）
static bool verify_signature(const uint8_t *alg, int alg_len, const uint8_t *blob, int blob_len, const uint8_t *sig,
                             int sig_len, const uint8_t *data, int data_len)
{
    Rd kb = {blob, blob_len, 0, false}, sb = {sig, sig_len, 0, false};
    int n, sn, sbn;
    const uint8_t *ktype = get_str(&kb, &n);
    const uint8_t *salg = get_str(&sb, &sn);
    const uint8_t *sbody = get_str(&sb, &sbn);
    if (kb.err || sb.err || sn != alg_len || memcmp(salg, alg, (size_t)sn))
        return false;
    if (str_is(alg, alg_len, "ecdsa-sha2-nistp256")) {
        int cn, qn;
        const uint8_t *curve = get_str(&kb, &cn);
        const uint8_t *q = get_str(&kb, &qn);
        if (kb.err || !str_is(ktype, n, "ecdsa-sha2-nistp256") || !str_is(curve, cn, "nistp256") || qn != 65)
            return false;
        // 署名の中身は mpint r、mpint s。32 バイトずつに直す
        Rd rs = {sbody, sbn, 0, false};
        uint8_t raw[64] = {0};
        for (int k = 0; k < 2; k++) {
            int mn;
            const uint8_t *m = get_str(&rs, &mn);
            while (mn > 0 && m[0] == 0) {
                m++;
                mn--;
            }
            if (rs.err || mn > 32)
                return false;
            memcpy(raw + 32 * k + (32 - mn), m, (size_t)mn);
        }
        uint8_t hash[32];
        br_sha256_context sha;
        br_sha256_init(&sha);
        br_sha256_update(&sha, data, (size_t)data_len);
        br_sha256_out(&sha, hash);
        br_ec_public_key pk = {BR_EC_secp256r1, (unsigned char *)q, 65};
        return br_ecdsa_i31_vrfy_raw(&br_ec_p256_m31, hash, 32, &pk, raw, 64) == 1;
    }
    bool s256 = str_is(alg, alg_len, "rsa-sha2-256"), s512 = str_is(alg, alg_len, "rsa-sha2-512");
    if (s256 || s512) {
        int en, nn;
        const uint8_t *e = get_str(&kb, &en);
        const uint8_t *nmod = get_str(&kb, &nn);
        if (kb.err || !str_is(ktype, n, "ssh-rsa"))
            return false;
        while (nn > 0 && nmod[0] == 0) {    // mpint の前の 0 を取る
            nmod++;
            nn--;
        }
        if (nn < 256 || nn > 1024 || sbn > nn)  // 2048 ビット未満の鍵は受け付けない
            return false;
        // 署名は法の長さより短いことがある（前の 0 が落ちる）ので、長さをそろえる
        static uint8_t sigbuf[1024];
        memset(sigbuf, 0, (size_t)(nn - sbn));
        memcpy(sigbuf + (nn - sbn), sbody, (size_t)sbn);
        uint8_t hash[64], got[64];
        int hlen = s256 ? 32 : 64;
        if (s256) {
            br_sha256_context sha;
            br_sha256_init(&sha);
            br_sha256_update(&sha, data, (size_t)data_len);
            br_sha256_out(&sha, hash);
        } else {
            br_sha512_context sha;
            br_sha512_init(&sha);
            br_sha512_update(&sha, data, (size_t)data_len);
            br_sha512_out(&sha, hash);
        }
        br_rsa_public_key pk = {(unsigned char *)nmod, (size_t)nn, (unsigned char *)e, (size_t)en};
        if (!br_rsa_i31_pkcs1_vrfy(sigbuf, (size_t)nn, s256 ? BR_HASH_OID_SHA256 : BR_HASH_OID_SHA512, (size_t)hlen, &pk, got))
            return false;
        return equal_ct(hash, got, hlen);
    }
    return false;
}

static void handle_userauth(const uint8_t *payload, int len)
{
    Rd r = {payload, len, 1, false};
    int un, sn, mn;
    const uint8_t *user = get_str(&r, &un);
    const uint8_t *service = get_str(&r, &sn);
    const uint8_t *method = get_str(&r, &mn);
    if (r.err || !str_is(service, sn, "ssh-connection") || un >= 64) {
        disconnect(2, "bad USERAUTH_REQUEST");
        return;
    }
    if (C->from_usb) {     // USB から: どの方法でも通す（パスワードなしで使える）
        auth_success();
        return;
    }
    if (timer_now_us() < locked_until) {
        disconnect(2, "Too many authentication failures; try again later");
        return;
    }
    if (str_is(method, mn, "password")) {
        int pn;
        get_u8(&r);     // パスワードの変更ではない
        const uint8_t *pass = get_str(&r, &pn);
        char u[64], p[128], want_user[64], ha1[33], typed[33];
        if (r.err || pn >= (int)sizeof p) {
            auth_failure(true);
            return;
        }
        memcpy(u, user, (size_t)un);
        u[un] = 0;
        memcpy(p, pass, (size_t)pn);
        p[pn] = 0;
        bool ok = monocs_auth_load(want_user, ha1);
        monocs_auth_ha1(u, p, typed);
        memset(p, 0, sizeof p);
        ok = ok && (monocs_auth_equal(u, want_user) & monocs_auth_equal(typed, ha1));
        if (ok)
            auth_success();
        else
            auth_failure(true);
        return;
    }
    if (str_is(method, mn, "publickey")) {
        int an, bn, gn = 0;
        bool has_sig = get_u8(&r) != 0;
        const uint8_t *alg = get_str(&r, &an);
        const uint8_t *blob = get_str(&r, &bn);
        int signed_len = r.pos;     // ここまでが署名される（前に session_id を付ける）
        const uint8_t *sig = has_sig ? get_str(&r, &gn) : nullptr;
        bool alg_ok = str_is(alg, an, "ecdsa-sha2-nistp256") || str_is(alg, an, "rsa-sha2-256") ||
                      str_is(alg, an, "rsa-sha2-512");
        if (r.err || !alg_ok || !key_authorized(blob, bn)) {
            auth_failure(has_sig);  // 鍵を試すだけ（署名なし）の失敗は数えない
            return;
        }
        if (!has_sig) {     // この鍵なら受ける、と答える（RFC 4252 7）
            static uint8_t tmp[2200];
            Buf b = {tmp, 0, sizeof tmp, false};
            put_u8(&b, MSG_USERAUTH_PK_OK);
            put_str(&b, alg, an);
            put_str(&b, blob, bn);
            send_buf(&b);
            return;
        }
        static uint8_t data[4096];
        Buf d = {data, 0, sizeof data, false};
        put_str(&d, C->session_id, 32);
        put(&d, payload, signed_len);
        if (!d.over && verify_signature(alg, an, blob, bn, sig, gn, data, d.len))
            auth_success();
        else
            auth_failure(true);
        return;
    }
    auth_failure(false);    // none など: 使える方法を知らせる
}

// ---- 接続（RFC 4254）: チャンネル 1 つ、シェルだけ ----
static void chan_reply(bool ok)
{
    uint8_t tmp[8];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE);
    put_u32(&b, C->peer_chan);
    send_buf(&b);
}

static void chan_text(const char *s)
{
    uint8_t tmp[300];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, MSG_CHANNEL_DATA);
    put_u32(&b, C->peer_chan);
    put_cstr(&b, s);
    send_buf(&b);
}

static void chan_close(void)
{
    if (!C->chan_open || C->chan_eof_sent)
        return;
    uint8_t tmp[8];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, MSG_CHANNEL_CLOSE);
    put_u32(&b, C->peer_chan);
    send_buf(&b);
    C->chan_eof_sent = true;
}

static void handle_channel_open(const uint8_t *payload, int len)
{
    Rd r = {payload, len, 1, false};
    int tn;
    const uint8_t *type = get_str(&r, &tn);
    uint32_t sender = get_u32(&r), window = get_u32(&r), maxp = get_u32(&r);
    uint8_t tmp[128];
    Buf b = {tmp, 0, sizeof tmp, false};
    if (r.err || C->chan_open || !str_is(type, tn, "session")) {
        put_u8(&b, MSG_CHANNEL_OPEN_FAILURE);
        put_u32(&b, sender);
        put_u32(&b, C->chan_open ? 4 : 3);     // 4 = 資源がない、3 = 知らない種類
        put_cstr(&b, C->chan_open ? "only one session" : "unknown channel type");
        put_cstr(&b, "");
        send_buf(&b);
        return;
    }
    C->chan_open = true;
    C->chan_shell = C->chan_eof_sent = false;
    C->peer_chan = sender;
    C->peer_window = window;
    C->peer_max_packet = maxp;
    C->my_window = CHAN_WINDOW;
    C->pty_rows = C->pty_cols = 0;
    put_u8(&b, MSG_CHANNEL_OPEN_CONFIRMATION);
    put_u32(&b, sender);
    put_u32(&b, 0);             // こちらのチャンネルの番号
    put_u32(&b, CHAN_WINDOW);
    put_u32(&b, CHAN_PACKET);
    send_buf(&b);
}

static void handle_channel_request(const uint8_t *payload, int len)
{
    Rd r = {payload, len, 1, false};
    int tn;
    get_u32(&r);
    const uint8_t *type = get_str(&r, &tn);
    bool want_reply = get_u8(&r) != 0;
    if (r.err || !C->chan_open)
        return;
    bool ok = false;
    if (str_is(type, tn, "pty-req") || str_is(type, tn, "window-change")) {
        if (type[0] == 'p') {
            int n;
            get_str(&r, &n);    // TERM（使わない。Monocs の curses は xterm の形で送る）
        }
        uint32_t cols = get_u32(&r), rows = get_u32(&r);
        if (!r.err && cols < 1000 && rows < 1000) {
            ok = true;      // 大きさ 0（相手に端末がない）のときは、大きさを知らないまま
            if (cols > 0 && rows > 0) {
                C->pty_cols = (int)cols;
                C->pty_rows = (int)rows;
                if (C->chan_shell)
                    monocs_console_set_size(C->pty_rows, C->pty_cols);
            }
        }
    } else if (str_is(type, tn, "shell")) {
        if (C->chan_shell) {
            ok = false;
        } else if (monocs_console_open() != 0) {
            if (want_reply)
                chan_reply(true);
            chan_text("Monocs: the console is in use (another ssh or telnet session)\r\n");
            chan_close();
            return;
        } else {
            C->chan_shell = true;
            C->hangup_pending = false;
            if (C->pty_rows > 0)
                monocs_console_set_size(C->pty_rows, C->pty_cols);
            monocs_console_input("\r", 1);    // シェルにプロンプトを出させる
            ok = true;
        }
    } else if (str_is(type, tn, "env")) {
        // MONOCS_STOP_APP=1（ssh -o SetEnv=MONOCS_STOP_APP=1。nos.py --stop-app）: exec がコンソールのアプリに待たされたら、
        // そのアプリを止めてから動かす。ほかの環境変数は受け取るだけ（使わない）
        int nn, vn;
        const uint8_t *name = get_str(&r, &nn), *val = get_str(&r, &vn);
        if (!r.err && str_is(name, nn, "MONOCS_STOP_APP") && vn == 1 && val[0] == '1')
            C->stop_app = true;
        ok = true;
    } else if (str_is(type, tn, "exec")) {
        int n;
        const uint8_t *cmd = get_str(&r, &n);
        // 行は pump_exec が、ほかの接続の exec が終わってから始める（monocs_rexec_start は同時に 1 つ）
        if (!r.err && !C->chan_shell && !C->chan_exec && n < 8192) {
            C->exec_cmd = malloc((size_t)n + 1);
            C->exec_in = malloc(CHAN_WINDOW);
            if (C->exec_cmd && C->exec_in) {
                memcpy(C->exec_cmd, cmd, (size_t)n);
                C->exec_cmd[n] = '\0';
                C->chan_exec = true;
                C->exec_started = C->exec_in_eof = C->exec_eof_sent = C->exec_told = false;
                C->exec_since = timer_now_us();
                C->exec_in_len = 0;
                ok = true;
            } else {
                exec_end();
            }
        }
        if (!ok) {
            if (want_reply)
                chan_reply(false);
            chan_close();
            return;
        }
    } else if (str_is(type, tn, "subsystem")) {
        // sftp（scp・sshfs も）。ほかの subsystem はない
        int n;
        const uint8_t *name = get_str(&r, &n);
        if (!r.err && str_is(name, n, "sftp") && !C->chan_shell && !C->chan_exec && !C->sftp) {
            C->sftp = sftp_new();
            C->sftp_eof = false;
            ok = C->sftp != nullptr;
        }
        if (!ok) {
            if (want_reply)
                chan_reply(false);
            chan_close();
            return;
        }
    }
    if (want_reply)
        chan_reply(ok);
}

static void handle_channel_data(const uint8_t *payload, int len)
{
    Rd r = {payload, len, 1, false};
    int n;
    get_u32(&r);
    const uint8_t *data = get_str(&r, &n);
    if (r.err || !C->chan_open)
        return;
    if ((uint32_t)n > C->my_window) {
        disconnect(2, "window exceeded");
        return;
    }
    C->my_window -= (uint32_t)n;
    if (C->sftp) {
        // 処理するまでためる（窓は pump_sftp が、処理した分だけ足し直す）
        if (!sftp_feed(C->sftp, data, (size_t)n))
            disconnect(2, "sftp buffer overflow");
        return;
    }
    if (C->chan_exec) {
        // 行が受け取るまでためる（窓は pump_exec が、受け取った分だけ足し直す）
        if (C->exec_in_len + (uint32_t)n <= CHAN_WINDOW) {
            memcpy(C->exec_in + C->exec_in_len, data, (size_t)n);
            C->exec_in_len += (uint32_t)n;
        }
        return;
    }
    if (C->chan_shell && n > 0)
        monocs_console_input(data, n);
    if (C->my_window < CHAN_WINDOW / 2) {      // 使った分を足し直す
        uint8_t tmp[16];
        Buf b = {tmp, 0, sizeof tmp, false};
        put_u8(&b, MSG_CHANNEL_WINDOW_ADJUST);
        put_u32(&b, C->peer_chan);
        put_u32(&b, CHAN_WINDOW - C->my_window);
        send_buf(&b);
        C->my_window = CHAN_WINDOW;
    }
}

// ---- 受け取ったパケットを扱う ----

// サービスの要求（鍵交換の後、最初に 1 回。ssh-userauth だけ）
static void handle_service_request(int type, const uint8_t *p, int len)
{
    Rd r = {p, len, 1, false};
    int n;
    const uint8_t *name = get_str(&r, &n);
    if (type != MSG_SERVICE_REQUEST || r.err || !str_is(name, n, "ssh-userauth")) {
        disconnect(7, "service not available");
        return;
    }
    uint8_t tmp[32];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, MSG_SERVICE_ACCEPT);
    put_cstr(&b, "ssh-userauth");
    send_buf(&b);
    C->phase = P_AUTH;
}

// 認証が済んだ後のメッセージ（接続の手順。RFC 4254）
static void handle_connection_msg(int type, const uint8_t *p, int len)
{
    Rd r = {p, len, 1, false};
    switch (type) {
    case MSG_USERAUTH_REQUEST:      // 認証の後のものは無視する（RFC 4252 5.1）
    case MSG_CHANNEL_EXTENDED_DATA:
        break;
    case MSG_CHANNEL_EOF:
        if (C->chan_exec)
            C->exec_in_eof = true;     // 標準入力の終わり（ためた分を渡した後に知らせる）
        if (C->sftp)
            C->sftp_eof = true;
        break;
    case MSG_GLOBAL_REQUEST: {
        int n;
        get_str(&r, &n);
        if (!r.err && get_u8(&r)) {     // 返事がほしい: 対応していない、と返す（keepalive はこれで足りる）
            uint8_t m = MSG_REQUEST_FAILURE;
            send_packet(&m, 1);
        }
        break;
    }
    case MSG_CHANNEL_OPEN:
        handle_channel_open(p, len);
        break;
    case MSG_CHANNEL_REQUEST:
        handle_channel_request(p, len);
        break;
    case MSG_CHANNEL_DATA:
        handle_channel_data(p, len);
        break;
    case MSG_CHANNEL_WINDOW_ADJUST: {
        get_u32(&r);
        uint32_t add = get_u32(&r);
        if (!r.err && C->chan_open)
            C->peer_window += add;
        break;
    }
    case MSG_CHANNEL_CLOSE:
        if (C->chan_open) {
            chan_close();
            if (C->chan_shell)
                monocs_console_close();
            exec_end();     // 動いている途中なら止める
            sftp_end();
            C->chan_open = C->chan_shell = false;
        }
        break;
    default: {
        uint8_t tmp[8];
        Buf b = {tmp, 0, sizeof tmp, false};
        put_u8(&b, MSG_UNIMPLEMENTED);
        put_u32(&b, C->c2s.seq - 1);
        send_buf(&b);
    }
    }
}

// 受け取ったパケット 1 つ。どの段階（phase）かで、受け付けるメッセージが決まる
static void handle_packet(const uint8_t *p, int len)
{
    if (len < 1)
        return;
    int type = p[0];
    // strict KEX: 最初の鍵交換の間は、決まった順のメッセージだけ（IGNORE・DEBUG なども認めない）。
    // 相手の KEXINIT より前に別のものが来ていたら、strict KEX とわかった時点で切る
    if (!C->kex_done) {
        if (C->phase == P_KEXINIT && type != MSG_KEXINIT)
            C->junk_before_kexinit = true;
        if (C->strict_kex && C->phase >= P_KEX_ECDH && type != MSG_KEX_ECDH_INIT && type != MSG_NEWKEYS) {
            disconnect(3, "strict KEX violation");
            return;
        }
    }
    // どの段階でも受け付けるもの
    if (type == MSG_DISCONNECT) {
        end_conn();
        return;
    }
    if (type == MSG_IGNORE || type == MSG_DEBUG || type == MSG_UNIMPLEMENTED || type == MSG_EXT_INFO)
        return;
    if (type == MSG_KEXINIT) {
        if (C->phase != P_KEXINIT && C->phase != P_OPEN) {
            disconnect(2, "unexpected KEXINIT");
            return;
        }
        C->rekeying = C->phase == P_OPEN;     // 認証の後に来たら、相手が鍵を交換し直す
        handle_kexinit(p, len);
        return;
    }
    switch (C->phase) {
    case P_KEXINIT:
        disconnect(2, "expected KEXINIT");
        break;
    case P_KEX_ECDH:
        if (C->ignore_next_kex)        // 外れた当て推量のパケット
            C->ignore_next_kex = false;
        else if (type != MSG_KEX_ECDH_INIT)
            disconnect(2, "expected KEX_ECDH_INIT");
        else
            handle_ecdh_init(p, len);
        break;
    case P_NEWKEYS:
        if (type != MSG_NEWKEYS)
            disconnect(2, "expected NEWKEYS");
        else
            handle_newkeys();
        break;
    case P_SERVICE:
        handle_service_request(type, p, len);
        break;
    case P_AUTH:
        if (type == MSG_USERAUTH_REQUEST)
            handle_userauth(p, len);
        else
            disconnect(2, "expected USERAUTH_REQUEST");
        break;
    case P_OPEN:
        handle_connection_msg(type, p, len);
        break;
    }
}

// rx にたまった分から、そろったパケットを取り出して扱う。何かしたら true
static bool process_rx(void)
{
    bool work = false;
    while (C->conn >= 0) {
        if (C->phase == P_VERSION) {
            // 相手の版の行（\n まで。\r は取る）。255 文字まで（RFC 4253 4.2）
            uint8_t *nl = memchr(C->rx, '\n', (size_t)C->rx_len);
            if (!nl) {
                if (C->rx_len > 255)
                    end_conn();
                break;
            }
            int n = (int)(nl - C->rx);
            int used = n + 1;
            if (n > 0 && C->rx[n - 1] == '\r')
                n--;
            if (n < 8 || n > 255 || (memcmp(C->rx, "SSH-2.0-", 8) && memcmp(C->rx, "SSH-1.99-", 9))) {
                end_conn();
                break;
            }
            memcpy(C->client_version, C->rx, (size_t)n);
            C->client_version[n] = 0;
            memmove(C->rx, C->rx + used, (size_t)(C->rx_len - used));
            C->rx_len -= used;
            C->phase = P_KEXINIT;
            work = true;
            continue;
        }
        int block = C->c2s.on ? 16 : 8, maclen = C->c2s.on ? 32 : 0;
        if (C->rx_len < block)
            break;
        if (!C->rx_head_done) {
            if (C->c2s.on)
                ctr_crypt(&C->c2s, C->rx, 16);    // 最初のブロックを復号して、長さを読む
            C->rx_head_done = true;
        }
        uint32_t plen = (uint32_t)C->rx[0] << 24 | C->rx[1] << 16 | C->rx[2] << 8 | C->rx[3];
        if (plen < 5 || plen > MAX_PACKET || (plen + 4) % (uint32_t)block != 0) {
            disconnect(2, "bad packet length");
            break;
        }
        int total = (int)plen + 4;
        if (C->rx_len < total + maclen)
            break;
        if (C->c2s.on) {
            ctr_crypt(&C->c2s, C->rx + 16, total - 16);
            uint8_t mac[32];
            mac_compute(&C->c2s, C->rx, total, mac);
            if (!equal_ct(mac, C->rx + total, 32)) {
                disconnect(5, "MAC error");
                break;
            }
        }
        int pad = C->rx[4], payload_len = (int)plen - 1 - pad;
        C->c2s.seq++;
        C->rx_head_done = false;
        if (pad < 4 || payload_len < 0) {
            disconnect(2, "bad padding");
            break;
        }
        static uint8_t payload[MAX_PACKET];
        memcpy(payload, C->rx + 5, (size_t)payload_len);
        memmove(C->rx, C->rx + total + maclen, (size_t)(C->rx_len - total - maclen));
        C->rx_len -= total + maclen;
        handle_packet(payload, payload_len);
        work = true;
    }
    return work;
}

static void send_chan_data(uint8_t *msg, int n)
{
    msg[0] = MSG_CHANNEL_DATA;
    msg[1] = (uint8_t)(C->peer_chan >> 24);
    msg[2] = (uint8_t)(C->peer_chan >> 16);
    msg[3] = (uint8_t)(C->peer_chan >> 8);
    msg[4] = (uint8_t)C->peer_chan;
    msg[5] = 0;
    msg[6] = 0;
    msg[7] = (uint8_t)(n >> 8);
    msg[8] = (uint8_t)n;
    send_packet(msg, 9 + n);
    C->peer_window -= (uint32_t)n;
}

// コンソールに出た文字を、相手の窓の分だけチャンネルのデータにして送る。
// 出力が流れ続けているとき（前に送ってから CON_STREAM_US の内）は、CON_HOLD_US までためて大きなパケットにする。
// 少しずつ送ると TCP の切れ端（Nagle を切っている）が Wi-Fi の送信の待ち行列をあふれさせ、捨てられた分を
// 再送の時間切れ（1〜1.5 秒）まで待って、画面が止まったり動いたりした（2026-11-20、数時間出し続けるアプリで）。
// 打った文字の折り返しのような、ぽつぽつ出るものは待たずに送る
static bool pump_console(void)
{
    if (!C->chan_shell || C->rekeying || C->phase != P_OPEN || C->chan_eof_sent)
        return false;
    uint32_t room = C->peer_window < C->peer_max_packet ? C->peer_window : C->peer_max_packet;
    if (room > CON_CHUNK)
        room = CON_CHUNK;
    if (room == 0 || TX_SIZE - C->tx_len < 16384)
        return false;
    if (monocs_console_hangup())
        C->hangup_pending = true;      // シェルで exit が打たれた: 残りの出力を送り切ってから閉じる
    // ためている間は読みに行かない（休まず読むと、1 文字ごとに書くコア 0 と写しのロックを奪い合い、
    // アプリの出力が半分以下の速さになった）
    uint64_t now = timer_now_us();
    bool streaming = now - C->con_sent_us < CON_STREAM_US;
    if (C->con_len > 0 && (uint32_t)C->con_len < room && streaming && !C->hangup_pending &&
        now - C->con_first_us < CON_HOLD_US)
        return false;
    int n = 0;
    if ((uint32_t)C->con_len < room) {
        n = monocs_console_read(C->con_msg + 9 + C->con_len, (int)room - C->con_len);
        if (n > 0) {
            if (C->con_len == 0)
                C->con_first_us = now;
            C->con_len += n;
        }
    }
    if (C->con_len == 0) {
        if (C->hangup_pending) {
            // 終了の値 0 を知らせて、チャンネルを閉じる（RFC 4254 6.10）。相手の ssh は 0 で終わる
            uint8_t tmp[40];
            Buf b = {tmp, 0, sizeof tmp, false};
            put_u8(&b, MSG_CHANNEL_REQUEST);
            put_u32(&b, C->peer_chan);
            put_cstr(&b, "exit-status");
            put_u8(&b, 0);
            put_u32(&b, 0);
            send_buf(&b);
            C->hangup_pending = false;
            chan_close();
            monocs_console_close();
            C->chan_shell = false;
        }
        return false;
    }
    if ((uint32_t)C->con_len < room && streaming && !C->hangup_pending && now - C->con_first_us < CON_HOLD_US)
        return false;       // もう少しためる（流れ続けている途中で、初めて読んだとき）
    int k = C->con_len < (int)room ? C->con_len : (int)room;
    send_chan_data(C->con_msg, k);
    C->con_len -= k;
    if (C->con_len > 0) {
        memmove(C->con_msg + 9, C->con_msg + 9 + k, (size_t)C->con_len);
        C->con_first_us = now;
    }
    C->con_sent_us = now;
    return true;
}

// exec を 2 秒以上待たせているとき、1 回だけ相手の標準エラーに知らせる（黙って止まって見えないように）
static void exec_tell_waiting(const char *msg)
{
    if (C->exec_told || timer_now_us() - C->exec_since < 2000000)
        return;
    int n = (int)strlen(msg);
    if ((uint32_t)n > C->peer_window || TX_SIZE - C->tx_len < 16384)
        return;
    uint8_t buf[160];
    Buf b = {buf, 0, sizeof buf, false};
    put_u8(&b, MSG_CHANNEL_EXTENDED_DATA);
    put_u32(&b, C->peer_chan);
    put_u32(&b, 1);             // SSH_EXTENDED_DATA_STDERR（RFC 4254 5.2）
    put_str(&b, (const uint8_t *)msg, n);
    send_buf(&b);
    C->peer_window -= (uint32_t)n;
    C->exec_told = true;
}

// sftp: ためたパケットを処理し、答えを相手の窓の分だけ送る。処理した分だけ窓を足し直す
static bool pump_sftp(void)
{
    if (!C->sftp || C->rekeying || C->phase != P_OPEN)
        return false;
    bool work = false, bad;
    if (sftp_run(C->sftp, &bad) > 0)
        work = true;
    if (bad) {
        disconnect(2, "bad sftp packet");
        return true;
    }
    uint32_t pending = (uint32_t)sftp_pending_input(C->sftp);
    if (C->my_window + pending < CHAN_WINDOW / 2) {
        uint8_t tmp[16];
        Buf b = {tmp, 0, sizeof tmp, false};
        put_u8(&b, MSG_CHANNEL_WINDOW_ADJUST);
        put_u32(&b, C->peer_chan);
        put_u32(&b, CHAN_WINDOW - C->my_window - pending);
        send_buf(&b);
        C->my_window = CHAN_WINDOW - pending;
    }
    static uint8_t msg[9 + 8192];
    for (;;) {
        size_t len;
        const uint8_t *out = sftp_output(C->sftp, &len);
        if (len == 0)
            break;
        uint32_t room = C->peer_window < C->peer_max_packet ? C->peer_window : C->peer_max_packet;
        if (room > 8192)
            room = 8192;
        if (room == 0 || TX_SIZE - C->tx_len < 16384)
            return work;
        int n = len < room ? (int)len : (int)room;
        memcpy(msg + 9, out, (size_t)n);
        send_chan_data(msg, n);
        sftp_consumed(C->sftp, (size_t)n);
        work = true;
    }
    if (C->sftp_eof && sftp_pending_input(C->sftp) == 0) {
        // 相手が送り終えた（sftp の終わり）: 終了の値 0 を知らせて閉じる（scp は、これがないと失敗とみなす）
        uint8_t tmp[40];
        Buf b = {tmp, 0, sizeof tmp, false};
        put_u8(&b, MSG_CHANNEL_REQUEST);
        put_u32(&b, C->peer_chan);
        put_cstr(&b, "exit-status");
        put_u8(&b, 0);
        put_u32(&b, 0);
        send_buf(&b);
        b.len = 0;
        put_u8(&b, MSG_CHANNEL_EOF);
        put_u32(&b, C->peer_chan);
        send_buf(&b);
        sftp_end();
        chan_close();
        return true;
    }
    return work;
}

// exec: 標準入力を行に渡し、行の出力を送り、終わったら終了の値を知らせて閉じる（RFC 4254 6.5・6.10）
static bool pump_exec(void)
{
    if (!C->chan_exec || C->rekeying || C->phase != P_OPEN)
        return false;
    if (!C->exec_started) {
        if (monocs_rexec_start(C->exec_cmd) != 0) {
            // ほかの接続の exec が動いている: 終わるのを待つ
            // コア 0 のいる番地も知らせる（シェルが止まったときの調べ。Pi 5 で /boot/kernel8.sym・kernel.map と比べる）
            uint64_t pc, lr;
            monocs_core0_where(&pc, &lr);
            char m[160];
            snprintf(m, sizeof m, "Monocs: waiting for another ssh command to finish... (core 0 at %#llx, x30 %#llx)\r\n",
                     (unsigned long long)pc, (unsigned long long)lr);
            exec_tell_waiting(m);
            return false;
        }
        C->exec_started = true;
    }
    if (monocs_rexec_queued()) {
        // シェルがプロンプトに戻るのを待っている（コンソールでアプリ・コマンドが動いている）
        MonocsSysStat st;
        monocs_sysstat(&st);
        char msg[128];
        if (C->stop_app && !C->app_stopped && timer_now_us() - C->exec_since >= 1000000) {
            // 頼まれていれば、1 秒待っても空かないとき、コンソールのアプリ（なければシェルの繰り返し）を止める
            monocs_console_interrupt();
            C->app_stopped = true;
            snprintf(msg, sizeof msg, "Monocs: stopped '%s' on the console to run this command\r\n",
                     st.app[0] ? st.app : "shell command");
            C->exec_told = false;       // 止めたことは、待っていると知らせた後でも、すぐ知らせる
            C->exec_since = 0;
            exec_tell_waiting(msg);
            return false;
        }
        if (st.app[0])
            snprintf(msg, sizeof msg, "Monocs: waiting for '%s' on the console to finish (quit it to continue)...\r\n", st.app);
        else
            snprintf(msg, sizeof msg, "Monocs: waiting for the shell on the console to finish a command...\r\n");
        exec_tell_waiting(msg);
    }
    bool work = false;
    if (C->exec_in_len > 0) {
        int k = monocs_rexec_write(C->exec_in, (int)C->exec_in_len);
        if (k > 0) {
            memmove(C->exec_in, C->exec_in + k, C->exec_in_len - (uint32_t)k);
            C->exec_in_len -= (uint32_t)k;
            work = true;
        }
    }
    if (C->exec_in_eof && C->exec_in_len == 0 && !C->exec_eof_sent) {
        monocs_rexec_write_eof();
        C->exec_eof_sent = true;
    }
    // 渡した分の窓を足し直す（ためている分は数えない）
    if (C->my_window + C->exec_in_len < CHAN_WINDOW / 2) {
        uint8_t tmp[16];
        Buf b = {tmp, 0, sizeof tmp, false};
        put_u8(&b, MSG_CHANNEL_WINDOW_ADJUST);
        put_u32(&b, C->peer_chan);
        put_u32(&b, CHAN_WINDOW - C->my_window - C->exec_in_len);
        send_buf(&b);
        C->my_window = CHAN_WINDOW - C->exec_in_len;
    }
    // 終わったかを先に見る（見た後に読む出力が、最後まで）
    int status = monocs_rexec_status();
    static uint8_t msg[9 + 8192];
    for (;;) {
        uint32_t room = C->peer_window < C->peer_max_packet ? C->peer_window : C->peer_max_packet;
        if (room > 8192)
            room = 8192;
        if (room == 0 || TX_SIZE - C->tx_len < 16384)
            return work;    // 相手の窓・送る待ちが空くまで（終了の知らせも、出力を送り切ってから）
        int n = monocs_rexec_read(msg + 9, (int)room);
        if (n <= 0)
            break;
        send_chan_data(msg, n);
        work = true;
    }
    if (status < 0)
        return work;
    uint8_t tmp[40];
    Buf b = {tmp, 0, sizeof tmp, false};
    put_u8(&b, MSG_CHANNEL_REQUEST);
    put_u32(&b, C->peer_chan);
    put_cstr(&b, "exit-status");
    put_u8(&b, 0);
    put_u32(&b, (uint32_t)status);
    send_buf(&b);
    b.len = 0;
    put_u8(&b, MSG_CHANNEL_EOF);
    put_u32(&b, C->peer_chan);
    send_buf(&b);
    exec_end();
    chan_close();
    return true;
}

static void begin_conn(int h)
{
    C->conn = h;
    C->from_usb = monocs_tcp_netif(h) == MONOCS_NETIF_USB;
    C->phase = P_VERSION;
    C->authed = C->rekeying = C->strict_kex = C->ext_info = C->ignore_next_kex = C->have_session_id = C->kex_done = C->junk_before_kexinit = false;
    C->rx_len = C->tx_len = C->tx_pos = 0;
    C->con_len = 0;
    C->con_sent_us = 0;
    C->rx_head_done = false;
    C->auth_tries = 0;
    C->chan_open = C->chan_shell = C->chan_exec = C->stop_app = C->app_stopped = false;
    C->started_us = timer_now_us();
    memset(&C->c2s, 0, sizeof C->c2s);
    memset(&C->s2c, 0, sizeof C->s2c);
    rng_seed();
    // こちらの版の行と KEXINIT を先に送る
    memcpy(C->tx, VERSION "\r\n", sizeof VERSION + 1);
    C->tx_len = (int)sizeof VERSION + 1;
    send_kexinit();
}

// ---- サービス ----
int service_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rng_seed();
    if (!load_host_key()) {
        printf("sshd: cannot prepare the host key\n");
        return 1;
    }
    for (int i = 0; i < SSHD_CONNS; i++)
        conns[i].conn = -1;
    listener = monocs_tcp_listen(SSHD_PORT);
    if (listener < 0) {
        printf("sshd: cannot listen on port %d\n", SSHD_PORT);
        return 1;
    }
    return 0;
}

void service_stop(void)
{
    for (int i = 0; i < SSHD_CONNS; i++) {
        C = &conns[i];
        if (C->conn >= 0)
            disconnect(11, "sshd is stopping");
    }
}

static int poll_conn(void);

int service_poll(void)
{
    int work = 0;
    int h = monocs_tcp_accept(listener, 0);
    if (h >= 0) {
        int i = 0;
        while (i < SSHD_CONNS && conns[i].conn >= 0)
            i++;
        if (i == SSHD_CONNS) {
            monocs_tcp_close(h);      // 同時につなげるのは SSHD_CONNS 個まで
        } else {
            C = &conns[i];
            begin_conn(h);
        }
        work = 1;
    }
    for (int i = 0; i < SSHD_CONNS; i++) {
        C = &conns[i];
        if (C->conn >= 0 && poll_conn())
            work = 1;
    }
    return work;
}

// 1 つの接続（C）を進める。仕事をしたら 0 以外
static int poll_conn(void)
{
    int work = 0;
    // 受信
    if (C->rx_len < RX_SIZE) {
        int n = monocs_tcp_recv(C->conn, C->rx + C->rx_len, RX_SIZE - C->rx_len, 0);
        if (n == 0 || n == -2) {
            end_conn();
            return 1;
        }
        if (n > 0) {
            C->rx_len += n;
            work = 1;
        }
    }
    if (process_rx())
        work = 1;
    if (C->conn < 0)
        return 1;
    if (!C->authed && timer_now_us() - C->started_us > LOGIN_GRACE_US) {
        disconnect(2, "login timeout");
        return 1;
    }
    for (int i = 0; i < 4 && pump_console(); i++)
        work = 1;
    if (pump_exec())
        work = 1;
    if (pump_sftp())
        work = 1;
    // 送信
    if (C->conn >= 0 && C->tx_pos < C->tx_len) {
        int s = monocs_tcp_send(C->conn, C->tx + C->tx_pos, C->tx_len - C->tx_pos, 0);
        if (s < 0) {
            end_conn();
            return 1;
        }
        C->tx_pos += s;
        if (s > 0)
            work = 1;
        if (C->tx_pos == C->tx_len)
            C->tx_pos = C->tx_len = 0;
        else if (C->tx_pos > TX_SIZE / 2) {
            memmove(C->tx, C->tx + C->tx_pos, (size_t)(C->tx_len - C->tx_pos));
            C->tx_len -= C->tx_pos;
            C->tx_pos = 0;
        }
    }
    return work;    // チャンネルを閉じた後は、相手が切るのを待つ（OpenSSH はチャンネルがなくなると自分で切る）
}
