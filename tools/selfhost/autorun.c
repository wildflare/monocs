// 実機でのセルフホストの確認用（make sdimage-self-auto が /autorun.c として入れる）
// UART がなくても確かめられるよう、起動時に /usr/src/kernel のビルドを行い、結果を ACT LED で知らせる:
//   ビルド中: 点灯したまま
//   成功: 「ピカッ、ピカッ、ピカッ、休み」の繰り返し（OS の普段の点滅と見分けるため）
//   失敗: 0.1 秒ごとの速い点滅
// できた /usr/src/kernel/build/kslim/kernel8.img は、PC でハッシュ値を Linux 上のビルドと比べる
#include <unistd.h>
#include <monocs.h>

#define main build_main
#include "/usr/src/kernel/build.c"
#undef main

int main(void)
{
    led_set(true);
    int r = chdir("/usr/src/kernel") == 0 ? build_main() : 1;
    for (;;) {
        if (r == 0) {
            for (int i = 0; i < 3; i++) {
                led_set(true);
                timer_delay_us(150000);
                led_set(false);
                timer_delay_us(150000);
            }
            timer_delay_us(1200000);
        } else {
            led_set(true);
            timer_delay_us(100000);
            led_set(false);
            timer_delay_us(100000);
        }
    }
}
