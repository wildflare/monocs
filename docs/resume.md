# Monocs 開発の再開の手引き

しばらく間が空いても、ここから読めば開発を再開できるようにまとめたもの（2026-11-16 の時点。Linux との互換と、標準のカーネルを slimcc 版にした（Monocs の中で完結）までを含む）。
細かい経緯・測った数字・不具合の記録は `CLAUDE.md`、Monocs の上のプログラムの書き方は `docs/programming-guide.md`、
実機の配線と SD カードは `docs/hardware-test.md`、確かめる道具は `tools/test/README.md`。

## 1. Monocs とは

Raspberry Pi Zero 2 W で動く、自作のベアメタル OS。名前は Mono + Computing System（同時に動くプログラムは 1 つ、アドレス空間と特権も 1 つ）。
使い方と仕組みの全体は `docs/manual/`（リファレンスマニュアル。2026-11-21）。

- **その場で C を書いて、コンパイルして、メモリ上で動かす**。実行ファイルを持たず、ソースが正本（コンパイルの結果は SD カードにキャッシュするだけ）
- **セルフホスト**: Monocs の上で、自作のコンパイラ（slimcc-a64）・リンカ・C ライブラリ・カーネルの全部を作り直せる（Linux 上のビルドとバイト単位で一致）
- **Monocs の中で完結**（ユーザーの方針、2026-11-16）: 標準のカーネルは自作のコンパイラ・リンカで作った slimcc 版（build/kslim/kernel8.img）。実機の上で作り直して、そのまま起動できる。GCC 版（build/kernel8.img）は比べるためだけに残す
- **速さが命**（ユーザーの方針）: 起動、コンパイル、アプリの起動、計算の速さをいちばん大事にする
- **サブ OS**（ユーザーの方針）: PC や Pi 5 から USB・Wi-Fi でつないで使う。HDMI とキーボードはいちばん最後
- **使い勝手は Linux に合わせる**: C ライブラリ、端末、シェル、コマンドの出力。合わせないのは Monocs の設計そのもの（プロセスがない、OS とアプリが同じ特権・同じアドレス空間）

## 2. 今できること

| 分野 | 内容 |
|---|---|
| 核 | 4 コア（アプリ 3 + サービス 1）、メモリの保護（カーネルのコード、スタックのあふれ）、例外からの復帰、アプリ用のヒープ、アプリからほかのプログラムを動かす `system()`（入れ子のアプリ） |
| ファイル | SD カード（SDHOST）の FAT32、読み出しのキャッシュ、書き込みのまとめ、`/dev/null` |
| シェル | 行の編集・履歴・補完、パイプ、リダイレクト、`&&`・`\|\|`・`;`、bash と同じ書き方（`if`・`for`・`while`・`until`・`case`・関数・`( )`・`{ }`・ヒアドキュメント、`test`・`[`、展開 `$NAME`・`${NAME:-x}`・`${NAME/a/b}` など・`$(...)`・`$((式))`・ファイル名の `*`・`{a,b}`・`~`、値の分け直し、`NAME=値 コマンド`、`sh FILE`・`source`。閉じていない行は `> ` で続きを読む。`&` はない）、PATH、`exit`、`unset`・`true`・`false`、`timeout`（SIGTERM・`-k`）、`dmesg`（カーネルの記録） |
| コマンド（/usr/bin） | ls cat cp mv rm rmdir mkdir touch echo df clear less grep head tail wc host passwd fatattr curl tar gunzip zcat pinctrl i2cdetect i2cget i2cset spi softuart pwm top |
| 言語 | slimcc-a64（C23、SSA の中間表現、インライン展開、命令の並べ替え）、内蔵アセンブラ、リンカ slimld、picolibc、curses |
| ネットワーク | USB イーサネット（192.168.7.2）と内蔵 Wi-Fi（WPA2、DHCP、起動時に自動でつなぐ）、lwIP、DNS。アプリからは Linux と同じソケットか、Monocs の `monocs_tcp_*`・`monocs_udp_*` |
| サービス（コア 3） | httpd（HTTP）、telnetd、sshd（SSH）、wifid（Wi-Fi の自動の接続）、sntp（時刻合わせ） |
| 外へ | `curl`（HTTP・HTTPS、圧縮）、TLS のライブラリ（/usr/lib/tls、中身は BearSSL）、展開（/usr/lib/inflate） |
| ハードウェア | GPIO のライブラリ（/usr/lib/gpio、`-lgpio`。機能・プル・読み書き・エッジの待ち）、`pinctrl`。I2C のライブラリ（/usr/lib/i2c、`-li2c`）、`i2cdetect`・`i2cget`・`i2cset`。SPI のライブラリ（/usr/lib/spi、`-lspi`）、`spi`。ソフトウェアの UART（/usr/lib/softuart、`-lsoftuart`。好きな GPIO で 115200bps まで）、`softuart`。PWM（/usr/lib/pwm、`-lpwm`。GPIO12・13・18・19）、`pwm` |
| 守り | USB からはパスワードなし、Wi-Fi からはパスワード（`passwd`）か公開鍵。HTTP はダイジェスト認証 |
| Linux との互換 | Linux 用のソースと Makefile をそのまま置いて動かす（シェルの `make` が Makefile を読んで実行ファイルを作る。`./prog` で動かす）。`system()`・`popen`、getopt・setenv・glob・正規表現・ワイド文字・ロケール（printf の `%ls`）、シグナル（Ctrl-C で SIGINT の関数、alarm）、ソケット・select・poll・getaddrinfo、pthread（スレッド 1 つに空いたコアを 1 つ）、OpenMP（`-fopenmp`）。ユーザーの fd（ファイルマネージャー）と mi（エディタ）がソースのまま動く |
| 道具 | `top`（コアごとの忙しさ・割り込み、メモリ、サービスごとの CPU。`top -b -n 2` は ssh・nos.py から）、`perf`・`perf record`（性能カウンタ）、`mem`、`clock`、`wifi`、`poweroff`、Pi 5 からの更新（`tools/uart/nos.py`） |

## 3. つなぎ方

- Zero 2 W の「USB」端子（内側）を Pi 5 につなぐ（電源もここから。PWR IN は空ける）。UART（GPIO14/15 ⇔ Pi 5 の `/dev/ttyAMA0`、3Mbps）もつないだままにする（起動の表示と、動かなくなったときの復旧に使う）
- Pi 5 からは: USB シリアル `/dev/serial/by-id/usb-Monocs_Monocs_console-if00`、USB イーサネット `192.168.7.2`（Pi 5 は 192.168.7.1）
- Wi-Fi: `/etc/wpa_supplicant.conf`（Linux と同じ形）があれば、起動して約 10 秒でつながる。アドレスは DHCP（今の家のルーターでは 192.168.3.96）
- 入り方: `ssh monocs@192.168.7.2`（USB。パスワードなし）、`ssh ユーザー名@Wi-Fi のアドレス`、`telnet`、`picocom -b 3000000 /dev/ttyACM0`
  - Windows からは ssh か、端末ソフトの RLogin（telnet.exe は mi の画面が崩れる）
  - ファイル（2026-11-16 から）: `scp FILE monocs@アドレス:/tmp/`、`sftp monocs@アドレス`、`sshfs monocs@アドレス:/ ~/monocs`（Monocs のファイルを手元の
    エディタで直接編集する。外すのは `fusermount -u ~/monocs`）。WinSCP・FileZilla（SFTP）も使える。鍵は ssh と同じ
- Pi 5 の `~/.ssh/config`（2026-11-21）に `Host monocs`（HostName 192.168.3.96、User monocs、IdentityFile ~/.ssh/id_ecdsa_monocs）。
  `ssh monocs`・`scp FILE monocs:/tmp/`・`sftp monocs` だけで入れる。Wi-Fi のアドレスが変わったら HostName を直す
- **Wi-Fi だけで開発する**（2026-11-16 から。USB・UART の線なしで。電源は PWR IN から）: `export NOS_HOST=192.168.3.96` にすると、
  nos.py は ssh（鍵 `~/.ssh/id_ecdsa_monocs`。Monocs の `/root/.ssh/authorized_keys` に登録済み）で操作する。コマンドは ssh の exec
  （`ssh -i ~/.ssh/id_ecdsa_monocs monocs@192.168.3.96 'ls /'` と同じ。シェルの 1 行を、プロンプトで待つ間に動かす。打ち込まないので
  使っている最中でもよい）、ファイルはふつうの sftp（約 2〜3MB/s）、kernel・reboot は ssh がまたつながるまで待つ（約 11 秒）。
  - カーネルの入れ替え（2026-11-16 から）: `nos.py kernel` は試しの起動をする（新しいカーネルを /boot/kernel8.trial.img に
    置き、reboot tryboot。ssh が通って tryboot confirm できたら kernel8.img にする。90 秒たっても確認されない・固まったときは、
    ウォッチドッグがリセットして、ファームウェアが前のカーネルで起動し直す（約 2 分））。ただし、起動のごく初め（約 0.2 秒、
    tryboot_init より前: boot.S・MMU・メモリの用意など）で止まるカーネルは戻せない（SD カードを PC で直す）。
  - 気をつけること: 新しいカーネルは先に QEMU で確かめ、送る前に `nos.py cmd 'cp /boot/kernel8.img /tmp/kernel8.prev'`。起動の表示は見えない（`nos.py log`・dmesg で）。
    sshd は同時に 3 接続まで（対話の ssh を開いたままでも nos.py が使える。対話のシェルは 1 つまで）。
    アドレスは DHCP なので変わることがある（ルーターで固定するとよい）。USB の試験（hwcheck の一部）は線がいる。

## 4. よく使うコマンド（Pi 5 のリポジトリの一番上で）

```sh
make                                   # 標準のカーネル（slimcc 版）build/kslim/kernel8.img
make kernel-gcc                        # 比べる用の GCC 版 build/kernel8.img
make sdimage                           # SD カードのイメージ build/sdcard.img（QEMU 用・実機用。標準のカーネル）
make qemu                              # QEMU で起動（標準のカーネル。GCC 版は make qemu-gcc）

python3 tools/uart/nos.py kernel                      # 標準のカーネルを実機に送って再起動（/boot/kernel8.img）
python3 tools/uart/nos.py cmd 'ls /'                  # 実機でコマンド（終了の値はそのコマンドの値。$? と同じ）
python3 tools/uart/nos.py log                         # カーネルの記録（dmesg）を HTTP で読む（打ち込まない）
python3 tools/uart/nos.py push FILE /path             # ファイルを送る
python3 tools/uart/nos.py pushdir DIR /path           # フォルダを送る
python3 tools/uart/nos.py run samples/hello.c         # 送って作って動かす
python3 tools/uart/nos.py poweroff                    # 書き出して止める（シェルの poweroff。電源を入れ直すまで動かない）
# Wi-Fi だけ: export NOS_HOST=192.168.3.96（上の 3）。同じ書き方で ssh を使う（打ち込まないので、下の「止まる」もない）
#   コンソールでアプリ（top など）が動いていると、終わるまで待つ（2 秒で知らせが出る）。--stop-app を付けると止めてから動かす
# nos.py は、実機が使われている（アプリが動いている・telnet・ssh でつながっている）と、打ち込まずに止まる。
# 続けるなら --force（試験のスクリプトは NOS_FORCE=1）。様子は curl http://192.168.7.2/.status

sh tools/test/verify.sh quick          # QEMU の検証（cc-test、ld-test、セルフホストの一致。約 5 分）
sh tools/test/hwcheck.sh               # 実機の検証（GCC 版・標準の slimcc 版の順に送って確かめる。最後に標準が残る）
sh tools/ssh/test.sh                   # SSH のサーバー（Linux で動かして本物の ssh でつなぐ）
sh tools/inflate/test.sh               # 展開（gzip・zlib と比べる）
sh tools/inflate/tartest.sh            # tar・gunzip（GNU tar・gzip と比べる）
sh tools/test/linuxapi.sh              # Linux の関数の互換（QEMU。Linux の出力と比べる）
sh tools/test/shell.sh                 # シェルの変数の展開・$?（bash と比べる）、timeout、dmesg（QEMU）
sh tools/test/netapi.sh                # ソケット・select・poll（実機。Linux の出力と比べる）
python3 tools/test/sockets/run.py example.com   # ソケット（実機と Pi 5。TCP・UDP・select のサーバー・DNS）
sh tools/make/test.sh                  # Makefile の解釈器（GNU make と比べる）
```

ほかの試験の一覧は `tools/test/README.md`。

変えたものを実機に置く:

| 変えたもの | 置き方 |
|---|---|
| カーネル（os/、cc/） | `make && nos.py kernel`（または下の「実機の上で作り直す」） |
| /usr/bin のコマンドと /usr/sbin のサービス | `sh tools/update_bin.sh`（ソースのまま入れ替えて、再起動） |
| 共有のライブラリ | `sh tools/tls/mklib.sh && nos.py pushdir build/lib /usr/lib` |
| ヘッダー（monocs.h など） | `nos.py push os/api/include/monocs.h /usr/include/monocs.h`（sys/socket.h などのフォルダのものは `/usr/include/sys/` などへ。picolibc の設定を変えたら `nos.py pushdir build/picolibc-install/include /usr/include` の後に os/api/include のものを送り直す） |
| 自分のアプリ（Linux のフォルダのまま） | `sh tools/apps/push.sh ~/dev/tools/NAME`（.c・.h・Makefile などを /root/apps/名前 に送る。ソースのまま、名前で動く。名前は Makefile のプログラムの名前。`-c` で先に実機のフォルダを消す）。mi は `sh tools/apps/mi.sh` |
| セルフホストの材料 | `make sdimage-self && nos.py pushdir build/selfsrc /usr/src/kernel`、実機で `cd /usr/src/kernel`、`cc -o buildlibc buildlibc.c && ./buildlibc`、`cc -o build-kernel build.c && ./build-kernel` |

実機の上で作り直して起動する（Monocs の中で完結。2026-11-16 に実機で確かめた: slimcc 版のカーネルの上で C ライブラリ 43 秒、カーネル 8 秒、どちらも Linux 上のビルドと一致。2026-11-21 にも Wi-Fi だけで、材料を送り直して `run buildlibc.c` 40.7 秒・`run build.c` 10.4 秒（3 コア）、どちらも Linux 上のビルドと SHA-256 が一致）:

```sh
cd /usr/src/kernel
cc -o buildlibc buildlibc.c && ./buildlibc       # C ライブラリ（picolibc）→ /usr/src/kernel/build/picolibc-slimcc/libc.a
cc -o build-kernel build.c && ./build-kernel     # カーネル → /usr/src/kernel/build/kslim/kernel8.img（libc.a はこれを使う）
cp /boot/kernel8.img /tmp/kernel8.prev   # 今のカーネルを残す（動かなければ SD カードを PC に差して戻す）
cp /usr/src/kernel/build/kslim/kernel8.img /boot/kernel8.img
sync
reboot
```

## 5. 変更したら確かめること

1. `make`（と、比べるなら `make kernel-gcc`）が警告なしで通る
2. `sh tools/test/verify.sh quick` で `cc-test exit=0`・`すべて成功`・`IDENTICAL` が 3 つ（コンパイラ・C ライブラリを変えたら `quick` なしで）
3. `sh tools/test/hwcheck.sh`（実機。両方のカーネル）
4. **速さ**: コンパイル・起動・実行の道筋に手を入れたら、前後で実機の速さを比べる
   - 測る前に `clock` の measured が 1000 MHz であることを見る（`config.txt` の `force_turbo=1` で保っている）
   - 目安（3 コア）: 標準の slimcc 版のカーネルの上で（2026-11-16）カーネルのビルド 8 秒、C ライブラリ 43 秒、`primes.c` 44 ms、`primes_mc.c` 495 ms。GCC 版の上では（2026-11-14）カーネル 5.5〜5.9 秒、C ライブラリ 31〜36 秒、`ls /usr/bin` の往復 0.47 秒
   - SD カードの書き込みは同じ量でも 2〜3 倍揺れるので、何回か・交互に測る
5. Linux との互換の関数（os/libc/ の posix.c・signal.c・socket.c など）を変えたら、`linuxapi.sh`（QEMU）と `netapi.sh`・`sockets/run.py`（実機）で Linux の出力と比べる
6. 動作を確かめたら、ステップ単位でコミットする（メッセージは日本語、最後に `Co-Authored-By:` の行）

## 6. 気をつけること（実機を止めないために）

- **動かないカーネルを送ると、SD カードを PC に差して `boot/kernel8.img` を戻すことになる**。危ない変更の前に `nos.py cmd 'cp /boot/kernel8.img /tmp/kernel8.old'`
- **割り込みを新しく足すカーネルを送る前に、起動時にそれを使うサービスを外す**（`/etc/services.txt`）。上がり続ける割り込みは、USB も UART も効かなくする（2026-11-13 に Wi-Fi で起きた）
  - そのときの復旧: 電源を入れ直した直後に UART から、シェルに組み込みのコマンドだけを先に送る（`recv /etc/services.txt …` で一覧を置き換え、`reboot`）。CLAUDE.md の「内蔵 Wi-Fi の受信を割り込みで読む」
- SD カードのドライバを疑っているときは、そのドライバの上でカーネルをコピーしない
- コンパイラ（cc/）を変えたら、`make cc-test` に加えてカーネルとセルフホストの一致（verify.sh）も確かめる
- third_party は無改造（変更はパッチで）。足す・差し替えるときはユーザーに確認する
- 取り出した区画・古いカーネルの控えは Pi 5 の `build/sd-backup/`（git の外）
- **ユーザーが実機を使っている最中に、nos.py で打ち込まない**（2026-11-14 に、ユーザーが telnet・ssh で mi・less を開いている画面に、コマンドの文字を打ち込んでしまった）。nos.py は /.status を見て止まるが、`--force` を付けるときは気をつける
- picolibc の設定（Makefile の PICOLIBC_OPTS）を変えたら、`rm -rf build/picolibc build/picolibc-install build/picolibc-slimcc` してから作り直す（meson が古い設定のまま使う）

## 7. どこに何があるか

```
os/boot/      起動（boot.S、linker.ld、config.txt）
os/kernel/    例外、MMU、メモリ、マルチコア、ロック、TLS、アプリの実行、サービス、時計、性能カウンタ、コンソールの写し
os/drivers/   uart、usb、sd（sdhost・sd_emmc）、sdio・wifi・wifi_auto（内蔵 Wi-Fi）、timer、irq、mailbox、led、rng
os/fs/        VFS、FatFs のつなぎ込み、SD カードの読み書きのまとめ
os/net/       lwIP のつなぎ込み、TCP・UDP・DNS の API
os/libc/      picolibc のつなぎ込み、malloc（TLSF）、termios、浮動小数点と 10 進の変換
os/slimcc/    組み込みのコンパイラの呼び出し（run、cc、キャッシュ）
os/api/       OS API の表（api.def）と、アプリ用のヘッダー（include/）
os/shell/     シェル、行の編集、perf、wifi コマンド
os/curses/    curses
cc/           slimcc-a64（コンパイラ・アセンブラ・リンカ）
bin/ services/ lib/ samples/    SD カードの同じ名前のフォルダに入るもの（C のソース）
third_party/  picolibc、FatFs、slimcc、lwIP、bearssl、firmware
tools/        Pi 5 で使う道具（uart/nos.py、test/、ssh/、tls/、inflate/、selfhost/、perf/、pi5/）
```

SD カード: `/boot`（カーネル）、`/usr/bin`、`/root/apps`（自分のアプリ。mi など）、`/usr/lib`（共有のライブラリ、`firmware/`、`slimcc/`）、
`/etc`（設定）、`/usr/include`、`/usr/sbin`、`/usr/share/samples`、`/usr/src/kernel`（セルフホストの材料）、`/tmp`（作業用）。

## 8. 決めてある方針（変えるときはユーザーに相談する）

- 速さが命。機能を足すときは速さを落とさない
- サブ OS として、つないで使うのを便利にしていく。HDMI・キーボード・Bluetooth は後
- カーネルに入れるのは起動に必要なドライバだけ。センサー・GPIO の部品は `/usr/lib` のライブラリ、常駐するもの（音、画面）は `/drivers`（仕組みは、最初の常駐型を作るときに一緒に作る）
- TLS 1.3・HTTP/2 は見送り（困ったときに見直す）
- SD カードへの書き出しを遅らせすぎない（電源が切れたときに失われる量）

## 9. 次の候補（どれも、始める前にユーザーと相談する）

**slimcc の出すコードを良くする**（ユーザーの希望で、後で続ける。2026-11-16）: 標準のカーネル（slimcc 版）と
GCC 版の差（今は約 1.5 倍）を縮める。今の位置・測り方（`tools/slimcc/devbench.sh`）・確かめ方
（`tools/slimcc/cmp_s.sh`・`cmp_o.sh`、make cc-test の GCC と比べる試験）・候補・試して見送ったものは
`docs/slimcc-codegen.md` にまとめてある。

**サンドボックス**（人が作った C のコードを安全に動かす。2026-11-22 に設計を相談、まだ作らない。ユーザーの「まだ作るのは待って」）: 決めたことは `docs/sandbox.md`（EL0 ＋ 画面・キー・マウスだけ、コンパイラは 1 つ、`run URL` の置き場所と許した版のハッシュ、`cc --report`）。

**Wi-Fi を速くする**（2026-11-22 に 1・2 と TCP の窓をした。CLAUDE.md の「Wi-Fi を速くする（2026-11-22）」）: 測り方は
`python3 tools/perf/wifibench.py recv|send MB`（Monocs で tools/perf/wifibench.c を /root/work に置いて動かす）と `wifi perf`。
今は受ける 3.6〜5MB/s・送る 2.3〜3MB/s で、転送の間 Monocs は半分〜8 割の時間何もしていない（電波・往復の時間で頭打ち）。
1 枠あたりのコマンドを減らす・glom・DMA は、Monocs の処理を減らすだけで速さには効かない見込みなので見送り中

つないで使うのを便利にするもの（おすすめの順）:

1. **名前でつながる（mDNS。`monocs.local`）**: Wi-Fi のアドレスは DHCP で変わりうる。小さな作業
2. **sftp・scp**: PC のエディタ（VS Code、WinSCP）から Monocs のファイルを直接開ける。中くらい
3. **`ssh ホスト コマンド`**: PC のスクリプトから Monocs でビルド・実行。中くらい
4. SSH の Ed25519 の鍵（署名の検証を自作する）

Linux のプログラムを動かす（2026-11-14 のユーザーの方針「Linux で作ったコードをそのまま動かす」の続き）:

- **おすすめ**: 実際に使いたい Linux のプログラムを Monocs に置いて動かしてみて、足りないもの（`undefined symbol` など）が出たら足す
- まだないもの: `fork`・`exec`（設計と合わない）、スレッドは pthread（空いたコアの数まで。スケジューラなし）、`dup`・`dup2`、IPv6、ループバック（127.0.0.1）、`sendmsg`・`recvmsg`、`setitimer`、`<endian.h>` の `be32toh` など（slimcc に `__builtin_bswap` がない）、ncurses の `newwin`・スクロール、`mmap`・`dlopen`・`iconv`、system() の WIFSIGNALED

そのほか:

- ハードウェアの実験台として使う: GPIO・I2C・SPI・UART（ソフトウェア）・PWM のライブラリ（/usr/lib/gpio・i2c・spi・softuart・pwm）はある。実際の機器（センサーなど）をつないで使う。I2C のリピーテッドスタートは機器で確かめていない
- 速さを詰める: Wi-Fi の受信（枠ごとの SDIO のコマンドの数、25MHz）、コンパイラのコード生成、ビルドのロックの待ち
- わかっていること・残っていること:
  - 家のルーターでは、つないでしばらくの間、最初の ARP の要求に応答が来ないことがある（1 秒以内の再送で通る。iPhone のテザリングでは起きない）
  - ターミナルの大きさの変更は、USB シリアル・UART には伝わらない（telnet・ssh は伝わる）
  - アプリが定義する `_Thread_local` の変数、`long double` の中間表現、複素数は未対応

## 10. 最近の大きな流れ（2026-11-13〜14）

内蔵 Wi-Fi（SD カードを SDHOST に移して EMMC を空ける → SDIO → 接続・DHCP → 起動時の自動の接続 → DNS・NTP → パスワードでの守り
→ 受信を割り込みで）、HTTPS（BearSSL、curl）、圧縮の展開（tar・gunzip）、SSH のサーバー、`exit`、SD カードのルートの整理（/boot）、
CPU を 1000 MHz に保つ（force_turbo）、全体の見直し（不具合の修正、使われていないコードの削除、長い関数の分割）、
キャッシュの偽の共有を減らす、`system()`（ユーザーのファイルマネージャー fd がソースのまま動く。UTF-8 の変換関数、readdir の `.`・`..`、
curses の nl、コマンドの `--`）、Linux の Makefile をそのまま読む、getopt・setenv・glob・正規表現・ワイド文字などの関数、
picolibc をマルチバイトありに（printf の `%ls`）、シグナル（Ctrl-C で SIGINT の関数）と popen、Linux と同じソケット・select・poll・getaddrinfo・alarm。
