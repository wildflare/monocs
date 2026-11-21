# 10. Pi 5 からの開発

OS そのものを直して実機で試すときの手順です。作業は Pi 5 のリポジトリ（`~/dev/c/monocs`）の一番上で行います。

## 10.1 ビルド

| コマンド | 作るもの |
|---|---|
| `make` | 標準のカーネル（自作のコンパイラで作る）`build/kslim/kernel8.img` |
| `make kernel-gcc` | 比べる用の GCC 版 `build/kernel8.img`（約 1.5 倍速い。ふだんは使わない） |
| `make cc` | コンパイラ slimcc-a64（Linux で動くもの）`cc/slimcc` |
| `make sdimage` | SD カードのイメージ `build/sdcard.img` |
| `make sdimage-self` | セルフホスト用の材料つきイメージ（`build/selfsrc` も作る） |
| `make qemu` | QEMU で起動（`make qemu-gcc` は GCC 版） |
| `make clean`・`make distclean` | 生成物を消す（distclean は build/ ごと） |

## 10.2 nos.py（実機の操作）

`tools/uart/nos.py` で、Pi 5 から実機を操作します。USB・UART・Wi-Fi のどれでも同じ書き方です。

```sh
export NOS_HOST=192.168.3.96          # Wi-Fi のとき（ssh を使う）。なければ USB、それもなければ UART

python3 tools/uart/nos.py cmd 'ls /'                   # コマンド（終了の値も返る）
python3 tools/uart/nos.py push FILE /path              # ファイルを送る
python3 tools/uart/nos.py pushdir DIR /path            # フォルダを送る（まとめて送って実機で展開）
python3 tools/uart/nos.py run samples/hello.c 引数     # 送って動かす
python3 tools/uart/nos.py kernel                       # カーネルを入れ替えて再起動（下の 10.3）
python3 tools/uart/nos.py reboot / poweroff
python3 tools/uart/nos.py log                          # dmesg を読む（打ち込まない）
```

- 実機が使われている（アプリが動いている、telnet・ssh でつながっている）と、打ち込まずに止まります。続けるなら `--force`。
- Wi-Fi のとき、コンソールでアプリ（`top` など）が動いていると終わるまで待ちます。`--stop-app` で止めてから動かします
  （エディタ mi は、保存していない編集が消えるので止めないでください）。
- つなぐたびに Pi 5 の日時を送ります。

## 10.3 カーネルを入れ替える

```sh
make && python3 tools/uart/nos.py kernel
```

Wi-Fi のときは **試しの起動（tryboot）** をします。

1. 新しいカーネルを `/boot/kernel8.trial.img` に置いて、`reboot tryboot`（次の 1 回だけ `tryboot.txt` で起動する）。
2. 新しいカーネルが ssh で答えて `tryboot confirm` できたら、`/boot/kernel8.img` にする（再起動から約 18 秒）。
3. 90 秒たっても確かめられない・固まったときは、ウォッチドッグがリセットして、**前のカーネルで起動し直します**（約 2 分）。
   nos.py は失敗と、新しいカーネルの記録（`/var/log/dmesg.1`）を出します。

戻せないのは、起動のごく初め（約 0.2 秒、試しの起動を確かめる前: 起動のアセンブリ、MMU、メモリの用意など）で止まるカーネルだけです。
そのときは SD カードを PC に差して `boot/kernel8.img` を戻します。危ない変更の前には控えを取っておきます。

```sh
python3 tools/uart/nos.py cmd 'cp /boot/kernel8.img /tmp/kernel8.prev'    # /tmp は起動で消えるので、残すなら /root に
```

USB・UART のときは、試しの起動をせずに直接書きます。

## 10.4 ほかのものを入れ替える

| 変えたもの | 入れ方 |
|---|---|
| `/usr/bin` のコマンド・`/usr/sbin` のサービス | `sh tools/update_bin.sh`（ソースを入れ替えて再起動） |
| 共有のライブラリ | `sh tools/tls/mklib.sh && nos.py pushdir build/lib /usr/lib` |
| ヘッダー（`monocs.h` など） | `nos.py push os/api/include/monocs.h /usr/include/monocs.h` |
| 自分のアプリ（Linux のフォルダのまま） | `sh tools/apps/push.sh ~/dev/tools/NAME`（`/root/apps/名前` へ） |
| エディタ mi | `sh tools/apps/mi.sh` |

**サービスやコマンドが新しい OS の関数を使うときは、先にヘッダーを送ります**（古いヘッダーのままだとコンパイルできず、サービスが動かない）。

## 10.5 確かめる

| コマンド | 中身 | 時間 |
|---|---|---|
| `sh tools/test/verify.sh quick` | QEMU で検証一式（コンパイラの試験、リンカの試験、セルフホストの一致） | 約 5 分 |
| `sh tools/test/verify.sh` | 上の全部（C ライブラリの作り直しも） | もっと長い |
| `sh tools/test/hwcheck.sh` | 実機の検証（GCC 版と標準のカーネルを順に送って試す） | カーネルごとに数分 |
| `make cc-test` | コンパイラの試験（GCC と結果を比べる） | |
| `sh tools/test/shell.sh` | シェル（bash と出力を比べる） | |
| `sh tools/test/linuxapi.sh` | Linux の関数（Linux の出力と比べる） | |

ほかの試験は `tools/test/README.md`。コンパイラ（cc/）を変えたら、`make cc-test` に加えて verify.sh でカーネルとセルフホストの一致も確かめます。
コンパイル・起動・実行の道筋を変えたら、前後で実機の速さを比べます（`clock` が 1000MHz であることを見て、何回か交互に）。

## 10.6 セルフホスト（Monocs の上で Monocs を作る）

Monocs は、自分の C ライブラリとカーネルを自分の上で作り直せます。できたものは Linux 上のビルドと 1 バイトも違いません。

```sh
# Pi 5 で材料を作って送る
make && make sdimage-self
python3 tools/uart/nos.py pushdir build/selfsrc /usr/src/kernel

# 実機で
cd /usr/src/kernel
run buildlibc.c          # C ライブラリ（picolibc の 1,012 ファイル）。約 41 秒
run build.c              # カーネル。約 10 秒 → build/kslim/kernel8.img
```

起動に使うなら、`/boot/kernel8.img` を控えてから写し、`sync`・`reboot` します。

## 10.7 決まりごと

- コミットのメッセージは日本語で、最後に `Co-Authored-By:` の行。
- `third_party/`（picolibc、FatFs、lwIP、BearSSL など）は改造しません。変えるときはパッチにして、足す・差し替えるときは相談します。
- カーネルは C23。レジスタは `volatile` の小さな関数にまとめ、番地には名前と出典（資料の章）を付けます。
- 実機を止めないための注意は `docs/resume.md` の 6。
