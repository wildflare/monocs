// pwm: ハードウェアの PWM を出す・止める・見る（Monocs のコマンド。/lib/pwm）
//   pwm                       2 つのチャンネルの今の設定
//   pwm PIN FREQ DUTY%        例: pwm 18 1000 25（GPIO18 に 1kHz・25%）
//   pwm PIN servo US          例: pwm 18 servo 1500（50Hz、1.5ms のパルス）
//   pwm PIN invert on|off     極性
//   pwm PIN off               止める（ピンは休みの値の出力）
// PIN は 12・18（チャンネル 0）、13・19（チャンネル 1）。コマンドが終わっても波は出続ける。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monocs_pwm.h>

static void usage(void)
{
    fprintf(stderr, "Usage: pwm                    show both channels\n"
                    "       pwm PIN FREQ DUTY%%     e.g. pwm 18 1000 25\n"
                    "       pwm PIN servo US       e.g. pwm 18 servo 1500 (50 Hz)\n"
                    "       pwm PIN invert on|off\n"
                    "       pwm PIN off\n"
                    "  PIN: 12 or 18 (channel 0), 13 or 19 (channel 1)\n");
}

static bool num(const char *s, double *v)
{
    char *end;
    *v = strtod(s, &end);
    return end != s && !*end;
}

static void show(int ch)
{
    static const int pins[2][2] = {{12, 18}, {13, 19}};
    int on_pin = -1;
    for (int k = 0; k < 2; k++)
        if (pwm_get(pins[ch][k], nullptr, nullptr) == 1)
            on_pin = on_pin < 0 ? pins[ch][k] : on_pin;
    double f, d;
    pwm_get(pins[ch][0], &f, &d);
    if (on_pin < 0) {
        printf("PWM%d: off\n", ch);
        return;
    }
    printf("PWM%d: %.6g Hz, duty %.2f%% (%.1f us high) on GPIO", ch, f, d * 100, f > 0 ? d / f * 1e6 : 0);
    bool first = true;
    for (int k = 0; k < 2; k++)
        if (pwm_get(pins[ch][k], nullptr, nullptr) == 1) {
            printf("%s%d", first ? "" : ",", pins[ch][k]);
            first = false;
        }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        show(0);
        show(1);
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }
    int pin = atoi(argv[1]);
    if (pin != 12 && pin != 13 && pin != 18 && pin != 19) {
        fprintf(stderr, "pwm: GPIO%s has no PWM (use 12, 13, 18, 19)\n", argv[1]);
        return 1;
    }
    int r;
    double a, b;
    if (argc == 3 && !strcmp(argv[2], "off")) {
        r = pwm_off(pin);
    } else if (argc == 4 && !strcmp(argv[2], "servo") && num(argv[3], &a)) {
        r = pwm_servo(pin, (int)a);
    } else if (argc == 4 && !strcmp(argv[2], "invert") && (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
        r = pwm_set_polarity(pin, !strcmp(argv[3], "on"));
    } else if (argc == 4 && num(argv[2], &a) && num(argv[3], &b)) {
        r = pwm_set(pin, a, b / 100);
    } else {
        usage();
        return 1;
    }
    if (r < 0) {
        perror("pwm");
        return 1;
    }
    return 0;
}
