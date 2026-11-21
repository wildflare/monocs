# tools/test — 確かめるための道具

リポジトリの一番上で使う。

## まとめて確かめる

| コマンド | 内容 | 時間の目安 |
|---|---|---|
| `sh tools/test/verify.sh` | QEMU だけで検証一式（cc-test、picolibc、ld-test、セルフホストの一致、2 世代目、libctest、ベンチマーク） | 約 20 分 |
| `sh tools/test/verify.sh quick` | 上からライブラリのビルドと libctest・ベンチマークを除いたもの（カーネルだけを変えたとき） | 約 5 分 |
| `sh tools/test/hwcheck.sh [KERNEL...]` | 実機に送って確かめる（シェル、telnet、HTTP、USB イーサネット、アプリの TCP、4 コア、セルフホストのビルド、mi、USB シリアルの読み手が途中で止まった後に戻るか（`device/flood.c`）、Linux との互換（下の `compat_dev.sh`）） | 1 つ約 4 分 |
| `sh tools/test/compat_dev.sh [WEB_HOST]` | 実機で Linux との互換をまとめて: linuxapi（と入れ子）を Linux の出力と比べる、netapi.sh、sockets/run.py、SIGWINCH（telnet で大きさを変える。`device/winch.c`）。hwcheck がカーネルごとに呼ぶ | 約 2 分 |

実機にカーネルを送る `nos.py kernel` と `hwcheck.sh` は、Claude Code の許可のルールを `.claude/settings.local.json`（コミットしない）に足してある（2026-11-14、ユーザーの許可）。
| `sh tools/apps/push.sh [-c] DIR [NAME]` | Pi 5 のプログラムのフォルダ（Linux 用のソースと Makefile のまま）を実機の /root/apps/名前 に送る（名前は Makefile のプログラムの名前） | 数秒 |
| `sh tools/apps/mi.sh` | 端末のエディタ mi（~/dev/tools/mini-editor）を実機の /root/apps/mi に送る（push.sh を呼ぶ） | 数秒 |

部分ごとのテスト（Linux だけで動く。変えたときに流す）:

| コマンド | 内容 |
|---|---|
| `sh tools/ssh/test.sh` | SSH のサーバー（`services/sshd`）を Linux 用にコンパイルし、本物の ssh でつなぐ（認証、鍵の交換し直し、大きな出力、壊れた入力、exec（`ssh host 'コマンド'`: 出力と終了の値、3MB を標準入力から標準出力へ、途中で切った後）） |
| `sh tools/inflate/test.sh` | 展開のライブラリ（`lib/inflate`）を gzip・zlib の出力と比べる。壊した入力でも落ちないこと |
| `sh tools/inflate/tartest.sh` | `tar`・`gunzip`・`zcat` を GNU の tar・gzip と比べる |
| `sh tools/tls/files.sh` | `/usr/lib/bearssl` に入れるファイルの一覧を作り直す（`KEEP_CURL=パス` で、Linux で動く curl を残せる） |
| `sh tools/make/test.sh` | Makefile の解釈器（`os/slimcc/makefile.c`）を GNU make と比べる（式の展開、プログラムの決め方、壊した Makefile） |
| `sh tools/libc/test_glob.sh` | `glob`（`os/libc/glob.c`）を glibc と 404 通りで比べる（検査付きでも） |
| `sh tools/libc/test_wctype.sh` | ワイド文字の分類・大文字小文字（`os/libc/wctype.c`）を、すべての文字で glibc と比べる |
| `gcc -std=c23 -O2 -o /tmp/tm tools/picolibc/test_malloc.c && /tmp/tm` | malloc（入れ子のアプリのヒープを含む） |

Linux のプログラムとの互換（同じソースを Linux と Monocs で動かして、出力を比べる）:

| コマンド | 内容 | 動かす場所 |
|---|---|---|
| `sh tools/test/linuxapi.sh` | `linuxapi.c`: getopt・環境変数・文字列・書式・正規表現・glob・ワイド文字・ロケールと `%ls`・シグナル・popen。`linuxapi_nest.c`: system() の子の変更が親に残らない | QEMU（先に `make sdimage`） |
| `sh tools/test/shell.sh` | `shell/lines.sh` の 132 行（case・関数・local・return・shift・set --・`( )`・`{ }`・`{a,b}`・`~`・`${V/a/b}`・`${V:0:5}`・ヒアドキュメント・`sh script.sh`（タブで字下げした関数・`<<-` などのスクリプトのファイル）も。変数の展開・`$?`・値の分け直し・代入・シェルの変数と export・`$(...)`・`${…}` の形・`$((式))`・if・for・while・until・break・continue・test・`[`・複数の行・ファイル名の展開・注釈）を bash と比べる（1 行ずつ打つので、続きの行を読むところも試す）。timeout（終わらないアプリ、system() の中、外の timeout が入れ子をまとめて止める）、例外・Ctrl-C の `$?`（139・130）、timeout のシグナル（TERM を関数で受ける・無視して -k・-s KILL・--preserve-status）、組み込みのコマンドだけの while を Ctrl-C で止める、dmesg（起動の表示・サービス・例外・時刻の形） | QEMU（先に `make sdimage`） |
| `sh tools/test/openmp.sh` | `openmp/omptest.c`（parallel・for・reduction・schedule・private・firstprivate・critical・atomic・single・master・barrier・nowait・num_threads・if・入れ子・omp_lock）を GCC の -fopenmp と比べる。Linux の slimcc（+ os/libc/omp.c）でスレッド 1・2・3・既定、QEMU の Monocs | Linux と QEMU（先に `make sdimage`） |
| `sh tools/test/gpio.sh` | `gpio/gpiotest.c`（/usr/lib/gpio の機能・出力・値・まとめての読み書き・使えないピンを断る）と `pinctrl` の get・set・lev・funcs・-p。プルとエッジは実機で `nos.py push tools/test/gpio/gpiotest.c /tmp/gpiotest.c; nos.py cmd 'run -lgpio /tmp/gpiotest.c hw'`（GPIO5・6 を使う。何もつないでいないこと） | QEMU（先に `make sdimage`） |
| `sh tools/test/i2c.sh` | `i2c/i2ctest.c`（/usr/lib/i2c）と i2cdetect・i2cget・i2cset を、QEMU のバス 1 に EEPROM（at24c、0x50）と温度センサー（tmp105、0x48）をつないで試す（`QEMU_EXTRA` で qterm.py に -device を渡す）。QEMU はリピーテッドスタートを真似ない（止めて始め直す）ので、`i2c_write_read` の本当の確認は実機で。実機の何もつないでいないバス 1 は `run -li2c -lgpio /tmp/i2ctest.c empty`（応答がないこと・SCL の速さを GPIO3 の値で測る） | QEMU（先に `make sdimage`） |
| `sh tools/test/spi.sh` | `spi/spitest.c`（/usr/lib/spi）と `spi` コマンドを、QEMU の SPI0 に SPI フラッシュ（w25q32）をつないで試す（JEDEC ID。QEMU は CS を動かさないので、フラッシュに通じるのは起動ごとに最初の命令だけ。起動を 2 回に分ける）。実機は `run -lspi -lgpio /tmp/spitest.c hw`（何もつながない。コア 1 が SCLK・MOSI・CE0 の値を読み続け、モード 0〜3・CS の極性・速さを波形で確かめる） | QEMU（先に `make sdimage`） |
| `sh tools/test/softuart.sh` | `softuart/sutest.c`（/usr/lib/softuart）と `softuart` コマンドの使い方（引数の誤り、ブレーク、時間切れ、空いたコアがないとき）。QEMU は 2 つのコアの時間が合わないので、受けたものの正しさは見ない。実機は `run -lsoftuart -lgpio /tmp/sutest.c hw`（線なし。TX と RX を同じ GPIO5 にして、9600・38400・115200bps で 256 種類の値を送って受け、送る時間も確かめる） | QEMU（先に `make sdimage`） |
| `sh tools/test/pwm.sh` | `pwm/pwmtest.c`（/usr/lib/pwm）と `pwm` コマンドの引数・ピン・クロックマネージャーの設定（QEMU は PWM そのものを真似ていない。CPRMAN は真似ている）。実機は `run -lpwm -lgpio /tmp/pwmtest.c hw`（何もつながない。割り込みを止めてピンの値を読み続け、周波数・High の割合・サーボのパルス・極性・同じチャンネルの 2 本・duty 0 と 1 を測る） | QEMU（先に `make sdimage`） |
| `sh tools/test/pthread.sh` | `pthread/pthtest.c`（戻り値・mutex・条件変数・once・key・バリア・rwlock・時間切れ・detach など）を Linux（gcc -pthread）と比べる。スレッドの exit(5)・例外 139・空いたコアより多く作ると EAGAIN | QEMU（先に `make sdimage`） |
| `sh tools/test/netapi.sh` | `netapi.c`: ソケット・getaddrinfo・inet_*・select・poll・alarm のうち、相手のいらないもの | 実機（QEMU は USB の機器がなくネットワークが動かない） |
| `python3 tools/test/sockets/run.py [WEB_HOST]` | Pi 5 を相手に: TCP の 1MB のエコー、待たない connect、断られる、UDP と時間切れ、DNS と HTTP、select のサーバーに 3 つ同時 | 実機と Pi 5 |

`verify.sh` の結果で、`IDENTICAL` がすべて出て、`cc-test exit=0` と `すべて成功` なら通過。
`cc-test` が `test/macro.c の .o が一致しない` で失敗することがある（`__TIME__` を使うので、2 回のコンパイルの
間に秒が変わると違う）。もう一度流して通れば問題ない。
コード生成を変えたときは `picolibc: CHANGED` になってよい（`libctest: IDENTICAL` で確かめる）。

## 端末の代わり（画面を再現する）

curses のアプリやシェルの行の編集のように、エスケープシーケンスで画面を書き直すものを確かめる。
出力を `vt.py`（小さな端末の再現: カーソルの移動、消去、折り返しの保留、全角、代わりの画面）に通して、
画面の文字を表示する。端末の大きさの問い合わせ（ESC [ 6 n）には、指定した大きさで答える。

| スクリプト | つなぎ方 |
|---|---|
| `qterm.py KERNEL IMAGE ROWS COLS STEP...` | QEMU を起動する |
| `hwterm.py ROWS COLS STEP...` | 実機の USB シリアル |
| `tnterm.py ROWS COLS STEP...` | 実機の telnet（`naws:行:桁` で大きさの変更も送れる。つないだ直後はプロンプトが出ないので、最初に `keys:\r`） |

`until` は、最後に `keys` を送り始めたところから探す（速い応答は送った直後に届くので。2026-11-14 に tnterm.py も qterm.py と同じにした）。
STEP: `wait:秒`、`keys:文字列`（`\x1b[A` などのエスケープ可）、`until:文字列:秒`（出るまで待ち、時間を表示）、
`screen:名前`（画面を表示）、`rawtail:バイト数`（送られたバイト列の最後）。

例（QEMU で mi を起動して画面を見る。/root/apps/mi を入れたイメージが要る）:

```sh
python3 tools/test/qterm.py build/kslim/kernel8.img build/sdcard.img 24 80 'until:/> :20' \
    'keys:mi /usr/share/samples/hello.c\r' 'until:ESC=Menu:120' 'screen:mi' 'keys:\x11'
```

注意: `vt.py` は確かめるための小さなものなので、本物の端末と違うことがある。表示がおかしいときは、
まず `rawtail` で送られたバイト列を見る（今までに 2 回、おかしかったのはスクリプトの方だった）。

## そのほか

| スクリプト | 内容 |
|---|---|
| `qdrive.py KERNEL IMAGE [TIMEOUT] -- CMD...` | QEMU にコマンドを順に送り、プロンプトを待つ（verify.sh が使う） |
| `telnet_check.py` | telnet の挨拶・コマンド・NAWS・Ctrl-C・200KB の表示（実機の /tmp に device/*.c が要る） |
| `echotest.py [MB]` | カーネルの TCP エコー（ポート 7）で速さと一致 |
| `appecho.py` | アプリの TCP（samples/tcp_echo.c）を作って動かし、 2MB を送り、Ctrl-C で止める |
| `device/*.c` | 実機で動かす小さなプログラム（端末の大きさ、無限ループ、200KB の表示） |
