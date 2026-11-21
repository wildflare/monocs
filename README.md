# Monocs

Raspberry Pi Zero 2 W のための、小さな自作 OS です。**電源を入れるとすぐシェルが出て、その場で C を書き、コンパイルして動かせる専用機**を目指しています。

A small hobby OS for the Raspberry Pi Zero 2 W: boot straight into a shell, write C, compile it on the device and run it right away.
(The documentation is in Japanese.)

```
$ ssh monocs
/root> mi hello.c          # エディタで書く
/root> run hello.c         # その場でコンパイルして動かす（2 回目からはキャッシュ）
Hello, Monocs!
/root> gpio readall        # 40 ピンの GPIO の様子
```

## 特徴

- **ソースのまま動かす**: OS の中にコンパイラ（自作の slimcc-a64、C23）とリンカが入っていて、C のソースをメモリの上でコンパイルして動かします。付いてくるコマンドもサービスも C のソースのままです。
- **セルフホスト**: Monocs の上で、自分の C ライブラリとカーネルを作り直せます（Linux 上のビルドと 1 バイトも違わない）。
- **速さが命**: 電源からシェルまで約 7 秒。小さなプログラムは 0.1 秒前後でコンパイルできます。
- **シンプルな作り**: 同時に動くアプリは 1 つ、アドレス空間と特権も 1 つ（名前は Mono + Computing System から）。システムコールはなく、アプリは OS の関数を直接呼びます。4 つのコアのうち 1 つは裏で動くサービス用で、アプリはほかのコアを独り占めできます。
- **Linux に合わせた使い勝手**: bash に近いシェル、Linux と同じ C ライブラリ・端末・ソケット・pthread・OpenMP。Lua・SQLite・vim・QuickJS をソースのまま動かしています。
- **つないで使う**: USB（シリアル・イーサネット）、内蔵 Wi-Fi（WPA2）、SSH（sftp・scp・sshfs）、telnet、HTTP・HTTPS（curl）。
- **電子工作**: GPIO・I2C・SPI・ソフトウェアの UART・PWM のコマンドとライブラリ。

## 動かす

要るもの: Raspberry Pi Zero 2 W、microSD カード、開発用の Raspberry Pi 5（Raspberry Pi OS 64 ビット）。

```sh
git clone --recursive https://github.com/wildflare/monocs
cd monocs
sudo apt install build-essential gdb qemu-system-arm qemu-user picocom mtools
make             # カーネル build/kslim/kernel8.img（自作のコンパイラで作る）
make qemu        # まず QEMU（raspi3b）で試す
make sdimage     # SD カードのイメージ build/sdcard.img
```

`build/sdcard.img` を SD カードに書き込み、Zero 2 W の「USB」端子を PC につなぐと、USB シリアルと USB イーサネット（192.168.7.2）で入れます。
詳しくは [マニュアルの第 2 章](docs/manual/02-setup.md) と [第 3 章](docs/manual/03-connect.md)。

## 文書

| 文書 | 中身 |
|---|---|
| [docs/manual/](docs/manual/README.md) | リファレンスマニュアル（使い方と仕組みの全体。まずここから） |
| [docs/programming-guide.md](docs/programming-guide.md) | Monocs でプログラムを書くための手引き |
| [docs/resume.md](docs/resume.md) | 開発を再開するときの手引き |
| [CLAUDE.md](CLAUDE.md) | 開発の経緯・測った数字・不具合の記録 |

## ライセンス

Monocs 自身は [MIT ライセンス](LICENSE)（Copyright (c) 2026 asamizu）です。他のプロジェクトから来たもの（slimcc、picolibc、FatFs、lwIP、BearSSL、Wi-Fi のファームウェア）は、それぞれのライセンスに従います（[THIRD_PARTY.md](THIRD_PARTY.md)）。

コンパイラ slimcc-a64 は [slimcc](https://github.com/fuhsnn/slimcc)（MIT）をもとに、AArch64 向けに作り直したものです。

## 作者

asamizu（GitHub: [wildflare](https://github.com/wildflare)）。Claude（Anthropic）と一緒に作っています。
