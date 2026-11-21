# 14. 付録

## 14.1 上限の数

| もの | 上限 |
|---|---|
| アプリが使えるヒープ | 約 400MB（カーネル用と分け合う） |
| アプリのスタック | 8MB（コアごと） |
| 同時に動くアプリ | 1（`system()` の入れ子は 8 段まで） |
| アプリが使えるコア | 3（サービスがあるとき。pthread のスレッドは 2 つまで） |
| 同時に開けるファイル（ソケット・パイプを含む） | 253（fd 3〜255） |
| 同時に開けるフォルダ | 32 |
| Linux のソケット | 32 |
| TCP の接続（サービスの分も） | 16。接続ごとに受信 64KB |
| UDP のソケット | 8 |
| ssh の接続 | 3（対話のシェルは 1） |
| telnet の接続 | 1 |
| ファイルのキャッシュ | 1 ファイル 4MB まで、合わせて 64MB |
| 書き込みのため | 4MB まで（約 1 秒で書き出す） |
| シェルの 1 行（続きの行を含む） | 8192 文字 |
| シェルの語の数 | 65,536 |
| `{a,b}` の展開 | 1 つの語から 1,024 個 |
| 履歴 | 200 行 |
| カーネルの記録 | 64KB の輪（SD カードの /var/log/dmesg は 1MB で書き直す） |
| pthread のキー | 64 |

## 14.2 ないもの

| 分野 | ないもの |
|---|---|
| プロセス | `fork`・`exec`・`dup`・`dup2`（関数はあるが ENOSYS を返す）、`&`、ほかのプログラムとのパイプの同時実行 |
| 言語 | C++、`long double` の一部、複素数（`_Complex`）、アプリで定義する `_Thread_local` の変数、`__builtin_bswap`（`<endian.h>` の `be32toh` など） |
| ライブラリ | `dlopen`（いつも NULL）、`iconv`、ncurses の `newwin`・スクロール |
| ネットワーク | IPv6、ループバック、`sendmsg`・`recvmsg`、マルチキャスト、TLS 1.3・HTTP/2、mDNS |
| SSH | Ed25519 の鍵、ポートの転送 |
| 機械 | HDMI の画面、USB キーボード、Bluetooth、音、GPIO の割り込み |
| ファイル | シンボリックリンク、パーミッション、ext4 など FAT32 以外 |

## 14.3 用語

| 用語 | 意味 |
|---|---|
| アプリ | シェルから動かすプログラム（コア 0 で動く。同時に 1 つ） |
| サービス | 裏で動き続けるプログラム（コア 3） |
| OS API の表 | アプリが呼べる OS の関数の一覧（`os/api/api.def`）。名前で番地を引いてつなぐ |
| コンパイルのキャッシュ | コンパイルした結果を `/var/cache/slimcc` に覚えたもの。2 回目から速い |
| セルフホスト | Monocs の上で Monocs 自身（C ライブラリとカーネル）を作り直すこと |
| 標準のカーネル・slimcc 版 | 自作のコンパイラで作ったカーネル（`build/kslim/kernel8.img`）。ふだん使う |
| GCC 版 | 比べるために GCC で作ったカーネル（`build/kernel8.img`） |
| 試しの起動（tryboot） | 新しいカーネルを 1 回だけ試し、確かめられなければ前に戻す仕組み |
| ウォッチドッグ | 固まったら機械をリセットする時計 |
| コンソールの写し | シェルの表示を telnet・ssh のサービスに渡す仕組み |
| EL1 | ARM の特権の段（OS の段）。Monocs はアプリもここで動く |
| SDHOST・EMMC | SD カードのコントローラ 2 つ。SD カードは SDHOST、EMMC は Wi-Fi のチップに使う |
| lwIP | TCP/IP の部品（third_party） |
| picolibc | C ライブラリ（third_party） |
| slimcc-a64 | 自作のコンパイラ（`cc/`） |
| slimld | 自作のリンカ |
| nos.py | Pi 5 から実機を操作する道具 |

## 14.4 関連する文書

| 文書 | 中身 |
|---|---|
| `docs/programming-guide.md` | Monocs でプログラムを書くための詳しい手引き |
| `docs/hardware-test.md` | 実機を最初に確かめたときの手順（LED、配線） |
| `docs/resume.md` | 開発を再開するときの手引き |
| `docs/slimcc-codegen.md`・`slimcc-compile-speed.md` | コンパイラを良くする作業 |
| `docs/memcpy.md` | memcpy の実装のメモ |
| `tools/test/README.md` | 試験の一覧 |
| `CLAUDE.md` | 開発の経緯・測った数字・不具合の全部の記録 |
| `cc/UPSTREAM.md` | コンパイラの元にした slimcc |
| `third_party/firmware/README.md` | Wi-Fi のファームウェアのライセンス |

## 14.5 ライセンス

- **Monocs 自身は MIT ライセンス**です（リポジトリの一番上の `LICENSE`、Copyright (c) 2026 asamizu）。
  著作権の表示とライセンスの文を残せば、誰でも自由に使う・直す・配ることができます。保証はありません。
- 他のプロジェクトから来たものは、それぞれのライセンスに従います（一覧は `THIRD_PARTY.md`）:
  slimcc（MIT。`cc/` は slimcc を書き換えたもので、`cc/LICENSE` の作者の表示を残す）、picolibc（BSD 系）、
  FatFs（BSD 1 条項相当）、lwIP（BSD 3 条項）、BearSSL（MIT）、Wi-Fi のファームウェア（Synaptics・Cypress のバイナリの
  再配布の許諾。変更・逆アセンブルは禁止）、Raspberry Pi のファームウェア（Broadcom の再配布の許諾）。
- カーネルには slimcc・picolibc・FatFs・lwIP が入っているので、カーネルや SD カードのイメージを配るときは、
  これらのライセンスの文も一緒に配ります。SD カードのイメージには `/usr/share/licenses/名前/` に入れてあります（`tools/mksd.sh`）。
