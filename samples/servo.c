// サーボモーター（SG90 など）を動かす。ハードウェアの PWM（/lib/pwm）で、50Hz のパルスを出す。
//   cc -o servo -lpwm /samples/servo.c && ./servo                  GPIO18 で、真ん中 → 端から端へゆっくり → 真ん中、で止める
//   cc -o servo -lpwm /samples/servo.c && ./servo PIN US           PIN に US マイクロ秒のパルスを出し続ける（シェルの pwm PIN servo US と同じ）
//   cc -o servo -lpwm /samples/servo.c && ./servo PIN MIN MAX      端を MIN・MAX マイクロ秒にして動かす（既定 1000・2000）
// つなぎ方（SG90）: 茶 = GND（ヘッダーの 6 番など）、赤 = 5V（2・4 番。止まると Pi が落ちるときは別の 5V から、GND を共通に）、
//   橙 = 信号（GPIO18 = 12 番。ほかに 12・13・19）。PIN は 12・13・18・19
// パルスの幅と角度: SG90 はおよそ 1000µs で一方の端、1500µs で真ん中、2000µs で反対の端（個体で違う。500〜2400µs まで
// 動くものも多いが、端に当たって唸るときは戻す）
#include <stdio.h>
#include <stdlib.h>
#include <monocs.h>
#include <monocs_pwm.h>

static void move(int pin, int from, int to, int step_us, int wait_ms)
{
    int dir = to > from ? 1 : -1;
    for (int us = from; dir > 0 ? us <= to : us >= to; us += dir * step_us) {
        pwm_servo(pin, us);
        timer_delay_us((uint64_t)wait_ms * 1000);
    }
    pwm_servo(pin, to);
}

int main(int argc, char **argv)
{
    int pin = argc > 1 ? atoi(argv[1]) : 18;
    if (argc == 3) {
        int us = atoi(argv[2]);
        if (pwm_servo(pin, us) < 0) {
            perror("servo");
            return 1;
        }
        printf("GPIO%d: %d us（出し続ける。止めるのは pwm %d off）\n", pin, us, pin);
        return 0;
    }
    int lo = argc > 3 ? atoi(argv[2]) : 1000, hi = argc > 3 ? atoi(argv[3]) : 2000;
    if (pwm_servo(pin, 1500) < 0 || lo < 400 || hi > 2600 || lo >= hi) {
        fprintf(stderr, "usage: servo [PIN [US | MIN MAX]]  (PIN 12/13/18/19, US 400-2600)\n");
        return 1;
    }
    printf("GPIO%d: center (1500 us)\n", pin);
    timer_delay_us(1000000);
    printf("-> %d us\n", lo);
    move(pin, 1500, lo, 10, 15);
    timer_delay_us(500000);
    printf("-> %d us\n", hi);
    move(pin, lo, hi, 10, 15);
    timer_delay_us(500000);
    printf("-> center\n");
    move(pin, hi, 1500, 10, 15);
    timer_delay_us(500000);
    pwm_off(pin);       // パルスを止める（サーボは力を抜く）
    printf("done（パルスを止めた）\n");
    return 0;
}
