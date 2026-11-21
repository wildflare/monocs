# 実機（Zero 2 W）での動作確認手順

Monocs を実機で動かし、確かめる手順です。

**今のつなぎ方（2026-11-07）**: Zero 2 W の「USB」端子（内側）を Pi 5 の USB につなぎます（電源も Pi 5 から入るので、
PWR IN には何もつなぎません）。操作と転送は USB（シリアル・イーサネット）、起動の表示を見るときだけ UART を使います。
UART のジャンパー線（「1. 配線」）も、当面つないだままにします（USB を壊したカーネルを送ったときに、起動の表示を見るため）。

| 使い方 | Pi 5 から |
|---|---|
| シェル（USB シリアル） | `picocom /dev/ttyACM0`（終わりは Ctrl-A、Ctrl-X） |
| シェル（telnet。画面の大きさも伝わる） | `telnet 192.168.7.2`（終わりは Ctrl-]、`quit`） |
| ファイル | `curl http://192.168.7.2/パス`（GET）、`curl -T ファイル http://192.168.7.2/パス`（PUT） |
| 起動の表示（UART） | `picocom -b 3000000 /dev/ttyAMA0` |
| 自動の操作・更新 | `tools/uart/nos.py`（「7. SD カードを差し替えずに更新する」） |
| まとめて確かめる | `sh tools/test/hwcheck.sh`（tools/test/README.md） |

Pi 5 の usb0 には NetworkManager の接続 `monocs-usb`（192.168.7.1/24）が設定してあります。
Pi 5 は chrony で Monocs に時刻を配ります（`sh tools/pi5/setup-chrony.sh`。Monocs のサービス sntp が起動の直後に合わせる）。
新しい Pi 5 で使うときは、この 2 つを設定し直します（`sh tools/pi5/setup-usbnet.sh`、`sh tools/pi5/setup-chrony.sh`）。
以下の 0〜6 は、最初に実機で起動を確かめたときの手順です（UART が中心）。

## 0. ジャンパー線なしでの簡易確認（ACT LED）

UART をつながなくても、基板上の緑の LED（ACT）で起動を確かめられます。

1. 「3. SD カードの作成」の手順で SD カードを作り、Zero 2 W に差して電源を入れます。
2. LED の様子を見ます。

| LED の様子 | 意味 |
|---|---|
| 電源投入直後に不規則に点滅する | ファームウェアが SD カードを読んでいる（OS とは無関係） |
| その後 1 秒周期（0.5 秒点灯、0.5 秒消灯）で点滅し続ける | 起動、MMU、メールボックス、タイマー割り込み、SD カードのマウントまで動いている（成功） |
| 0.2 秒周期の速い点滅 | 起動はしたが、SD カードのマウントに失敗した（SD カードドライバの問題） |
| 点灯したまま | カーネルは起動したが、途中（MMU、メールボックスなど）で止まっている |
| 消えたまま、または不規則な点滅が続く | カーネルが起動していない（SD カードの中身や config.txt の問題） |

### C のコンパイルと実行の確認（ステップ 6）

`make sdimage-blink` で `build/sdcard-blink.img` を作り、同じように SD カードに書き込んで起動します。
起動時に `samples/blink.c` が slimcc-a64 でコンパイルされ、自動で実行されます。

| LED の様子 | 意味 |
|---|---|
| 「ピカッ、ピカッ、休み」を繰り返す（約 1 秒周期で 2 回ずつ光る） | 実機で C のコンパイルと実行ができている（成功） |
| 1 秒周期の規則正しい点滅のまま | コンパイルか実行に失敗して、シェルに戻っている |
| 点滅が止まる | 生成したコードの実行中に止まっている |

確認が終わったら、通常のイメージ（`build/sdcard.img`）に戻してください。

SD カードの初期化には最大で数秒かかります。速い点滅に変わるかどうかは、10 秒ほど様子を見てから判断してください。
結果を教えてください。UART での確認（以下の手順）は、ジャンパー線が届いてから行います。

### セルフホストの確認（Monocs 上でカーネルをビルドする）

`make sdimage-self-auto` で `build/sdcard-self-auto.img` を作り、同じように SD カードに書き込んで起動します。
起動時に `/autorun.c`（`tools/selfhost/autorun.c`）が、SD カードの `/usr/src/kernel` にあるソース一式から
Monocs 自身のカーネルをビルドします（実機で約 10 秒）。

| LED の様子 | 意味 |
|---|---|
| 点灯したまま | ビルド中 |
| 「ピカッ、ピカッ、ピカッ、休み」を繰り返す | ビルドが成功した |
| 0.1 秒ごとの速い点滅 | ビルドに失敗した |
| 点灯したまま 10 分以上変わらない | ビルドの途中で止まっている |

成功したら電源を切り、SD カードを PC に差して、`/usr/src/kernel/build/kslim/kernel8.img` のハッシュ値を確認してください。
Windows ではコマンドプロンプトで次のようにします（`E:` は SD カードのドライブ）。

```
certutil -hashfile E:\src\build\kslim\kernel8.img SHA256
```

Linux 上のビルド（`make kernel-slimcc` の `build/kslim/kernel8.img`）と同じ値なら、実機の Monocs が
自分自身を正しくビルドできています。値は作業の報告に書きます。

### マルチコアの確認（4 つのコアで計算する）

`make sdimage-smp-auto` で `build/sdcard-smp-auto.img` を作り、同じように SD カードに書き込んで起動します。
起動時に `/autorun.c`（`tools/smp/autorun.c`）が次の 3 つを確かめます（QEMU で数秒）。

1. 4 つのコアが起動するか
2. 4 つのコアで分担して数えた素数の数が、1 つのコアで数えた数と一致するか
3. 4 つのコアが同じ変数をアトミック命令で 100 万回ずつ増やし、合計が 400 万になるか
   （コア間のキャッシュの一貫性。`CPUECTLR_EL1.SMPEN` はファームウェアが立てる前提で、自前では立てていません）

| LED の様子 | 意味 |
|---|---|
| 点灯したまま | 確認中 |
| 「ピカッ」を 4 回、休み、を繰り返す | すべて成功した |
| 速い点滅を約 2 秒、そのあと「ピカッ」を何回か、休み、を繰り返す | 失敗した。「ピカッ」の回数は起動したコアの数 |
| 点灯したまま 1 分以上変わらない | 途中で止まっている（キャッシュの一貫性の問題の可能性が高い） |

失敗したときは、「ピカッ」の回数も教えてください。

## 1. 配線

Zero 2 W と Pi 5 を、ジャンパー線 3 本でつなぎます。両方とも電源を切ってから作業してください。

| Zero 2 W | | Pi 5 |
|---|---|---|
| 8 番ピン（GPIO14、TXD） | → | 10 番ピン（GPIO15、RXD） |
| 10 番ピン（GPIO15、RXD） | ← | 8 番ピン（GPIO14、TXD） |
| 6 番ピン（GND） | — | 6 番ピン（GND） |

- 5V と 3.3V のピンはつながないでください。Zero 2 W の電源は、Zero 2 W 自身の USB 電源端子（PWR IN）から取ります。
- 両方とも 3.3V の信号なので、レベル変換は要りません。

## 2. Pi 5 側の準備（初回のみ）

1. `/boot/firmware/config.txt` の末尾に次の 1 行を追加し、再起動します。GPIO14/15 の UART0 が `/dev/ttyAMA0` として使えるようになります。

   ```ini
   dtparam=uart0=on
   ```

2. シリアル端末ソフトを入れます。

   ```sh
   sudo apt install picocom
   ```

3. 再起動後、`ls -l /dev/ttyAMA0` でデバイスがあることを確認します。

## 3. SD カードの作成

### 方法 A: イメージを書き込む（Windows の Rufus なども使える）

1. `make sdimage` を実行します。`build/sdcard.img`（64MB）ができます。
2. このファイルを Rufus、Raspberry Pi Imager（「カスタムイメージを使う」）、`dd` などで SD カードに書き込みます。
   SD カードの中身はすべて消えます。

### 方法 B: ファイルをコピーする

1. プロジェクトのディレクトリで次を実行します。`build/sdcard/` に必要なファイルがそろいます。

   ```sh
   make sdcard
   ```

   ファームウェア（`bootcode.bin`、`start.elf`、`fixup.dat`、Device Tree、`disable-bt.dtbo`）は、Pi 5 の `/boot/firmware` からコピーします。

2. SD カードを FAT32 でフォーマットし（パーティションは 1 つ）、`build/sdcard/` の中身をすべて、SD カードのルートにコピーします。`overlays/` ディレクトリも含めてください。

   例（SD カードが `/media/user/BOOT` にマウントされている場合）:

   ```sh
   cp -r build/sdcard/* /media/user/BOOT/
   sync
   ```

## 4. 起動

1. Pi 5 で端末を開きます。終了するときは `Ctrl-A` に続けて `Ctrl-X` を押します。

   ```sh
   picocom -b 3000000 /dev/ttyAMA0

   Monocs は 3Mbps で通信します（ファームウェアのログは 115200bps なので、起動直後は化けて見えます）。
   配線が長いなどで化けるときは、シェルで `baud 115200` と入力し、10 秒以内に端末を
   `picocom -b 115200` で開き直して `y` を押すと、115200bps に戻せます（`y` が届かなければ元の速さに戻ります）。
   ```

2. Zero 2 W に SD カードを差し、電源を入れます。

## 5. 期待される出力

`config.txt` で `uart_2ndstage=1` を指定しているので、最初にファームウェアのログが流れます。そのあとに、次のような出力が続きます。

```
Monocs: hello from Raspberry Pi Zero 2 W
boot EL 2, SCTLR_EL1 0x30d01805
heap 0x........-0x........ (... MB)
timer: CNTFRQ ........ Hz, tick 100 Hz
usb: ...
smp: 4 cores
vfs: SD card mounted
svc: starting /usr/sbin/httpd.c
svc: starting /usr/sbin/telnetd.c

Monocs shell. "help" でコマンドの一覧を表示します
/>
```

プロンプト `/>` が出たら、次のコマンドを試してください（方法 A のイメージにはサンプルが入っています）。

```
help
ls samples
cc -o /tmp/hello samples/hello.c && /tmp/hello
cc -o /tmp/primes samples/primes.c && /tmp/primes
cc -o /tmp/blink samples/blink.c && /tmp/blink      ← LED が速く点滅します。Ctrl-C で止めてください
cc -o /tmp/crash samples/crash.c && /tmp/crash      ← 例外の内容が表示され、シェルに戻れば成功です
```

## 6. 確認していただきたい点

結果は、次の項目ごとに教えてください。うまくいかなかった場合は、端末の出力をすべて貼り付けてください。

1. **起動メッセージ**：`Monocs: hello ...` は表示されたか。
2. **起動時の EL**：`boot EL` の値（2 を想定しています）。
3. **メールボックス**：`heap` の行が表示されたか。表示されない場合や `memory: mailbox failed` が出た場合は、メールボックスに渡しているアドレスの別名（`0xC0000000`）が実機では合っていない可能性があります。
4. **ヒープの範囲**：`heap` の行の値。RAM 512MB から GPU 用の領域を引いた分が、ヒープの上限になる想定です。
5. **タイマー**：`CNTFRQ` の値。LED がほぼ 1 秒周期で点滅するか。
6. **入力**：キー入力が表示されるか。Enter でコマンドが実行されるか。Backspace で消せるか。
7. **キャッシュの一貫性**：`CPUECTLR_EL1.SMPEN` を立てていないので、表示が崩れたり止まったりしないか。
   `samples/hello.c` を作って動かせれば、slimcc-a64 が書いたコードを命令キャッシュとの整合を取って実行できています。
8. **SD カード**：`vfs: SD card mounted` とファイル一覧が出るか。`sd: ... failed` が出た場合は、その行を送ってください。
9. **C の実行**：上の `run` の各コマンドの結果。`primes.c` の実行時間（ms）も教えてください。

## 7. SD カードを差し替えずに更新する（USB・HTTP。UART でも）

Pi 5 からファイルやカーネルを送れます（`tools/uart/nos.py`）。既定では USB シリアルでコマンドを送り、
ファイルは HTTP（PUT）で送ります（HTTP のサービスが止まっていればシェルの `recv`）。`--uart` で UART を使います。
nos.py と同じ端末（USB シリアル）を picocom で開いている間は使えないので、閉じてから実行してください
（telnet は開いたままでよい）。

```sh
tools/uart/nos.py cmd 'ls /'                  # コマンドを実行して出力を表示
tools/uart/nos.py push samples/hello.c /hello.c   # ファイルを送る（CRC32 で確かめる）
tools/uart/nos.py run samples/primes.c        # C のファイルを /tmp に送り、作って動かす
make && tools/uart/nos.py kernel              # 標準のカーネル build/kslim/kernel8.img を /boot/kernel8.img に送って再起動
tools/uart/nos.py pushdir mylib /usr/lib/mylib    # フォルダごと送る（アプリ、共有のライブラリなど）
```

- `nos.py kernel` は、隣にある関数名の表（`build/kslim/kernel8.sym`）も `/boot/kernel8.sym` に送ります（シェルの `perf record` が使う）。`/boot` がない古い SD カードには、ルートの `/kernel8.img` に送ります。

- 送ったファイルは、全体を受け取って CRC32 が合ったときだけ書き込みます（途中で失敗しても元のファイルは残ります）。
- 動かないカーネルを送ると起動しなくなります。そのときは SD カードを PC に差して、`boot/kernel8.img` を書き戻してください。

## 8. SD カードのフォルダ

ルートに置くのは、Raspberry Pi のファームウェアがルートから読むもの（`bootcode.bin`・`start.elf`・`fixup.dat`・`config.txt`）と、下のフォルダだけです（2026-11-13 に整理）。このうち `bootcode.bin`・`start.elf`・`fixup.dat` には FAT の隠し属性が付いていて、`ls -a` でだけ見えます（`fatattr` で変えられます。PC では隠しファイルの表示を有効にすると見えます）。`config.txt` は直すことがあるので見えるままです。

| フォルダ | 中身 |
|---|---|
| `/boot` | カーネル（`kernel8.img`）、関数名の表（`kernel8.sym`）、dtb、`overlays/`（`config.txt` の `os_prefix=boot/`。`overlays/README` があるので overlays もここから読む） |
| `/etc` | 設定（`services.txt` 起動時に始めるサービス、`wpa_supplicant.conf` Wi-Fi、`htdigest` Wi-Fi からのパスワード、`ssh/` ホスト鍵、`ssl/certs/` 認証局） |
| `/tmp` | 自分で使う作業用（`nos.py run` もここに置く） |
| `/usr/bin` | Monocs に付いてくるアプリ（ls、cat、less、grep など。更新で入れ替わる） |
| `/root/apps` | 自分で作った・入れたアプリ（mi など。名前だけで動く。`/usr/bin` より先に探す） |
| `/usr/lib` | 共有のライブラリ（C のソースのまま。アプリの `compile_flags.txt` の `-l名前` で `/usr/lib/名前` をつなぐ。ライブラリのフォルダにも `compile_flags.txt` を置ける）。ほかに `/usr/lib/firmware/brcm`（内蔵 Wi-Fi のファームウェア。Linux と同じ場所）、`/usr/lib/slimcc/include`（コンパイラ付属のヘッダー） |
| `/usr/sbin` | 裏で動くプログラム（HTTP、telnet、SSH、Wi-Fi の自動の接続、SNTP）。起動時に始めるものは `/etc/services.txt` |
| `/usr/share/samples` | サンプルのプログラム |
| `/usr/include` | アプリが使うヘッダー（picolibc、`monocs.h` など） |
| `/usr/src/kernel` | セルフホストのビルドの材料（`nos.py pushdir build/selfsrc /usr/src/kernel`） |
| `/usr/src` | ソース（/usr/src/monocs は /usr/bin とサービス。作るのは build.sh） |

アプリ（`/root/apps/名前` のフォルダ）は、サブフォルダの中の .c も一緒にコンパイルします（「.」で始まるものは飛ばす）。
入れたくないファイル（テスト用の main を持つものなど）は、そのフォルダの `.runignore` に 1 行に 1 つ書きます
（.gitignore と同じ考え方。例: `test_buffer.c`、`*_test.c`、`tests/`）。

## うまくいかないとき

- **何も表示されない**：配線（TX と RX が交差しているか、GND がつながっているか）、`/dev/ttyAMA0` かどうか、ボーレートを確認してください。ファームウェアのログも出ない場合は、配線か Pi 5 側の設定の問題です。
- **ファームウェアのログは出るが、`Monocs` が出ない**：カーネルが起動していないか、UART の初期化に失敗しています。ファームウェアのログの末尾を送ってください。
- **文字化けする**：ボーレート（UART のクロック設定）の問題です。そのまま送ってください。
- **USB シリアル（/dev/ttyACM0）が現れない**：「USB」端子（内側）につないでいるか、電源だけのケーブルでないかを確かめてください。起動の表示（UART）に `usb:` の行があります。
- **SD カードの中身がおかしい（ファイルが壊れた）**：HTTP で区画をそのまま取り出して（`curl -o fat.img http://192.168.7.2/.sdcard/fat.img`）、Pi 5 で `fsck.vfat -n fat.img` で調べられます（CLAUDE.md「SD カードの読み書きをまとめる」）。
