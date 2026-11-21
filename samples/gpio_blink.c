// GPIO17（ヘッダーの 11 番）につないだ LED を 0.5 秒ごとに点滅させる。GPIO27（13 番）のボタン
// （押すと GND につながる）を押すと止まる。Ctrl-C でも止まる。
//   cc -o gpio_blink -lgpio /samples/gpio_blink.c && ./gpio_blink [LED の GPIO [ボタンの GPIO]]
// LED は 330Ω ほどの抵抗を直列に入れて GPIO と GND の間に（3.3V。ピンあたり 16mA まで）
#include <stdio.h>
#include <stdlib.h>
#include <monocs_gpio.h>

int main(int argc, char **argv)
{
    int led = argc > 1 ? atoi(argv[1]) : 17;
    int button = argc > 2 ? atoi(argv[2]) : 27;
    if (!gpio_pin_ok(led) || !gpio_pin_ok(button) || led == button) {
        fprintf(stderr, "gpio_blink: cannot use GPIO%d / GPIO%d\n", led, button);
        return 1;
    }
    gpio_write(led, 0);
    gpio_mode(led, GPIO_OUT);
    gpio_mode(button, GPIO_IN);
    gpio_pull(button, GPIO_PULL_UP);
    printf("blinking GPIO%d; press the button on GPIO%d (or Ctrl-C) to stop\n", led, button);
    // 待つ間にボタンの立ち下がりを見る（500ms で時間切れなら点滅を進める）
    while (gpio_wait_edge(button, GPIO_EDGE_FALLING, 500) == 0)
        gpio_toggle(led);
    gpio_write(led, 0);
    gpio_mode(led, GPIO_IN);
    printf("stopped\n");
    return 0;
}
