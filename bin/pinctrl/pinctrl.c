// pinctrl: GPIO の状態を見る・変える。Raspberry Pi OS の pinctrl と同じ書き方（BCM2837 でできる分だけ）
//   pinctrl [-p] [-e] get [GPIO]       状態（機能・出力・プル・値）。GPIO を省くと 0〜53 全部
//   pinctrl [-p] [-e] set GPIO OPT...  ip op a0〜a5 pu pd pn dh dl
//   pinctrl [-p] lev [GPIO]            値（1・0）
//   pinctrl [-p] funcs [GPIO]          選べる機能（CSV）
//   pinctrl [-p] poll GPIO             値が変わるたびに 1 行（Ctrl-C で終わる）
// GPIO は番号・名前・範囲を , でつないだもの（4、18-21、ID_SDA,9-11）。-p はヘッダーのピンの番号で書く・出す。
// 変えられるのは /lib/gpio が許すピン（GPIO0〜27、14・15 を除く）だけ。見るのは全部できる。
// Linux と違うところ: プルは読み出せない（BCM2837 にその仕組みがない）ので -- と出す。no（機能なし）はない。
// poll の出力の形は Monocs のもの。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_gpio.h>

#define NPINS 54

// 名前: Zero 2 W のデバイスツリー（bcm2710-rpi-zero-2-w.dtb の /soc/gpio@7e200000 の gpio-line-names）
static const char *const line_name[NPINS] = {
    "ID_SDA", "ID_SCL", "GPIO2", "GPIO3", "GPIO4", "GPIO5", "GPIO6", "GPIO7", "GPIO8", "GPIO9",
    "GPIO10", "GPIO11", "GPIO12", "GPIO13", "GPIO14", "GPIO15", "GPIO16", "GPIO17", "GPIO18", "GPIO19",
    "GPIO20", "GPIO21", "GPIO22", "GPIO23", "GPIO24", "GPIO25", "GPIO26", "GPIO27", "HDMI_HPD_N",
    "STATUS_LED_N", "CTS0", "RTS0", "TXD0", "RXD0", "SD1_CLK", "SD1_CMD", "SD1_DATA0", "SD1_DATA1",
    "SD1_DATA2", "SD1_DATA3", "CAM_GPIO1", "WL_ON", "BT_ON", "WIFI_CLK", "SDA0", "SCL0", "SMPS_SCL",
    "SMPS_SDA", "SD_CLK_R", "SD_CMD_R", "SD_DATA0_R", "SD_DATA1_R", "SD_DATA2_R", "SD_DATA3_R",
};

// 機能の名前 a0〜a5（GPIO0〜27。Raspberry Pi OS の `pinctrl -c bcm2835 funcs 0-27`。BCM2835 ARM Peripherals 6.2 の表）
static const char *const alt_name[28][6] = {
    {"SDA0", "SA5", "PCLK", "AVEOUT_VCLK", "AVEIN_VCLK", "-"},
    {"SCL0", "SA4", "DE", "AVEOUT_DSYNC", "AVEIN_DSYNC", "-"},
    {"SDA1", "SA3", "LCD_VSYNC", "AVEOUT_VSYNC", "AVEIN_VSYNC", "-"},
    {"SCL1", "SA2", "LCD_HSYNC", "AVEOUT_HSYNC", "AVEIN_HSYNC", "-"},
    {"GPCLK0", "SA1", "DPI_D0", "AVEOUT_VID0", "AVEIN_VID0", "ARM_TDI"},
    {"GPCLK1", "SA0", "DPI_D1", "AVEOUT_VID1", "AVEIN_VID1", "ARM_TDO"},
    {"GPCLK2", "SOE_N_SE", "DPI_D2", "AVEOUT_VID2", "AVEIN_VID2", "ARM_RTCK"},
    {"SPI0_CE1_N", "SWE_N_SRW_N", "DPI_D3", "AVEOUT_VID3", "AVEIN_VID3", "-"},
    {"SPI0_CE0_N", "SD0", "DPI_D4", "AVEOUT_VID4", "AVEIN_VID4", "-"},
    {"SPI0_MISO", "SD1", "DPI_D5", "AVEOUT_VID5", "AVEIN_VID5", "-"},
    {"SPI0_MOSI", "SD2", "DPI_D6", "AVEOUT_VID6", "AVEIN_VID6", "-"},
    {"SPI0_SCLK", "SD3", "DPI_D7", "AVEOUT_VID7", "AVEIN_VID7", "-"},
    {"PWM0", "SD4", "DPI_D8", "AVEOUT_VID8", "AVEIN_VID8", "ARM_TMS"},
    {"PWM1", "SD5", "DPI_D9", "AVEOUT_VID9", "AVEIN_VID9", "ARM_TCK"},
    {"TXD0", "SD6", "DPI_D10", "AVEOUT_VID10", "AVEIN_VID10", "TXD1"},
    {"RXD0", "SD7", "DPI_D11", "AVEOUT_VID11", "AVEIN_VID11", "RXD1"},
    {"FL0", "SD8", "DPI_D12", "CTS0", "SPI1_CE2_N", "CTS1"},
    {"FL1", "SD9", "DPI_D13", "RTS0", "SPI1_CE1_N", "RTS1"},
    {"PCM_CLK", "SD10", "DPI_D14", "I2CSL_SDA_MOSI", "SPI1_CE0_N", "PWM0"},
    {"PCM_FS", "SD11", "DPI_D15", "I2CSL_SCL_SCLK", "SPI1_MISO", "PWM1"},
    {"PCM_DIN", "SD12", "DPI_D16", "I2CSL_MISO", "SPI1_MOSI", "GPCLK0"},
    {"PCM_DOUT", "SD13", "DPI_D17", "I2CSL_CE_N", "SPI1_SCLK", "GPCLK1"},
    {"SD0_CLK", "SD14", "DPI_D18", "SD1_CLK", "ARM_TRST", "-"},
    {"SD0_CMD", "SD15", "DPI_D19", "SD1_CMD", "ARM_RTCK", "-"},
    {"SD0_DAT0", "SD16", "DPI_D20", "SD1_DAT0", "ARM_TDO", "-"},
    {"SD0_DAT1", "SD17", "DPI_D21", "SD1_DAT1", "ARM_TCK", "-"},
    {"SD0_DAT2", "TE0", "DPI_D22", "SD1_DAT2", "ARM_TDI", "-"},
    {"SD0_DAT3", "TE1", "DPI_D23", "SD1_DAT3", "ARM_TMS", "-"},
};

// 40 ピンのヘッダーのピン → GPIO（-1 は電源・GND）
static const int header_gpio[41] = {
    -1, -1, -1, 2, -1, 3, -1, 4, 14, -1, 15, 17, 18, 27, -1, 22, 23, -1, 24, 10, -1,
    9, 25, 11, 8, -1, 7, 0, 1, 5, -1, 6, 12, 13, -1, 19, 16, 26, 20, -1, 21,
};

static bool opt_pin, opt_echo;

static int gpio_to_header(int g)
{
    for (int p = 1; p <= 40; p++)
        if (header_gpio[p] == g)
            return p;
    return -1;
}

// 機能の 3 ビットの値 → a0〜a5 の番号（入力・出力は -1）
static int alt_index(int fsel)
{
    static const int idx[8] = {-1, -1, 5, 4, 0, 1, 2, 3};
    return idx[fsel & 7];
}

static const char *func_name(int g, int fsel)
{
    if (fsel == GPIO_IN)
        return "input";
    if (fsel == GPIO_OUT)
        return "output";
    return g < 28 ? alt_name[g][alt_index(fsel)] : "alt";
}

static void print_state(int g)
{
    int fsel = gpio_get_mode(g), lev = gpio_read(g);
    char fs[4];
    if (fsel == GPIO_IN)
        strcpy(fs, "ip");
    else if (fsel == GPIO_OUT)
        strcpy(fs, "op");
    else
        snprintf(fs, sizeof fs, "a%d", alt_index(fsel));
    const char *drive = fsel == GPIO_OUT ? (lev ? "dh" : "dl") : "  ";
    printf("%2d: %s %s -- | %s // %s = %s\n", opt_pin ? gpio_to_header(g) : g, fs, drive, lev ? "hi" : "lo",
           line_name[g], func_name(g, fsel));
}

// GPIO の並びを読む。sel[g] に印を付ける。誤りなら 1
static int parse_list(const char *s, bool sel[NPINS])
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        int lo = -1, hi = -1;
        char *end;
        long a = strtol(tok, &end, 10);
        if (end != tok && (*end == 0 || *end == '-')) {
            lo = hi = (int)a;
            if (*end == '-') {
                char *e2;
                long b = strtol(end + 1, &e2, 10);
                if (e2 == end + 1 || *e2) {
                    lo = -1;
                } else {
                    hi = (int)b;
                }
            }
        } else {
            for (int g = 0; g < NPINS; g++)
                if (strcasecmp(tok, line_name[g]) == 0)
                    lo = hi = opt_pin ? gpio_to_header(g) : g;
        }
        if (lo < 0 || hi < lo) {
            fprintf(stderr, "Unknown GPIO \"%s\"\n", tok);
            return 1;
        }
        for (int n = lo; n <= hi; n++) {
            int g = n;
            if (opt_pin)
                g = n <= 40 ? header_gpio[n] : -1;
            if (g < 0 || g >= NPINS) {
                if (opt_pin && n <= 40)
                    continue;   // 電源・GND のピンは飛ばす
                fprintf(stderr, "Unknown GPIO \"%s\"\n", tok);
                return 1;
            }
            sel[g] = true;
        }
    }
    return 0;
}

static int select_pins(int argc, char **argv, int i, bool sel[NPINS])
{
    if (i >= argc) {
        if (opt_pin) {
            for (int p = 1; p <= 40; p++)
                if (header_gpio[p] >= 0)
                    sel[header_gpio[p]] = true;
        } else {
            for (int g = 0; g < NPINS; g++)
                sel[g] = true;
        }
        return 0;
    }
    return parse_list(argv[i], sel);
}

static void each(bool sel[NPINS], void (*fn)(int))
{
    if (opt_pin) {
        for (int p = 1; p <= 40; p++)
            if (header_gpio[p] >= 0 && sel[header_gpio[p]])
                fn(header_gpio[p]);
    } else {
        for (int g = 0; g < NPINS; g++)
            if (sel[g])
                fn(g);
    }
}

static void print_funcs(int g)
{
    printf("%d, %s", opt_pin ? gpio_to_header(g) : g, line_name[g]);
    for (int a = 0; a < 6; a++)
        printf(", %s", g < 28 ? alt_name[g][a] : "-");
    printf("\n");
}

static int cmd_set(int argc, char **argv, int i)
{
    bool sel[NPINS] = {};
    if (i >= argc) {
        fprintf(stderr, "pinctrl: set needs a GPIO\n");
        return 1;
    }
    if (parse_list(argv[i++], sel))
        return 1;
    int mode = -1, pull = -1, drive = -1;
    for (; i < argc; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "ip"))
            mode = GPIO_IN;
        else if (!strcmp(o, "op"))
            mode = GPIO_OUT;
        else if (o[0] == 'a' && o[1] >= '0' && o[1] <= '5' && !o[2])
            mode = (int[]){GPIO_ALT0, GPIO_ALT1, GPIO_ALT2, GPIO_ALT3, GPIO_ALT4, GPIO_ALT5}[o[1] - '0'];
        else if (!strcmp(o, "pu"))
            pull = GPIO_PULL_UP;
        else if (!strcmp(o, "pd"))
            pull = GPIO_PULL_DOWN;
        else if (!strcmp(o, "pn"))
            pull = GPIO_PULL_OFF;
        else if (!strcmp(o, "dh"))
            drive = 1;
        else if (!strcmp(o, "dl"))
            drive = 0;
        else {
            fprintf(stderr, "Unknown option \"%s\"\n", o);
            return 1;
        }
    }
    for (int g = 0; g < NPINS; g++)
        if (sel[g] && !gpio_pin_ok(g)) {
            fprintf(stderr, "pinctrl: GPIO%d (%s) is used by Monocs\n", g, line_name[g]);
            return 1;
        }
    for (int g = 0; g < NPINS; g++) {
        if (!sel[g])
            continue;
        // 出力にするときは値を先に決める（一瞬だけ前の値が出ないように）
        if (drive >= 0)
            gpio_write(g, drive);
        if (mode >= 0)
            gpio_mode(g, mode);
        if (pull >= 0)
            gpio_pull(g, pull);
    }
    if (opt_echo)
        each(sel, print_state);
    return 0;
}

static int cmd_poll(int argc, char **argv, int i)
{
    bool sel[NPINS] = {};
    int g = -1;
    if (i >= argc || parse_list(argv[i], sel))
        return 1;
    for (int n = 0; n < NPINS; n++)
        if (sel[n]) {
            if (g >= 0) {
                fprintf(stderr, "pinctrl: poll takes one GPIO\n");
                return 1;
            }
            g = n;
        }
    if (!gpio_pin_ok(g)) {
        fprintf(stderr, "pinctrl: GPIO%d (%s) is used by Monocs\n", g, line_name[g]);
        return 1;
    }
    int n = opt_pin ? gpio_to_header(g) : g;
    printf("%2d: %s\n", n, gpio_read(g) ? "hi" : "lo");
    fflush(stdout);
    for (;;) {
        int e = gpio_wait_edge(g, GPIO_EDGE_BOTH, -1);
        if (e < 0)
            return 1;
        printf("%2d: %s\n", n, e == GPIO_EDGE_RISING ? "hi" : "lo");
        fflush(stdout);
    }
}

static void usage(FILE *f)
{
    fprintf(f, "Use:\n"
               "  pinctrl [-p] [-e] get [GPIO]\n"
               "  pinctrl [-p] [-e] set <GPIO> [options]\n"
               "  pinctrl [-p] lev [GPIO]\n"
               "  pinctrl [-p] funcs [GPIO]\n"
               "  pinctrl [-p] poll <GPIO>\n"
               "GPIO is a comma-separated list of GPIO names, numbers or ranges, e.g. 4 or 18-21 or ID_SDA,9-11\n"
               "-p: use pin numbers on the 40-way header. -e: print the new state after set.\n"
               "set options: ip op a0-a5 pu pd pn dh dl\n"
               "Only GPIO0-27 (except 14, 15: the console) can be changed. Pulls cannot be read back (shown as --).\n");
}

static int print_lev_first;

static void print_lev(int g)
{
    printf("%s%d", print_lev_first++ ? " " : "", gpio_read(g));
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'p')
                opt_pin = true;
            else if (*o == 'e')
                opt_echo = true;
            else if (*o == 'v')
                ;
            else if (*o == 'h') {
                usage(stdout);
                return 0;
            } else {
                fprintf(stderr, "pinctrl: unknown option -%c\n", *o);
                usage(stderr);
                return 1;
            }
        }
    }
    const char *cmd = i < argc ? argv[i++] : "get";
    bool sel[NPINS] = {};
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(cmd, "set"))
        return cmd_set(argc, argv, i);
    if (!strcmp(cmd, "poll"))
        return cmd_poll(argc, argv, i);
    if (strcmp(cmd, "get") && strcmp(cmd, "lev") && strcmp(cmd, "funcs")) {
        // Linux と同じく、命令を省いて GPIO だけなら get
        i--;
        cmd = "get";
    }
    if (select_pins(argc, argv, i, sel))
        return 1;
    if (!strcmp(cmd, "get")) {
        each(sel, print_state);
    } else if (!strcmp(cmd, "lev")) {
        each(sel, print_lev);
        printf("\n");
    } else {
        each(sel, print_funcs);
    }
    return 0;
}
