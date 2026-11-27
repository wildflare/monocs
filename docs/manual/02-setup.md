# 2. 用意と最初の起動

## 2.1 要るもの

| もの | 用途 |
|---|---|
| Raspberry Pi Zero 2 W | Monocs が動く基板 |
| microSD カード | OS とファイル。64MB のイメージから使えますが、今の実機は 32GB のカード（区画を 29GB に広げたもの） |
| Raspberry Pi 5（Raspberry Pi OS 64 ビット） | 開発用。OS のビルド、SD カードのイメージ作り、操作 |
| USB ケーブル（micro-B） | Zero 2 W の「USB」端子と Pi 5 をつなぐ（電源と通信） |
| ジャンパー線 3 本（あれば） | UART。起動の表示を見る・動かなくなったときに戻すため |
| 2.4GHz の Wi-Fi（あれば） | 線なしで使う・インターネットにつなぐ |

Pi 5 には次のものを入れておきます。

```sh
sudo apt install build-essential gdb qemu-system-arm qemu-user picocom mtools
```

## 2.2 OS を作る（Pi 5 で）

リポジトリの一番上で:

```sh
make             # 標準のカーネル（slimcc-a64 で作る）build/kslim/kernel8.img
make sdimage     # SD カードのイメージ build/sdcard.img（64MB、FAT32）
make qemu        # 実機の前に QEMU で起動してみる（終わるには Ctrl-A、X）
```

- 初めてのときは picolibc（C ライブラリ）とコンパイラのビルドも走るので、時間がかかります。
- 比べるための GCC 版のカーネルは `make kernel-gcc`（build/kernel8.img）です。ふだんは使いません。

## 2.3 SD カードを作る

どちらかのやり方で作ります。

**方法 A: イメージを書き込む**（いちばん簡単）

`build/sdcard.img` を Raspberry Pi Imager（「カスタムイメージを使う」）・Rufus・`dd` などで SD カードに書き込みます。
カードの中身はすべて消えます。イメージは 64MB なので、大きなカードでも区画は 64MB です（広げ方は [第 5 章](05-files.md) の 5.7）。

**方法 B: ファイルを写す**

```sh
make sdcard                       # build/sdcard/ に中身を集める
cp -r build/sdcard/* /media/user/BOOT/    # FAT32 でフォーマットしたカードに、全部写す
sync
```

SD カードのルートには、ファームウェア（`bootcode.bin`・`start.elf`・`fixup.dat`）と `config.txt`、フォルダが入ります。
カーネルは `/boot/kernel8.img` です（`config.txt` の `os_prefix=boot/`）。

### config.txt の中身

| 行 | 意味 |
|---|---|
| `arm_64bit=1` | 64 ビットで起動 |
| `os_prefix=boot/` | カーネルなどを /boot から読む |
| `kernel=kernel8.img` | カーネルのファイル名 |
| `enable_uart=1`、`dtoverlay=disable-bt`、`init_uart_clock=48000000` | UART（GPIO14・15）を使えるようにする |
| `force_turbo=1` | CPU をいつも 1000MHz にする（ないと起動から 60 秒後に 600MHz に落ちて、計算が約 4 割遅くなる） |
| `uart_2ndstage=1` | ファームウェアのログも UART に出す |

## 2.4 配線

### いちばん簡単: USB 1 本

Zero 2 W の **「USB」と書かれた内側の端子**を、Pi 5（か PC）の USB につなぎます。**電源もここから入る**ので、
外側の「PWR IN」には何もつなぎません。Pi 5 からは、USB シリアルと USB イーサネットの 2 つに見えます（[第 3 章](03-connect.md)）。

> 「PWR IN」から電源を入れたまま「USB」端子もホストにつなぐのは避けてください（両方の 5V がつながるはずです。確かめていません）。

### UART（あれば安心）

USB や Wi-Fi が使えないカーネルを送ってしまったときも、起動の表示を見て操作できるようにするための線です。

| Zero 2 W | | Pi 5 |
|---|---|---|
| 8 番ピン（GPIO14、TXD） | → | 10 番ピン（GPIO15、RXD） |
| 10 番ピン（GPIO15、RXD） | ← | 8 番ピン（GPIO14、TXD） |
| 6 番ピン（GND） | — | 6 番ピン（GND） |

- 5V と 3.3V のピンはつながないでください。どちらも 3.3V の信号なので、レベル変換は要りません。
- Pi 5 では `/boot/firmware/config.txt` の終わりに `dtparam=uart0=on` を足して再起動すると、`/dev/ttyAMA0` になります。
- 速さは 3Mbps です（`picocom -b 3000000 /dev/ttyAMA0`。ファームウェアのログだけは 115200bps）。

### Wi-Fi だけ

Wi-Fi の設定（[第 3 章](03-connect.md) の 3.5）が済んでいれば、電源（PWR IN か USB）だけで使えます。今の開発は主にこの形です。
ただし、起動のごく初めで止まるカーネルを送ると、SD カードを PC に差して直すしかありません（[第 11 章](11-troubleshooting.md)）。

## 2.5 Pi 5 の準備（初めてのとき）

```sh
sh tools/pi5/setup-usbnet.sh     # USB イーサネットに 192.168.7.1 を付ける（NetworkManager の接続 monocs-usb）
sh tools/pi5/setup-chrony.sh     # Monocs に時刻を配る（chrony）
```

SSH の鍵を作って、Monocs に登録しておくと便利です（[第 3 章](03-connect.md) の 3.7）。

## 2.6 起動を確かめる

電源を入れると、次のように進みます。時間は、カーネルが動き出してからのものです（`dmesg` の時刻。2026-11-26 に実機で測った）。
電源を入れてからカーネルが動き出すまでのファームウェアの起動（SD カードから `start.elf` などを読む）は、この前にかかります（測っていない）。

| 時間 | 起きること |
|---|---|
| 0 秒 | （その前に）ファームウェアが SD カードを読む（緑の LED が不規則に点滅） |
| 約 0.2 秒 | カーネルが動き出す（`Monocs: hello from Raspberry Pi Zero 2 W`） |
| 約 0.7 秒 | USB がつながる、4 コアが動く、SD カードを読む |
| 約 0.8〜1.5 秒 | サービス（HTTP・telnet・SSH・Wi-Fi・時刻合わせ）が始まる |
| 約 1.5 秒 | シェルのプロンプト（`Monocs shell. "help" でコマンドの一覧を表示します`）。USB ならここから使える |
| 約 4.3 秒 | Wi-Fi がつながる |
| 約 6.3 秒 | Wi-Fi のアドレスが付く（ここから Wi-Fi で ssh できる） |

カーネルを入れ替えた直後など、サービス（SSH の BearSSL など）のコンパイルのキャッシュがないときは、サービスを始めるのに数秒余分にかかります。

緑の LED（ACT）でも様子がわかります。

| LED | 意味 |
|---|---|
| 1 秒に 1 回の点滅 | 正常に動いている |
| 0.2 秒ごとの速い点滅 | 起動したが、SD カードを読めなかった |
| 点いたまま | 起動の途中で止まった |
| 消えたまま | カーネルが動いていない（SD カードの中身か config.txt の問題） |

起動の表示は、あとから `dmesg` で見られます。プロンプトが出たら `help` と打つと、コマンドの一覧が出ます。
