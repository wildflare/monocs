// Monocs の独自 API（アプリから使う）
// 実体はカーネルにあり、OS API の関数表（os/api/api.def）を通して呼ぶ。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TIMER_HZ    100

uint64_t timer_ticks(void);             // 起動からのティック数（TIMER_HZ 回/秒）
uint64_t timer_now_us(void);            // 起動からの時間（マイクロ秒）
void timer_delay_us(uint64_t us);       // 指定時間だけ待つ

void led_set(bool on);                  // ACT LED（緑）の点灯・消灯

void uart_putc(char c);                 // コンソールに 1 文字出す（改行の変換なし）
char uart_getc(void);                   // コンソールから 1 文字読む（届くまで待つ）
bool uart_has_input(void);              // 読める文字があるか

// 端末（picocom など）の画面の行数と桁数。端末に問い合わせる（ESC [ 6 n）。答えがなければ 24 行 80 桁で false。
// Linux と同じ ioctl(fd, TIOCGWINSZ, &ws)・tcgetwinsize でも取れる（中身は同じ）。
// アプリの getchar() は、Linux の端末と同じく既定で 1 行ずつ・エコーあり（termios。tcsetattr で ICANON・
// ECHO を外すと 1 文字ずつ）。矢印キーは ESC [ A〜D の 3 文字で届く
bool monocs_term_size(int *rows, int *cols);
// 端末の桁数（telnet は知らされた大きさ、ほかは最後に問い合わせた値。まだなら一度だけ問い合わせる）
int monocs_term_cols(void);

// TCP（USB イーサネット。Monocs は 192.168.7.2、Pi 5 は 192.168.7.1）。ハンドルは 0 以上、失敗は -1。
// timeout_ms: 0 = 待たない、負 = いつまでも。アプリが終わると（Ctrl-C でも）、開いたままの接続は閉じる。
// 例: samples/tcp_echo.c（サーバー）、samples/http_get.c（クライアント）
int monocs_tcp_listen(int port);                                    // 待ち受けを始める（どの口に届いたものも）
int monocs_tcp_listen_addr(const char *ip, int port);               // ip（"192.168.7.2" など）に、その口から届いたものだけ
int monocs_tcp_accept(int listener, int timeout_ms);                // 接続を受け付ける
int monocs_tcp_connect(const char *ip, int port, int timeout_ms);   // つなぐ（ip は "192.168.7.1" の形か、名前 "example.com"）
int monocs_tcp_send(int h, const void *buf, int len, int timeout_ms); // 送れたバイト数（-1 = つながっていない）
int monocs_tcp_recv(int h, void *buf, int len, int timeout_ms);     // バイト数、0 = 相手が閉じた、-1 = 時間切れ、-2 = エラー
void monocs_tcp_close(int h);
// 受け付けた接続が届いた口。USB（ケーブルの先の Pi 5）は信用し、Wi-Fi からのものはパスワードを確かめる、という
// 使い分けに（services/httpd.c・telnetd.c、monocs_auth.h）。ハンドルが違えば -1
#define MONOCS_NETIF_USB  0
#define MONOCS_NETIF_WIFI 1
int monocs_tcp_netif(int h);
int monocs_tcp_peer(int h, char *ip, int *port);   // 相手のアドレス（ip は 16 バイト以上。NULL 可）とポート（NULL 可）。0 で成功

// UDP（待つ形。timeout_ms: 0 = 待たない、負 = いつまでも）
// 名前（"example.com"）から IP アドレス（ip に "93.184.216.34" の形。size は 16 以上）。0 = 成功、-1 = 引けない・時間切れ。
// "192.168.7.1" の形ならそのまま返す。timeout_ms: 0 = 待たない（問い合わせは始まり、引けたら覚えているので次は成功する）
int monocs_resolve(const char *name, char *ip, int size, int timeout_ms);
int monocs_udp_open(int port);                                       // ソケット（0 なら空いている番号）。-1 = 失敗
int monocs_udp_sendto(int h, const char *ip, int port, const void *buf, int len);   // 送ったバイト数か -1（1472 まで）。ip が名前のときは待たずに引く（引けるまで -1）
int monocs_udp_recvfrom(int h, void *buf, int len, char *ip, int *port, int timeout_ms); // バイト数、-1 = 時間切れ（ip は 16 バイト以上）
void monocs_udp_close(int h);

// ファイルの日時を変える（POSIX の utime。picolibc の utime.h には宣言がない。times が NULL なら今）
struct utimbuf;
int utime(const char *path, const struct utimbuf *times);

// 日時（日時の設定は clock_settime・settimeofday）
void monocs_set_timezone(int minutes);   // タイムゾーン（UTC からの分。JST は 540）
bool monocs_clock_known(void);           // 日時が設定されたか（されるまでは起動を 2026-01-01 として数える）

// SD カードを直接読み書きする（セクタは 512 バイト。成功なら 0）。ファイルシステムを壊しうるので注意。
// 書き換えた後は monocs_disk_discard でキャッシュを捨て、monocs_reboot(false) ですぐ再起動すること
int monocs_disk_read(uint32_t sector, uint32_t count, void *buf);
int monocs_disk_write(uint32_t sector, uint32_t count, const void *buf);
void monocs_disk_sync(bool force);      // 遅らせている書き込みを書き出す（force でなければ 1 秒たったものだけ）
void monocs_disk_discard(void);         // 遅らせている書き込みと読み出しのキャッシュを捨てる
void monocs_reboot(bool sync);          // 再起動する（sync なら、先に書き込みを済ませる）
// 内蔵 Wi-Fi の自動の接続を少し進める（/etc/wpa_supplicant.conf のネットワークにつなぎ、切れたらつなぎ直す。
// 待たない。何かしたら 1）。サービス services/wifid.c が呼び続ける
int monocs_wifi_auto_poll(void);

// コンソールの写し（サービスの telnet サーバーなどが、シェルのコンソールにつながる。os/kernel/contap.c）
int monocs_console_open(void);               // 写しを受け取り始める（ほかが開いていれば -1）
void monocs_console_close(void);
int monocs_console_read(void *buf, int n);   // コンソールに出た文字を読む（待たない。読んだバイト数）
void monocs_console_input(const void *p, int n);  // シェルへの入力を差し込む（Ctrl-C も扱う）
void monocs_console_set_size(int rows, int cols); // 端末の画面の大きさ（monocs_term_size が返す。0 なら知らない）
// readdir が最後に返したものに FAT の隠し属性が付いているか（ls は -a がなければ出さない）
bool monocs_dirent_hidden(void *dir);   // 引数は DIR *
// FAT の属性を set で立て、clear で消す（両方 0 なら読むだけ）。今の属性（MONOCS_FAT_* と、ディレクトリなら 16）か -1
#define MONOCS_FAT_READONLY 0x01
#define MONOCS_FAT_HIDDEN   0x02
#define MONOCS_FAT_SYSTEM   0x04
#define MONOCS_FAT_DIR      0x10
#define MONOCS_FAT_ARCHIVE  0x20
int monocs_fat_attr(const char *path, int set, int clear);
bool monocs_console_remote(void);         // telnet・ssh でだれかがつないでいるか（写しが開いている）
bool monocs_app_running(void);            // コア 0 でアプリが動いているか（シェルがコマンドを待っていない）
int monocs_shell_status(void);            // シェルが最後に動かした行の終了の値（$?）
void monocs_sha256(const void *data, size_t len, uint8_t out[32]);   // SHA-256（FIPS 180-4）
size_t monocs_klog_read(char *buf, size_t size);  // カーネルの記録（dmesg と同じもの。古いものから。写したバイト数）
bool monocs_console_hangup(void);         // シェルで exit・logout が打たれたら true（1 回だけ）。出力を送り切ってから接続を閉じる
#include "monocs_sysstat.h"     // monocs_sysstat（システムの様子。/bin/top）
// シェルの 1 行を動かしてもらう（サービスから。sshd の exec。os/kernel/rexec.c）。シェルがプロンプトで入力を待つ間に
// 動かす（コマンドの実行中なら、終わるまで待つ）。同時に 1 つ。cd・変数は終わったら戻る
int monocs_rexec_start(const char *cmd);  // 頼む（0 = 受け付けた、-1 = ほかの行を動かしている・長すぎる（8KB まで））
int monocs_rexec_read(void *buf, int n);  // 行の標準出力・標準エラーを読む（待たない。読んだバイト数）
int monocs_rexec_write(const void *p, int n);  // 行の標準入力に渡す（待たない。受け取ったバイト数）
void monocs_rexec_write_eof(void);        // 標準入力の終わり
int monocs_rexec_status(void);            // -1 = まだ、0〜255 = 終わった（終了の値。残りの出力は read で）
void monocs_rexec_release(void);          // 後始末（読み終えたら呼ぶ。動いている途中なら止める）
bool monocs_rexec_queued(void);           // 頼んだ行がまだ始まっていない（シェルがプロンプトに戻るのを待っている）
void monocs_console_interrupt(void);      // コンソールで動いているアプリを止める（Ctrl-\ と同じ。シェルの繰り返しも止める）
void monocs_core0_where(uint64_t *pc, uint64_t *lr);    // コア 0 が最後の割り込みのときにいた番地と x30（止まったときの調べ）

// サービス（裏で動き続けるプログラム。シェルの svc start FILE.c、起動時は /services.txt）は、main の代わりに
// 次を定義する。コア 3 で、すべてのサービスの service_poll を順に呼び続ける（os/kernel/svc.c）。例: services/httpd.c
//   int  service_start(int argc, char **argv);  // 始めるとき（0 で成功。なくてもよい）
//   int  service_poll(void);                     // 待たずに戻る。仕事をしたら 0 以外
//   void service_stop(void);                     // 止めるとき（なくてもよい）

// セルフホスト用（シェルの slimcc・slimld コマンドと同じ。引数は argv[0] から。成功なら 0）
int monocs_cc(int argc, char **argv);   // slimcc [-c] [-o OUT.o] [-I DIR] [-D NAME[=VAL]] [-U NAME]
                                           //   [-isystem DIR] [-nostdinc] [-ffunction-sections] [-std=STD] FILE
int monocs_ld(int argc, char **argv);   // slimld [-v] [-Map FILE] -o kernel8.img FILE.o... LIB.a...

// マルチコア（コア 1〜3 で関数を動かす）。アプリが終わると、動いている関数は止まる。
// ほかのコアで動く関数からも、printf・malloc・ファイル操作などを呼べる（C ライブラリの中はロックで守る）。
// errno、strtok・rand などの C ライブラリの内部の状態はコアごと（_Thread_local。2026-11-06 から）。
// アプリが自分で _Thread_local の変数を定義することはできない（リンクでエラーになる）。
// コンソールへの出力は、コアどうしで文字の単位で混ざることがある
int monocs_cores(void);                                          // 使えるコアの数（コア 0 を含む）
bool monocs_core_start(int core, void (*fn)(void *), void *arg); // コア core（1〜3）で fn(arg) を始める
bool monocs_core_wait(int core);                                 // 終わるのを待つ（例外・中断なら false）

// コア間のロック（スピンロック。同じコアなら重ねて取れる）。MONOCS_MUTEX_INIT で初期化する
typedef struct {
    unsigned word;
    int owner;
    int count;
} monocs_mutex;
#define MONOCS_MUTEX_INIT { 0, -1, 0 }
void monocs_mutex_lock(monocs_mutex *m);
void monocs_mutex_unlock(monocs_mutex *m);
