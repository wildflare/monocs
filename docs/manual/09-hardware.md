# 9. ハードウェア（GPIO・I2C・SPI・UART・PWM）

40 ピンのヘッダーにつないだ部品は、コマンドで試し、ライブラリを使ったプログラムで動かせます。
どれもカーネルには入っておらず、`/usr/lib` のライブラリ（C のソース）がレジスタを直接読み書きします。

## 9.1 ピン配置

```
          3.3V  [ 1] [ 2]  5V
 SDA1  GPIO2    [ 3] [ 4]  5V
 SCL1  GPIO3    [ 5] [ 6]  GND
       GPIO4    [ 7] [ 8]  GPIO14  TXD（コンソール）
          GND   [ 9] [10]  GPIO15  RXD（コンソール）
       GPIO17   [11] [12]  GPIO18  PWM0
       GPIO27   [13] [14]  GND
       GPIO22   [15] [16]  GPIO23
          3.3V  [17] [18]  GPIO24
 MOSI  GPIO10   [19] [20]  GND
 MISO  GPIO9    [21] [22]  GPIO25
 SCLK  GPIO11   [23] [24]  GPIO8   CE0
          GND   [25] [26]  GPIO7   CE1
 SDA0  GPIO0    [27] [28]  GPIO1   SCL0
       GPIO5    [29] [30]  GND
       GPIO6    [31] [32]  GPIO12  PWM0
 PWM1  GPIO13   [33] [34]  GND
 PWM1  GPIO19   [35] [36]  GPIO16
       GPIO26   [37] [38]  GPIO20
          GND   [39] [40]  GPIO21
```

- 信号は **3.3V** です。5V の信号を直接つながないでください。1 本あたりの電流は 16mA までにします（LED には 330Ω ほどの抵抗）。
- **GPIO14・15 はコンソールの UART** なので、ライブラリからは変えられません。
- 今の様子は `gpio readall`（表）か `pinctrl get` で見られます。

## 9.2 GPIO

コマンド:

```sh
gpio readall                  # 全部のピンの表（BCM・wPi の番号、名前、機能、値）
gpio -g mode 17 out           # -g は BCM の番号（既定は WiringPi の番号、-1 はヘッダーのピンの番号）
gpio -g write 17 1
gpio -g read 27
gpio -g mode 27 up            # プルアップ（down・tri も）
pinctrl set 17 op dh          # Raspberry Pi OS の pinctrl と同じ書き方
pinctrl poll 27               # 値が変わるたびに 1 行
watch -n 1 gpio readall       # 1 秒ごとに表示し直す
```

ライブラリ（`-lgpio`、`#include <monocs_gpio.h>`）:

| 関数 | 意味 |
|---|---|
| `gpio_mode(pin, GPIO_IN / GPIO_OUT / GPIO_ALT0〜5)` | 機能 |
| `gpio_pull(pin, GPIO_PULL_OFF / DOWN / UP)` | プル |
| `gpio_write(pin, 0/1)`、`gpio_read(pin)`、`gpio_toggle(pin)` | 読み書き |
| `gpio_write_mask(mask, value)`、`gpio_read_all()` | まとめて |
| `gpio_wait_edge(pin, GPIO_EDGE_RISING / FALLING / BOTH, timeout_ms)` | 変化を待つ（1: 来た、0: 時間切れ） |
| `gpio_pin_ok(pin)` | 変えてよいピンか |

例 `/usr/share/samples/gpio_blink.c`（GPIO17 の LED を点滅、GPIO27 のボタンで止める）:

```sh
cc -o blink -lgpio /usr/share/samples/gpio_blink.c && ./blink
```

## 9.3 I2C

バス 1 が GPIO2（SDA）・GPIO3（SCL）、バス 0 が GPIO0・GPIO1 です。

```sh
i2cdetect -y 1                # つながっている機器のアドレス（i2c-tools と同じ書き方・出力）
i2cget -y 1 0x76 0xd0         # レジスタを読む
i2cset -y 1 0x76 0xf4 0x27    # 書く
```

ライブラリ（`-li2c`、`#include <monocs_i2c.h>`）:

```c
i2c_open(1, 400000);                        // バス 1 を 400kHz で（0 なら 100kHz）
int id = i2c_read_reg(1, 0x76, 0xD0);       // 1 バイト読む（失敗は -1）
i2c_write_reg(1, 0x76, 0xF4, 0x27);
uint8_t buf[6];
i2c_read_regs(1, 0x76, 0xF7, buf, 6);       // まとめて読む（リピーテッドスタート）
i2c_close(1);
```

ほかに `i2c_write`・`i2c_read`・`i2c_write_read`・`i2c_write_regs`・`i2c_probe`。
実際の機器でのリピーテッドスタートは、まだ確かめていません。

## 9.4 SPI

SPI0: MOSI GPIO10、MISO GPIO9、SCLK GPIO11、CE0 GPIO8、CE1 GPIO7。

```sh
spi 0x9f -r 3                 # 0x9F を送り、3 バイト受ける（SPI フラッシュの ID など）
spi -c 1 -s 500000 -m 3 0x01 0x02     # CE1、500kHz、モード 3
```

ライブラリ（`-lspi`、`#include <monocs_spi.h>`）: `spi_open(cs, hz, mode)`、`spi_transfer(cs, tx, rx, len)`、
`spi_write`、`spi_read`、`spi_write_read`、`spi_close`。モードは `SPI_MODE_0`〜`3`（Linux の spidev と同じ値）。

## 9.5 ソフトウェアの UART

本物の UART はコンソールが使っているので、好きな GPIO で動くソフトウェアの UART を用意しています（8N1、300〜115200bps、4 つまで）。

```sh
softuart -b 9600 5 6                  # TX=GPIO5、RX=GPIO6 で端末のように（Ctrl-] で終わる）
softuart -b 9600 -x 'AT\r\n' 5 6      # 送って、0.5 秒の間に返ってきたものを出す
```

ライブラリ（`-lsoftuart`）: `softuart_open(tx, rx, baud)`、`softuart_write`・`putc`、`softuart_read`・`getc`（timeout_ms）、
`softuart_available`、`softuart_close`。受信は空いたコアを 1 つ使って裏で受けます。

## 9.6 PWM

ハードウェアの PWM が 2 チャンネルあります（チャンネル 0: GPIO12・18、チャンネル 1: GPIO13・19）。

```sh
pwm 18 1000 25                # GPIO18 に 1kHz・25%
pwm 18 servo 1500             # サーボ（50Hz、1.5ms のパルス）
pwm 18 off                    # 止める
pwm                           # 今の設定
```

ライブラリ（`-lpwm`）: `pwm_set(pin, freq_hz, duty)`（duty は 0.0〜1.0）、`pwm_set_ns`、`pwm_servo(pin, us)`、
`pwm_set_polarity`、`pwm_off`、`pwm_get`。プログラムが終わっても波は出続けます。

サーボ（SG90）は、茶を GND（6 番）、赤を 5V（2 番か 4 番）、橙を GPIO18（12 番）につないで、
`run -lpwm /usr/share/samples/servo.c` で動くことを確かめています。

## 9.7 気をつけること

- OS とアプリは同じ特権で動くので、ライブラリを使わずにレジスタを直接触ることもできます（`docs/programming-guide.md` の 14 章）。
  ただし Wi-Fi（GPIO34〜41）・SD カード（GPIO48〜53）のピンを変えると、OS が動かなくなります。
- 割り込みで受ける仕組みはまだありません（エッジは印を調べ続けて待ちます）。
- 常駐するドライバ（音・画面など）の仕組みは、最初にそれを作るときに一緒に作る予定です（`/drivers`）。
