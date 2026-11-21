// services/sshd を Linux（Pi 5）で動かして、本物の ssh コマンドで試すための代わり（Monocs の上では使わない）。
// monocs.h の TCP をソケットで、コンソールの写しを擬似端末の上の /bin/sh で用意し、service_poll を回す。
//   sshd_linux [PORT]        環境変数 SSHD_NETIF=usb なら「USB から届いた」ことにする（既定は Wi-Fi 扱い）
// tools/ssh/test.sh が使う。
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int service_start(int argc, char **argv);
int service_poll(void);
static bool errno_is_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static int listen_port = 2222;

uint64_t timer_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

int monocs_tcp_listen(int port)
{
    (void)port;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)listen_port)};
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 4) != 0)
        return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

int monocs_tcp_accept(int l, int timeout_ms)
{
    (void)timeout_ms;
    int fd = accept4(l, nullptr, nullptr, SOCK_CLOEXEC);  // exec の子（/bin/sh）に渡さない
    if (fd >= 0)
        fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

int monocs_tcp_recv(int h, void *buf, int len, int timeout_ms)
{
    (void)timeout_ms;
    ssize_t n = recv(h, buf, (size_t)len, 0);
    if (n < 0)
        return errno_is_again() ? -1 : -2;
    return (int)n;
}

int monocs_tcp_send(int h, const void *buf, int len, int timeout_ms)
{
    (void)timeout_ms;
    ssize_t n = send(h, buf, (size_t)len, MSG_NOSIGNAL);
    if (n < 0)
        return errno_is_again() ? 0 : -1;
    return (int)n;
}

void monocs_tcp_close(int h)
{
    close(h);
}

int monocs_tcp_netif(int h)
{
    (void)h;
    const char *e = getenv("SSHD_NETIF");
    return e && !strcmp(e, "usb") ? 0 : 1;
}

// コンソールの写し: 擬似端末の上の /bin/sh
static int pty_fd = -1;
static pid_t shell_pid;

int monocs_console_open(void)
{
    if (pty_fd >= 0)
        return -1;
    shell_pid = forkpty(&pty_fd, nullptr, nullptr, nullptr);
    if (shell_pid == 0) {
        setenv("PS1", "monocs-test$ ", 1);
        execl("/bin/sh", "sh", (char *)nullptr);
        _exit(1);
    }
    fcntl(pty_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

void monocs_console_close(void)
{
    if (pty_fd >= 0) {
        close(pty_fd);
        kill(shell_pid, SIGHUP);
        waitpid(shell_pid, nullptr, WNOHANG);
    }
    pty_fd = -1;
}

int monocs_console_read(void *buf, int n)
{
    if (pty_fd < 0)
        return 0;
    ssize_t r = read(pty_fd, buf, (size_t)n);
    return r > 0 ? (int)r : 0;
}

void monocs_console_input(const void *p, int n)
{
    if (pty_fd >= 0 && write(pty_fd, p, (size_t)n) < 0)
        perror("pty write");
}

// シェルで exit が打たれた: ここでは、擬似端末の上のシェルが終わったら
bool monocs_console_hangup(void)
{
    return pty_fd >= 0 && waitpid(shell_pid, nullptr, WNOHANG) == shell_pid;
}

void monocs_console_set_size(int rows, int cols)
{
    struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
    if (pty_fd >= 0)
        ioctl(pty_fd, TIOCSWINSZ, &ws);
}

// exec の行: /bin/sh -c（標準入力・標準出力と標準エラーをパイプで）
static pid_t exec_pid = -1;
static int exec_in = -1, exec_out = -1, exec_status = -1;

int monocs_rexec_start(const char *cmd)
{
    if (exec_pid >= 0)
        return -1;
    int pin[2], pout[2];
    if (pipe(pin) != 0 || pipe(pout) != 0)
        return -1;
    exec_pid = fork();
    if (exec_pid == 0) {
        dup2(pin[0], 0);
        dup2(pout[1], 1);
        dup2(pout[1], 2);
        close(pin[1]);
        close(pout[0]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
        _exit(127);
    }
    close(pin[0]);
    close(pout[1]);
    exec_in = pin[1];
    exec_out = pout[0];
    fcntl(exec_in, F_SETFL, O_NONBLOCK);
    fcntl(exec_out, F_SETFL, O_NONBLOCK);
    exec_status = -1;
    return 0;
}

int monocs_rexec_read(void *buf, int n)
{
    if (exec_out < 0)
        return 0;
    ssize_t r = read(exec_out, buf, (size_t)n);
    return r > 0 ? (int)r : 0;
}

int monocs_rexec_write(const void *p, int n)
{
    if (exec_in < 0)
        return n;   // 終わりを知らせた後・行が終わった後は捨てる
    ssize_t r = write(exec_in, p, (size_t)n);
    return r > 0 ? (int)r : 0;
}

void monocs_rexec_write_eof(void)
{
    if (exec_in >= 0)
        close(exec_in);
    exec_in = -1;
}

int monocs_rexec_status(void)
{
    if (exec_pid < 0)
        return -1;
    if (exec_status < 0) {
        int st;
        if (waitpid(exec_pid, &st, WNOHANG) != exec_pid)
            return -1;
        exec_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    // 出力のパイプがまだ開いていれば、残りがあるかもしれない（Monocs と同じく、終わった後に読む）
    return exec_status;
}

void monocs_rexec_release(void)
{
    if (exec_pid < 0)
        return;
    if (exec_status < 0) {
        kill(exec_pid, SIGKILL);
        waitpid(exec_pid, nullptr, 0);
    }
    if (exec_in >= 0)
        close(exec_in);
    if (exec_out >= 0)
        close(exec_out);
    exec_in = exec_out = -1;
    exec_pid = -1;
}

// Linux の代わりの行はすぐ始まる（待たせない）。名前の要るときのための monocs_sysstat は空
bool monocs_rexec_queued(void)
{
    return false;
}

void monocs_console_interrupt(void)
{
}

typedef struct MonocsSysStat MonocsSysStat;
void monocs_sysstat(MonocsSysStat *st)
{
    (void)st;   // 呼ばれない（monocs_rexec_queued が false）
}

// コア 0 のいる番地（待たせているときの知らせ。Linux では 0）
void monocs_core0_where(uint64_t *pc, uint64_t *lr)
{
    *pc = *lr = 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        listen_port = atoi(argv[1]);
    signal(SIGPIPE, SIG_IGN);
    if (service_start(0, nullptr) != 0)
        return 1;
    printf("listening on 127.0.0.1:%d\n", listen_port);
    fflush(stdout);
    for (;;)
        if (!service_poll())
            usleep(1000);
}
