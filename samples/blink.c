// ACT LED を「2 回光って休む」パターンで点滅させる。Ctrl-C で止める
// （OS の 1 秒周期の点滅や、SD カードのエラー時の速い点滅と見分けられる）
#include <stdio.h>
#include <monocs.h>

static void flash(int on_ms, int off_ms)
{
    led_set(true);
    timer_delay_us(on_ms * 1000);
    led_set(false);
    timer_delay_us(off_ms * 1000);
}

int main(void)
{
    printf("blinking... (Ctrl-C to stop)\n");
    for (;;) {
        flash(100, 150);
        flash(100, 650);
    }
}
