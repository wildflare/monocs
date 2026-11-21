// gpio: WiringPi の gpio コマンドと同じ書き方で、40 ピンのヘッダーの GPIO を見る・変える（2026-11-20。ユーザーの
// 「gpio readall コマンドみたいなのがあったらいいかも」から）
//   gpio readall                 全部のピンの表（BCM・wPi の番号、名前、機能、値。WiringPi の readall と同じ形）
//   gpio [-g|-1] read PIN        値（0・1）
//   gpio [-g|-1] write PIN 0|1   出力の値を決める
//   gpio [-g|-1] toggle PIN      出力を反対に
//   gpio [-g|-1] mode PIN MODE   in・out・up・down・tri（プルなし）・alt0〜alt5
// ピンの番号は WiringPi と同じく、既定は wPi の番号、-g は BCM の GPIO の番号、-1 はヘッダーのピンの番号。
// 変えられるのは /usr/lib/gpio が許すピン（GPIO0〜27、コンソールの UART の 14・15 を除く）。見るのは全部できる。
// 表の形は記憶にある WiringPi（2.5x）の readall に合わせた（Pi 5 に WiringPi がなく、本物とは比べていない）。
// プルは読み出せない（BCM2837 にその仕組みがない）。細かく見るなら pinctrl
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_gpio.h>

// ヘッダーのピン（1〜40）の BCM の番号（-1 は電源・GND）と、WiringPi の名前
static const int phys_bcm[41] = {
    -1, -1, -1, 2, -1, 3, -1, 4, 14, -1, 15, 17, 18, 27, -1, 22, 23, -1, 24, 10, -1,
    9, 25, 11, 8, -1, 7, 0, 1, 5, -1, 6, 12, 13, -1, 19, 16, 26, 20, -1, 21,
};
static const char *const phys_name[41] = {
    "",        "3.3v",    "5v",      "SDA.1",   "5v",      "SCL.1",   "0v",      "GPIO. 7", "TxD",
    "0v",      "RxD",     "GPIO. 0", "GPIO. 1", "GPIO. 2", "0v",      "GPIO. 3", "GPIO. 4", "3.3v",
    "GPIO. 5", "MOSI",    "0v",      "MISO",    "GPIO. 6", "SCLK",    "CE0",     "0v",      "CE1",
    "SDA.0",   "SCL.0",   "GPIO.21", "0v",      "GPIO.22", "GPIO.26", "GPIO.23", "0v",      "GPIO.24",
    "GPIO.27", "GPIO.25", "GPIO.28", "0v",      "GPIO.29",
};
// wPi の番号 → BCM の番号（WiringPi の 40 ピンの表。0〜31）
static const int wpi_bcm[32] = {
    17, 18, 27, 22, 23, 24, 25, 4, 2, 3, 8, 7, 10, 9, 11, 14,
    15, -1, -1, -1, -1, 5, 6, 13, 19, 26, 12, 16, 20, 21, 0, 1,
};

// 機能の名前（GPFSEL の値ごと。WiringPi と同じ）
static const char *const mode_name[8] = {"IN", "OUT", "ALT5", "ALT4", "ALT0", "ALT1", "ALT2", "ALT3"};

static int bcm_to_wpi(int bcm)
{
    for (int i = 0; i < 32; i++)
        if (wpi_bcm[i] == bcm)
            return i;
    return -1;
}

static void border(const char *mid)
{
    printf(" +-----+-----+---------+------+---+%s+---+------+---------+-----+-----+\n", mid);
}

static void header(void)
{
    printf(" | BCM | wPi |   Name  | Mode | V | Physical | V | Mode | Name    | wPi | BCM |\n");
}

// ピンの欄（左は右寄せ、右は左寄せ）
static void cells(int phys, char *bcm, char *wpi, const char **mode, char *v)
{
    int b = phys_bcm[phys];
    bcm[0] = wpi[0] = v[0] = '\0';
    *mode = "";
    if (b < 0)
        return;
    snprintf(bcm, 4, "%d", b);
    int w = bcm_to_wpi(b);
    if (w >= 0)
        snprintf(wpi, 4, "%d", w);
    int m = gpio_get_mode(b);
    *mode = m >= 0 && m < 8 ? mode_name[m] : "";
    int val = gpio_read(b);
    if (val >= 0)
        snprintf(v, 2, "%d", val);
}

static void readall(void)
{
    border("---Pi Z2W-");
    header();
    border("----++----");
    for (int p = 1; p <= 39; p += 2) {
        char lb[4], lw[4], lv[2], rb[4], rw[4], rv[2];
        const char *lm, *rm;
        cells(p, lb, lw, &lm, lv);
        cells(p + 1, rb, rw, &rm, rv);
        printf(" | %3s | %3s | %7s | %4s | %1s | %2d || %-2d | %1s | %-4s | %-7s | %-3s | %-3s |\n", lb, lw,
               phys_name[p], lm, lv, p, p + 1, rv, rm, phys_name[p + 1], rw, rb);
    }
    border("----++----");
    header();
    border("---Pi Z2W-");
}

static void usage(void)
{
    fprintf(stderr, "usage: gpio readall\n"
                    "       gpio [-g|-1] read PIN | write PIN 0|1 | toggle PIN | mode PIN in|out|up|down|tri|alt0..alt5\n"
                    "  PIN is a wiringPi number by default, a BCM GPIO number with -g, a header pin number with -1\n");
}

// 番号の決め方（numbering: 'w' wPi、'g' BCM、'1' ヘッダーのピン）で BCM の番号に。だめなら -1
static int to_bcm(const char *s, char numbering)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < 0)
        return -1;
    switch (numbering) {
    case 'g': return n <= GPIO_PIN_MAX ? (int)n : -1;
    case '1': return n >= 1 && n <= 40 ? phys_bcm[n] : -1;
    default:  return n < 32 ? wpi_bcm[n] : -1;
    }
}

int main(int argc, char **argv)
{
    char numbering = 'w';
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-g"))
            numbering = 'g';
        else if (!strcmp(argv[i], "-1"))
            numbering = '1';
        else {
            usage();
            return 1;
        }
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "readall")) {
        readall();
        return 0;
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    int pin = to_bcm(argv[i++], numbering);
    if (pin < 0) {
        fprintf(stderr, "gpio: no such pin: %s\n", argv[i - 1]);
        return 1;
    }
    if (!strcmp(cmd, "read")) {
        int v = gpio_read(pin);
        if (v < 0) {
            perror("gpio");
            return 1;
        }
        printf("%d\n", v);
        return 0;
    }
    if (!strcmp(cmd, "toggle")) {
        if (gpio_toggle(pin) < 0) {
            perror("gpio");
            return 1;
        }
        return 0;
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    const char *arg = argv[i];
    int r;
    if (!strcmp(cmd, "write")) {
        if (strcmp(arg, "0") && strcmp(arg, "1")) {
            usage();
            return 1;
        }
        r = gpio_write(pin, arg[0] == '1');
    } else if (!strcmp(cmd, "mode")) {
        static const struct { const char *name; int mode; } modes[] = {
            {"in", GPIO_IN}, {"input", GPIO_IN}, {"out", GPIO_OUT}, {"output", GPIO_OUT},
            {"alt0", GPIO_ALT0}, {"alt1", GPIO_ALT1}, {"alt2", GPIO_ALT2}, {"alt3", GPIO_ALT3},
            {"alt4", GPIO_ALT4}, {"alt5", GPIO_ALT5},
        };
        if (!strcmp(arg, "up") || !strcmp(arg, "down") || !strcmp(arg, "tri") || !strcmp(arg, "off")) {
            r = gpio_pull(pin, arg[0] == 'u' ? GPIO_PULL_UP : arg[0] == 'd' ? GPIO_PULL_DOWN : GPIO_PULL_OFF);
        } else {
            r = -2;
            for (size_t k = 0; k < sizeof modes / sizeof modes[0]; k++)
                if (!strcmp(arg, modes[k].name))
                    r = gpio_mode(pin, modes[k].mode);
            if (r == -2) {
                usage();
                return 1;
            }
        }
    } else {
        usage();
        return 1;
    }
    if (r < 0) {
        perror("gpio");
        return 1;
    }
    return 0;
}
