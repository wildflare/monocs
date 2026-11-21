// コンソールの写し（タップ）: サービス（services/telnetd.c など）がコンソールにつながるための仕組み。
// コンソールに出した文字（uart_putc。UART・USB と同じもの）を 64KB のバッファに写し、サービスが
// service_poll の中で読み出す（contap_read）。サービスからの入力はシェルの受信バッファに入れる（uart_rx_push）。
// カーネルからサービスの関数を呼ぶことはしない（出力はどのコア・割り込みを止めた場所からも来るため）。
//
// バッファがいっぱいなら、サービスが読むのを待つ（サービスはコア 3 で動くので、別のコアからは待てる）。
// 待つ間は USB の割り込みの処理を自分で呼ぶ（TCP の確認応答が届かないとサービスが送れないため）。
// 0.5 秒進まなければ、読まれるまで捨てる（相手が受け取らないとき）。サービスのコア自身の出力は待たない。

#include <stdatomic.h>
#include "contap.h"
#include "svc.h"
#include "sysreg.h"
#include "../drivers/timer.h"
#include "../drivers/uart.h"
#include "../drivers/usb.h"
#include "../libc/signal_os.h"

#define TAP_SIZE (64 * 1024)
static uint8_t buf[TAP_SIZE];
static uint32_t head, tail;
static atomic_flag lock = ATOMIC_FLAG_INIT;
static atomic_bool active;
static bool stalled;
static atomic_int win_rows, win_cols;
static bool registered;
static atomic_bool hangup;              // シェルの exit から、サービスへ
static _Atomic uint64_t last_input_us;  // 写しから最後に入力があった時刻（exit がどこで打たれたかの目安）
#define HANGUP_INPUT_US 2000000         // exit の Enter は、この時間の内に写しから来ているはず

static uint64_t lock_take(void)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();
    while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire))
        __asm__ volatile("yield");
    return daif;
}

static void lock_give(uint64_t daif)
{
    atomic_flag_clear_explicit(&lock, memory_order_release);
    write_sysreg(daif, daif);
}

static bool push(uint8_t c)
{
    uint64_t daif = lock_take();
    uint32_t next = (head + 1) % TAP_SIZE;
    bool ok = next != tail;
    if (ok) {
        buf[head] = c;
        head = next;
    }
    lock_give(daif);
    return ok;
}

// コンソールの出力（uart_putc から）。開いていて、読まれていれば true
static bool tap_putc(char c)
{
    if (!atomic_load(&active))
        return false;
    uint64_t deadline = 0;
    while (!push((uint8_t)c)) {
        if (stalled || cpu_id() == SVC_CORE || !atomic_load(&active))
            return false;
        __asm__ volatile("dsb sy\n\tsev" ::: "memory");     // サービスのコアを起こす
        usb_handle_irq();
        uint64_t now = timer_now_us();
        if (!deadline)
            deadline = now + 500000;
        else if (now > deadline)
            stalled = true;
    }
    return !stalled;
}

int contap_open(void)
{
    if (atomic_load(&active))
        return -1;
    if (!registered) {
        uart_add_tx_mirror(tap_putc, nullptr);
        registered = true;
    }
    uint64_t daif = lock_take();
    head = tail = 0;
    stalled = false;
    lock_give(daif);
    atomic_store(&hangup, false);
    atomic_store(&last_input_us, 0);
    atomic_store(&active, true);
    return 0;
}

// 写しが開いているか（telnet・ssh でだれかがつないでいる。httpd の /.status。os/api の monocs_console_remote）
bool contap_active(void)
{
    return atomic_load(&active);
}

void contap_close(void)
{
    atomic_store(&active, false);
    atomic_store(&win_rows, 0);
    atomic_store(&win_cols, 0);
}

int contap_read(void *out, int n)
{
    uint8_t *p = out;
    uint64_t daif = lock_take();
    int k = 0;
    while (k < n && tail != head) {
        p[k++] = buf[tail];
        tail = (tail + 1) % TAP_SIZE;
    }
    if (k)
        stalled = false;
    lock_give(daif);
    return k;
}

void contap_input(const void *p, int n)
{
    if (n > 0) {
        atomic_store(&last_input_us, timer_now_us());
        uart_rx_push(p, (size_t)n);
    }
}

bool contap_request_hangup(void)
{
    uint64_t last = atomic_load(&last_input_us);
    if (!atomic_load(&active) || last == 0 || timer_now_us() - last > HANGUP_INPUT_US)
        return false;
    atomic_store(&hangup, true);
    return true;
}

bool contap_hangup(void)
{
    return atomic_load(&active) && atomic_exchange(&hangup, false);
}

void contap_set_size(int rows, int cols)
{
    int old_r = atomic_exchange(&win_rows, rows), old_c = atomic_exchange(&win_cols, cols);
    if (old_r != rows || old_c != cols)
        signal_post_winch();    // アプリが SIGWINCH に関数を登録していれば渡す（os/libc/signal.c）
}

bool contap_size(int *rows, int *cols)
{
    int r = atomic_load(&win_rows), c = atomic_load(&win_cols);
    if (!atomic_load(&active) || r <= 0 || c <= 0)
        return false;
    *rows = r;
    *cols = c;
    return true;
}
