# 13. コマンド一覧

シェルの `help` でも一覧が出ます。Linux と同じ名前のものは、書き方と出力をできるだけ同じにしています。

## 13.1 シェルに組み込みのコマンド

シェル自身の状態を変えるもの、プログラムを動かす仕組み、カーネルの中を見る・変えるものは、シェルに組み込みです。

### シェル

| コマンド | 意味 |
|---|---|
| `help` | コマンドの一覧 |
| `cd [DIR]`・`pwd` | フォルダの移動・今のフォルダ |
| `export NAME=VALUE`・`unset NAME`・`env [-i] [-u N] [N=V]… [CMD]` | 環境変数 |
| `set -- ARG…`・`shift [N]`・`local`・`return [N]` | 位置の引数、関数の中 |
| `test`・`[`・`true`・`false`・`:` | 条件、何もしない |
| `break [N]`・`continue [N]` | 繰り返しを抜ける・次へ |
| `source FILE`・`. FILE`・`sh FILE`・`sh -c CMD` | スクリプト |
| `history [-c]` | 履歴 |
| `which NAME…` | どれが動くか |
| `timeout [-s SIG] [-k 秒] 秒 CMD…` | 時間を区切って動かす（期限で 124） |
| `time [-p] CMD` | かかった時間（予約語） |
| `write FILE` | 打った内容をファイルに書く |
| `exit`・`logout` | telnet・ssh の接続を閉じる |

### プログラム

| コマンド | 意味 |
|---|---|
| `run FILE[.c]\|DIR\|- [引数]` | その場でコンパイルして動かす |
| `make [-C DIR] [目標] [変数=値]` | Makefile から作り方を選んでコンパイルする（`make clean` で忘れる） |
| `cc [-o PROG] FILE.c…`・`cc -c FILE.c -o FILE.o` | 実行ファイル・.o を作る |
| `cc --report [-l名前] FILE.c…` | 作らずに、そのプログラムが使う OS の機能（ファイルを消す、ネットワーク、ほかのプログラム、SD カード・再起動などの機械の設定、ほかのコア、ハードウェアのライブラリ）と、インラインアセンブリ・周辺機器の番地の直書きを出す。ほかの人が作ったコードを動かす前に。ポインタで OS のメモリを直接いじるような手は出ないので、悪意は防げない |
| `ld -o kernel8.img FILE.o… LIB.a…` | リンカ（Monocs のカーネルを作る専用） |
| `cache [clear [NAME\|PATH]…]` | コンパイルのキャッシュ |
| `cweb URL\|FILE.c\|DIR/[#sha256=…]` | C の Web のブラウザ。ページはサンドボックスで動く C のプログラムで、`cweb_go("next.c")` で次のページへ、`cweb_back()` で戻る（`#include <cweb.h>`）。見る人は Ctrl-C で 1 つ前のページへ戻り、Ctrl-\\ でブラウザごと終わる。リンクに `#sha256=` を書くと、移った先の中身を確かめる。/ で終わるとフォルダのページ（目録 `cweb.txt` のファイルをまとめて。`#sha256=` は目録のハッシュ）（docs/cweb.md） |
| `sandbox FILE.c\|http://…/FILE.c[#sha256=…] [引数]` | サンドボックス（EL0）で動かす。http:// なら取ってきて、中身の SHA-256 を確かめる（合わなければ動かさない。前に許した版から変わったら聞く）。OS のメモリ・ハードウェア・ファイル・ネットワークには手を出せない。使えるのは画面に書く（printf など）・キーを読む（端末の設定・poll も）・画面の大きさ・時刻・眠る・乱数・文字列・malloc・exit・curses（作っている途中。docs/sandbox.md）。Ctrl-C で止まる |
| `svc [list\|start FILE\|stop NAME]` | サービス |
| `perf [-e EV,…] [-c] CMD…`・`perf -l`・`perf record …` | 性能カウンタ |

### カーネル・機械

| コマンド | 意味 |
|---|---|
| `dmesg [-c\|-C]` | カーネルの記録 |
| `mem [-r]` | メモリ、ファイルのキャッシュ、SD カードの読み書き |
| `clock` | CPU のクロック、温度、電圧 |
| `uptime` | 起動からの時間 |
| `date [-s @秒\|-s 年-月-日 時:分[:秒]] [-z +0900]` | 日時・タイムゾーン |
| `usb` | USB とネットワークの様子 |
| `wifi [start\|scan\|connect [SSID PASS]\|disconnect\|status\|log\|events\|perf [reset]\|pm [0\|1\|2]]` | 内蔵 Wi-Fi（perf: 時間の内訳、pm: 省電力） |
| `sync` | 書き込みをすぐ SD カードへ |
| `reboot [tryboot]` | 再起動（tryboot: 次の 1 回だけ tryboot.txt で） |
| `poweroff`・`halt` | 書き出して止まる |
| `tryboot [status\|confirm]` | 試しの起動 |
| `watchdog [on\|off\|status]` | ウォッチドッグ |
| `baud [BPS]` | UART の速さ |
| `recv PATH SIZE CRC32 [BLOCK]` | ファイルを受け取る（nos.py が使う） |
| `sdtest [N]`・`sdtest hs [N]` | SD カードのドライバの試験（開発用） |

## 13.2 /usr/bin のコマンド

どれも C のソースのまま置いてあり、初めて使うときにコンパイルされます。

### ファイル

| コマンド | 意味 |
|---|---|
| `ls [-a] [-l] [-h] [-1] [PATH…]` | 一覧（GNU の ls に近い） |
| `cat [FILE…]` | 中身を出す |
| `cp [-r] SRC… DST`・`mv SRC… DST`・`rm [-rf] PATH…` | 写す・動かす・消す |
| `mkdir [-p] DIR…`・`rmdir DIR…`・`touch FILE…` | フォルダを作る・消す、日時 |
| `df [-h]` | 空き |
| `fatattr [+-ahrs] FILE…` | FAT の属性 |
| `less [+g\|+G] [FILE]` | 1 画面ずつ見る（/・? で検索、q で終わる） |
| `head [-n N]`・`tail [-n N]`・`wc [-lwc]` | 頭・終わり・数える |
| `grep [-ivnclrFE] PATTERN [FILE…]` | 探す |
| `sha256sum [-c] [FILE…]` | SHA-256（GNU と同じ出力。`-c` で確かめる）。C の Web の目録を作る: `sha256sum *.c *.h > cweb.txt` |
| `tar x\|t [v] [f FILE] [-C DIR] [--strip-components=N]` | tar の展開・一覧（gzip も。作る c はない） |
| `gunzip [-c] [-k] [-f] FILE…`・`zcat FILE…` | gzip の展開 |
| `echo [-n] …`・`clear` | 出す・画面を消す |

### ネットワーク・守り

| コマンド | 意味 |
|---|---|
| `curl [オプション] URL` | HTTP・HTTPS |
| `host NAME` | DNS で引く |
| `passwd [-d] [名前]` | Wi-Fi から入るときのパスワード |

### 様子を見る

| コマンド | 意味 |
|---|---|
| `top [-b] [-n 回数] [-d 秒]` | コア・メモリ・サービス |
| `watch [-n 秒] [-d] [-t] CMD…` | 何度も動かして表示し直す（Ctrl-C で終わる） |

### ハードウェア

| コマンド | 意味 |
|---|---|
| `gpio readall`・`gpio [-g\|-1] read\|write\|toggle\|mode PIN …` | GPIO（WiringPi の gpio と同じ書き方） |
| `pinctrl get\|set\|lev\|funcs\|poll …` | GPIO（Raspberry Pi OS の pinctrl と同じ書き方） |
| `i2cdetect`・`i2cget`・`i2cset` | I2C（i2c-tools と同じ書き方） |
| `spi [-c CS] [-s HZ] [-m MODE] [-r N] BYTE…` | SPI |
| `softuart [-b BAUD] [-x TEXT] TX RX` | ソフトウェアの UART |
| `pwm [PIN FREQ DUTY%\|PIN servo US\|PIN off]` | PWM |

## 13.3 /root/apps にあるもの（今の実機）

| 名前 | 中身 |
|---|---|
| `mi` | 端末のエディタ（Ctrl-S で保存、Ctrl-Q で終わる） |
| `fd` | ファイルマネージャー |
| `lua` | Lua 5.4.9 |
| `sqlite3` | SQLite 3.54.1 |
| `vim` | vim 9.2 |
| `qjs` | QuickJS（JavaScript） |

## 13.4 Pi 5 の道具（tools/）

| 道具 | 意味 |
|---|---|
| `tools/uart/nos.py` | 実機の操作（[第 10 章](10-development.md)） |
| `tools/update_bin.sh` | /usr/bin・/usr/sbin を入れ替える |
| `tools/apps/push.sh DIR` | 自分のアプリを /root/apps に送る |
| `tools/test/verify.sh`・`hwcheck.sh` | 検証一式（QEMU・実機） |
| `tools/mkimg.sh`・`mksd.sh` | SD カードのイメージ・中身を作る |
| `tools/pi5/setup-usbnet.sh`・`setup-chrony.sh` | Pi 5 の準備 |
