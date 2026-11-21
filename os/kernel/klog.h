// カーネルの記録（dmesg）: あとから読める、時刻付きの輪のバッファ（64KB）
//
// 記録するもの:
//   - 起動からシェルの最初のプロンプトまでのコンソールの表示すべて（USB のコンソールは、つながる前の表示が
//     見えないため）
//   - その後は、klog_printf で書いたもの（Wi-Fi の接続・切断、サービスの開始・停止、USB、SD カードの誤り）と、
//     klog_tee_begin〜end の間にコンソールに出したもの（例外の表示など）
// 読むのはシェルの dmesg、HTTP の GET /.log（OS API の monocs_klog_read）。SD カードの /var/log/dmesg にも書き足す
// （os/kernel/klog_file.c。前の起動のものは /var/log/dmesg.1）。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// コンソールに出す 1 文字（uart_putc から）。記録する間だけ呼ばれる（klog_capture_active）。
// locked が false なら、ロックを取らない（MMU の前。排他アクセスが使えない）
void klog_console_char(char c, bool locked);
// コンソールの表示を記録する間か（起動中か、このコアが klog_tee_begin の中）
extern volatile bool klog_boot;
extern volatile int klog_tee_any;
static inline bool klog_capture_active(void)
{
    return klog_boot || klog_tee_any;
}
// シェルが最初のプロンプトを出す前に呼ぶ（ここから先は、コンソールの表示をそのままは記録しない）
void klog_boot_done(void);
// この間にこのコアがコンソールに出したものも記録する（重ねてよい）
void klog_tee_begin(void);
void klog_tee_end(void);
// 記録だけする（コンソールには出さない）。行の終わりの \n はなくてもよい
void klog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// コンソールにも同じことを出したときの記録: 起動中は、コンソールの表示がそのまま記録に入るので書かない
#define klog_printed(...) do { if (!klog_boot) klog_printf(__VA_ARGS__); } while (0)
// 古いものから buf に写す（size まで。輪が一周していたら、途中の行は飛ばす）。写したバイト数
size_t klog_read(char *buf, size_t size);
// *pos（これまでに読んだ位置。最初は 0）から後に書かれたものを写し、*pos を進める。写したバイト数（一周して消えた分は飛ばす）
size_t klog_read_since(uint64_t *pos, char *buf, size_t size);
// 記録の大きさ（バイト）
size_t klog_size(void);
void klog_clear(void);

// 起動の終わり方（再起動をまたいで残る。os/kernel/klog.c）
#define KLOG_END_RUNNING  0     // 動いていた（そのままリセットされた: 固まってウォッチドッグか、電源を切らずにリセット）
#define KLOG_END_REBOOT   1
#define KLOG_END_POWEROFF 2
#define KLOG_END_HALT     3     // カーネルが止まった（例外など。kernel_halt）
void klog_set_end(int how);
// 書いた分をすべて RAM へ（止まる・再起動の前に）
void klog_persist_flush(void);
// 前の起動の記録（残っていれば true。text は行の頭から、how は KLOG_END_*）
bool klog_previous(const char **text, size_t *len, int *how);
