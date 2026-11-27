# CLAUDE.md — Monocs（Raspberry Pi Zero 2 W の自作 OS）開発指示書

## プロジェクト概要

Raspberry Pi Zero 2 W 向けのベアメタル自作OS「Monocs」（Mono + Computing System。単一の・シンプルな計算システム: 同時に動くプログラムは 1 つ、アドレス空間と特権も 1 つの、シンプルな作り。ユーザーと決めた、2026-11-21。文書・表示は頭だけ大文字の Monocs、API は monocs_*）。
- 名前の移り変わり: new_os（〜2026-11-07）→ SeedOS（〜11-12）→ Unux（〜11-21）→ Hibana（2026-11-21。火花の意味）→ Monocs（2026-11-21〜。ユーザーの希望。Unux は UNIX の商標に 1 文字違いで近く、検索でも Unix と取り違えられるため。SeedOS は Raspberry Pi Zero で動く SeedSigner OS とまぎらわしいため。Monocs は調べた範囲でほかのソフト・OS になかった。似た名前の自作 OS に MonaOS・MonkOS がある）。
- 2026-11-21 に、ソース・コメント・文書・ファイル名・API（`monocs_*`、`monocs.h`、`__MONOCS__`、`cc/platform/monocs.c`、`slimcc-monocs`）・USB の機器名（`/dev/serial/by-id/usb-Monocs_Monocs_console-if00`）・画面の表示をすべて Monocs にした（同じ日の朝に Unux から Hibana にしたものを、さらに置き換えた）。git の履歴のコミットのメッセージは前の名前のまま。
- リポジトリのディレクトリは 2026-11-21 に ~/dev/c/unux → ~/dev/c/hibana → ~/dev/c/monocs に移した（そのたびに picolibc を作り直す。下の「フォルダを移したら」）。前の名前のまま残っているもの: 実機の SD カードのボリュームの名前（NEW_OS。新しく作るイメージは MONOCS）だけ。Pi 5 の設定（NetworkManager の接続 new_os-usb → hibana-usb → monocs-usb、chrony の conf.d/seedos.conf → hibana.conf → monocs.conf、ssh の鍵 ~/.ssh/id_ecdsa_monocs）と、実機の vim の configure が作ったファイル（auto/config.mk・pathdef.c の「Compiled by」）も直した。
- 実機も 2026-11-21 に移した（カーネル、/usr/include・/usr/lib・/usr/bin・/usr/sbin・/usr/share/samples・/usr/src/kernel、ssh のホスト鍵 /etc/ssh/monocs_host_key、.monocs-make。前の名前のファイルは消した）。/etc/htdigest は realm に名前が入る（ユーザー名:Monocs:MD5(…)）ので、名前を変えたら実機で `passwd` を打ち直す。
- コンパイラとリンカの名前（slimcc・slimld）は元の slimcc から来たものなので残す（シェルのコマンドは cc・ld）。

- 電源投入後すぐにシェルのプロンプトが出て、その場で C を書き、コンパイルし、メモリ上で実行できる専用機を目指す。
- 使い方の位置づけ（ユーザーの方針、2026-11-13）: **サブ OS**。PC や Pi 5 から USB・Wi-Fi でつないで使う（シリアル、telnet、SSH、HTTP）。単体で使うための HDMI の画面とキーボードは、いちばん最後に回す。機能を選ぶときは「つないで使うのが便利になるか」を優先する。
- 最終目標は、自作OS上で自作コンパイラ（slimcc-a64）が自分自身とOSをビルドするセルフホスト（2026-11-05、QEMU と実機で達成。下記「セルフホスト」）。
- 開発マシンは Raspberry Pi 5（Raspberry Pi OS 64ビット）。OS が動くのは Zero 2 W と QEMU `raspi3b` のみ。

## 再開するとき（2026-11-14 時点の状態。Linux との互換まで）

- **slimcc の出すコードを良くする続き**（ユーザーの希望、2026-11-16）は `docs/slimcc-codegen.md`（今の位置・測り方・確かめ方・候補・見送ったもの）から。2026-11-27 の続きは、その文書の「0. 再開するとき」（今の数字、GCC 版と関数ごとに比べる探し方、次の候補 = switch の表引き、学んだこと）。
- **slimcc のコンパイルを速くする**（ユーザーの希望「速くしたい」、2026-11-21）は `docs/slimcc-compile-speed.md`（決めたこと: ゼロから作り直さず後ろの半分を段階的に作り替える。今の時間の内訳、段階 1・2、確かめ方）から。段階 2（中間表現を配列と番号の形に作り替える）は始める前に確認する。
- 使い方と仕組みの全体（初めて使う人向けのリファレンスマニュアル）は `docs/manual/`（2026-11-21。ユーザーの希望。OS の仕様・使い方を変えたら、マニュアルも直す）。
- サンドボックス（人が作った C のコードを安全に動かす）は `docs/sandbox.md`（2026-11-22 に設計を決めた。まだ作らない。作る前にユーザーにもう一度見てもらう）。
- **まず `docs/resume.md`（再開の手引き）を読む**: 今できること、つなぎ方、よく使うコマンド、確かめ方、気をつけること、次の候補を 1 つにまとめてある。この CLAUDE.md は、経緯・測った数字・不具合の記録を含む詳しい版。

- ブランチ（2026-11-21。GitHub に公開するため、ユーザーと合意）: 作業は **main**（履歴のないブランチから、2026-11-21 の中身を 1 つのコミットにして始めた。公開するのはこれだけ）。それまでの全部の履歴は手元のブランチ **archive**（と build/sd-backup/monocs-archive-20261121.bundle）に残す。**archive は push しない**（`git push --all` もしない）。コミットの作者のアドレスは GitHub の非公開のアドレス（このリポジトリの git config）。main のファイル・コミットのメッセージは公開されるので、家のネットワークの情報（ルーターの SSID、実機の MAC アドレス）などの個人の情報を書かない。
- 作業は main にすべてコミット済み。リモートは origin = git@github.com:wildflare/monocs.git（2026-11-21 から。Pi 5 の鍵 ~/.ssh/id_ed25519_github）。push するのは main だけ。実機（Zero 2 W）では標準の slimcc 版カーネル（2026-11-16 から。実機の上で作り直したものと同じ）が動き、SD カードに /root/apps/mi・fd・lua（自分のアプリのソース）、/usr/bin・/usr/sbin（付いてくるコマンドとサービスのソース）、/usr/src/kernel（セルフホストのビルドの材料。古いことがあるので、使う前に `nos.py pushdir build/selfsrc /usr/src/kernel`）がある。フォルダの構成は下の「フォルダの構成（2026-11-19）」。
- つなぎ方: Zero 2 W の「USB」端子（内側）を Pi 5 に（電源もここから。PWR IN は空ける）。UART（GPIO14/15、`/dev/ttyAMA0`）もつないだまま。詳しくは docs/hardware-test.md の冒頭。Wi-Fi だけでも開発できる（2026-11-16。`export NOS_HOST=192.168.3.96` で nos.py が ssh を使う。docs/resume.md の 3、下の「Wi-Fi だけで開発する」）。
- よく使うもの:
  - `make`（標準のカーネル = slimcc 版 build/kslim/kernel8.img。2026-11-16 から）、`make kernel-gcc`（比べる用の GCC 版 build/kernel8.img）、`make sdimage`、`make cc`
  - `python3 tools/uart/nos.py kernel`（標準のカーネルを実機に送って再起動。GCC 版は `nos.py kernel build/kernel8.img`）、`nos.py cmd 'ls /'`、`nos.py push`・`pushdir`・`run`
  - `sh tools/test/verify.sh [quick]`、`sh tools/test/hwcheck.sh`、`sh tools/apps/push.sh DIR`（Pi 5 のプログラムのフォルダを /apps に送る）、`sh tools/apps/mi.sh`（mi を送り直す）
  - nos.py は、実機が使われている（アプリが動いている・telnet・ssh でつながっている。httpd の /.status）と打ち込まずに止まる。続けるなら `--force`（NOS_FORCE=1）
  - 実機で速さを調べる: シェルの `perf コマンド`（キャッシュのミスなどの数）、`perf record [-e cycles] [-g] コマンド`（どの関数か）
  - verify.sh の cc-test で `test/macro.c の .o が一致しない` が出ることがある（`__TIME__` を使うので、2 回のコンパイルの間に秒が変わると違う）。もう一度流して通れば問題ない
- 実機を壊したとき: 動かないカーネルを送ったら、SD カードを PC に差して boot/kernel8.img を戻す（nos.py kernel は前のものを残さない。必要なら先に `nos.py cmd 'cp /boot/kernel8.img /work/kernel8.old'` などで残す）。SD カードの FAT が壊れたら「SD カードの読み書きをまとめる」の診断と修復。SDHOST に移る前に動いていた EMMC のカーネル（kernel8.good）は、Pi 5 の build/sd-backup/root-20261113/ に控えて、実機からは消した（2026-11-13）。SD カードのドライバを疑っているときは、そのドライバの上でカーネルをコピーしない（2026-11-12 に、壊れたドライバの上で kernel8.old を kernel8.img にコピーして、起動できなくなった）。取り出した区画は Pi 5 の build/sd-backup/ に残してある。
- コミットのメッセージは日本語で、最後に `Co-Authored-By: Claude <noreply@anthropic.com>` の形の行を付ける（使っているモデルの名前に合わせる）。
- Monocs の上で動くプログラムを書くとき（人・AI）は、まず docs/programming-guide.md を読む（使える関数、入力のクセ、メモリ、ファイル、マルチコア、ネットワーク、サービス、落とし穴）。OS の仕様を変えたら、手引きも直す。
- 次の候補（ユーザーと決めていないもの。始める前に相談する）:
  - （おすすめ。2026-11-14）Linux のプログラムを動かす続き: 実際に使いたい Linux のプログラムを Monocs に置いて動かし、足りないものが出たら足す（ユーザーの方針「Linux で作ったコードをそのまま動かす」。今あるもの・ないものは docs/resume.md の 9 と、下のロードマップの「Linux との互換」）
  - （済み、2026-11-13: 内蔵 Wi-Fi と curl で、HTTP・HTTPS とも取れる。下の案はやめた）インターネットから取る: Pi 5 を中継役（HTTPS を代わりに取る小さなプロキシ）にする案がおすすめ（2026-11-11 に相談。Monocs に TLS を入れるのは大きい）
  - 常駐型のドライバの仕組み（/drivers。下の「ドライバ」。最初の常駐型（音かディスプレイ）を作るときに。センサー・GPIO の部品は /lib のライブラリで作れる）
  - HDMI での画面出力、HTTP・telnet 以外のサービス、システムを守るの続き（アプリを EL0 にするのは基本設計の変更なので必要になってから）
  - 見送った（2026-11-12）: SD カードへの書き出しを読み出しの途中でしない（ビルドのロックの待ちの残りはこれ。電源が切れたときに失われる量が増えるので、ユーザーと合意して見送り。「SD カードはこれぐらいが限界」）

## コミュニケーション

- 報告・説明は日本語の です・ます調で、簡潔に書く。
- 大きな設計変更や、third_party の追加・差し替えは、実施前に確認を取る。
- 実機（Zero 2 W）での動作確認は人間が行う。実機向けの手順が必要なときは、手順を書いて依頼する。

## ターゲット

| 項目 | 内容 |
|---|---|
| SoC | RP3A0（BCM2710 / BCM2837 相当、Pi 3 と同系統） |
| CPU | Cortex-A53 ×4、AArch64、**ARMv8.0-A（LSE アトミック命令なし）** |
| RAM | 512MB |
| 周辺機器ベース | `0x3F000000`（GPIO `0x3F200000`、PL011 UART `0x3F201000`、System Timer `0x3F003000`、Mailbox `0x3F00B880`） |
| ARM ローカル周辺 | `0x40000000`（コアごとのタイマー・割り込み） |
| カーネルの読み込み先 | `0x80000`（`kernel8.img`） |
| コンソール | PL011 UART（GPIO14/15、3Mbps。2026-11-06 までは 115200bps）。Pi 5 の GPIO14/15 と直結（TX/RX 交差、GND 共通）。USB シリアル（CDC-ACM、2026-11-06〜）も同時に使える |

## 基本設計

- **単一アドレス空間・単一特権**：OS とアプリはすべて EL1 で動く。MMU は恒等マップ（キャッシュを有効にするため）。
- **シングルタスク**：同時に動くプログラムは 1 つ。マルチコアは、アプリがコア 1〜3 に関数を任せる形で使う（2026-11-06 から。下記「マルチコア」）。
- **サービスはコア 3**：裏で動き続けるもの（HTTP サーバーなど）はカーネルの外のプログラム（サービス）にして、コア 3 で動かす。アプリ用のコアとサービス用のコアを分け、アプリはコアを独り占めでき、割り込まれない（非対称マルチプロセッシング。ユーザーと合意、2026-11-06。スレッドとスケジューラは入れない）。
- **OS API は関数表**：コンパイラに「名前 → 関数アドレス」の表を渡し、アプリから直接呼ばせる（RISC OS の SWI に相当）。
- **Monocs の上でコンパイルして、実行ファイルで動かす**（2026-11-17 から。ユーザーの方針「Linux と同じ方が多くの人に理解されやすい。そうでないと初めて使う人は毎回このシステムのことを学習しないとならない」）：`cc -o`・`make` で実行ファイルを作り、`./prog`・PATH の名前で動かす（Linux と同じ）。外部のアセンブラ・リンカは使わない（コンパイラの中で機械語まで作る）。実行ファイルは Monocs 独自の形（つなぐ前のコンパイルの結果。動かすたびに OS の関数とつなぐので、カーネルを入れ替えても作り直さなくてよい）。2026-11-16 までの「実行ファイルを持たず、ソースのまま動かす（run とコンパイルのキャッシュ）」はやめた（下の「実行ファイル」）。
- **異常時の復帰**：アプリで同期例外が起きたら、例外ハンドラで内容を表示してシェルに戻る。
- **速さが命**（ユーザーの方針、2026-11-13）: Monocs は「電源を入れたらすぐ C を書いて、その場でコンパイルして動かす」専用機なので、速さ（起動、コンパイル、アプリの起動、計算）をいちばん大事にする。
  - 機能を足すときは、速さを落とさないことを確かめる。コンパイル・起動・アプリの実行の道筋に手を入れたら、前後で実機の速さを比べる（セルフホストのビルド、`run /samples/primes.c`・`primes_mc.c` など）。遅くなるなら、その分を取り返す方法を考えるか、ユーザーに相談する。
  - 速さを測るときは、`clock` の measured が 1000MHz であること（起動から 60 秒の内は最大で動き、条件がそろわない）と、SD カードの書き込みの揺れ（同じ量で 2〜3 倍）を考えて、条件を交互に・何回か測る。1 回の比べもので「遅くなった・速くなった」と決めない（2026-11-13 に、CPU のクロックの違いを Wi-Fi・SD カードのドライバのせいと見誤りかけた）。
  - 速さと引き換えにするもの（消費電力など）は、ユーザーと相談して決める（例: force_turbo=1 で CPU を常に 1000MHz に）。
- **Monocs の中で完結**（ユーザーの方針、2026-11-16）: 標準のカーネルは、自作のコンパイラ（slimcc-a64）とリンカ（slimld）で作った slimcc 版。Monocs の上で作り直して、そのまま起動できる。GCC 版は比べるためだけに残す（GCC 版のほうが約 1.5 倍速いが、方針として slimcc 版を使う。差は slimcc の出すコードを良くして縮める）。
- **Linux に合わせる**（ユーザーと合意、2026-11-12）：使い勝手（C ライブラリ・POSIX の動き、端末、シェル、コマンドの出力）は、基本的に Linux（GNU・POSIX）に合わせる。Linux 用に書かれたプログラムがソースのまま動き、人も AI も知っている書き方で正しく書けるように。合わせないのは Monocs の設計そのもの（プロセスがない、OS とアプリが同じ特権・同じアドレス空間）だけ（実行ファイルも 2026-11-17 から Linux と同じ使い方）。
- **ドライバ**（ユーザーと合意、2026-11-07）：カーネルに入れるのは、起動に必要なもの（UART、USB、SD、タイマー・割り込み、メールボックス、LED）だけ。起動に関係ないドライバ（I2C・SPI のセンサー、GPIO の部品、音など）はカーネルに入れず、SD カードの専用のフォルダ（案: `/drivers` の C ファイル、起動時に読み込むものは `/drivers.txt`）に置く。ドライバの関数をアプリから OS の関数と同じように呼べるように、名前を解決する表に足す仕組みを、最初のそういうドライバを作るときに一緒に作る（裏で動き続けるものはサービスの仕組みを使う）。
  - 2 種類に分ける（ユーザーと合意、2026-11-13）:
    - **ライブラリ型**（I2C・SPI のセンサー、ボタン・LED など GPIO の部品、描くたびに送るだけの小さなディスプレイ）: 呼ばれたときにレジスタを読み書きするだけで、常駐しなくてよいもの。共有のライブラリ `/lib/名前`（`-l名前`）として作る。今の仕組みのままで作れる（カーネルの変更は要らない）。使うアプリだけが負担し、起動は遅くならない。
    - **常駐型**（音、HDMI の画面、割り込みで受けるもの、アプリをまたいで状態を持つもの）: `/drivers/名前`。起動時に常駐させるものは `/etc/drivers.txt`（ルートを整理したので、前の案の /drivers.txt から変えた）。仕組み（起動時のコンパイルと常駐、OS API の表に関数を足す、割り込みの登録の口。今は irq.c に書き込んである）は、最初の常駐型（音かディスプレイ）を作るときに一緒に作る。使わない仕組みを先に作らない。
    - `-l名前` が /drivers も探す形（ハードウェアのものを全部 /drivers に置く）も考えたが、常駐するかどうかが置き場所でわかるように、分けておく。
    - 今カーネルにあるドライバ（UART・USB・SD・タイマー・メールボックス・LED、例外として Wi-Fi）は、このまま。

## ディレクトリ構成

```
.
├── CLAUDE.md
├── Makefile
├── os/
│   ├── boot/        boot.S、linker.ld
│   ├── kernel/      例外、MMU、メモリ管理、OS API 表
│   ├── drivers/     uart、timer、irq、mailbox、sd（sdhost・sd_emmc・sd_test）、usb、sdio・wifi（内蔵 Wi-Fi）
│   ├── fs/          VFS、FatFs のつなぎ込み
│   ├── net/         ネットワーク（USB イーサネットの上の IPv4。lwIP、TCP・UDP の API）
│   ├── libc/        picolibc のつなぎ込み（syscall スタブ、POSIX の補い posix.c、wcwidth.c、malloc.c、ldtoa.c）
│   ├── curses/      curses（ncurses 互換の小さなもの。ヘッダーは api/include/curses.h）
│   ├── slimcc/      slimcc-a64 のつなぎ込み（実行 run.c、コマンド cmd.c: シェルの cc・ld）
│   ├── api/         OS API 表（api.def、api.c）、アプリ用ヘッダー include/monocs.h・include/curses.h
│   └── shell/       シェル（行の編集 lineedit.c、性能カウンタの perf.c、wifi コマンド wifi_cmd.c）
├── cc/              slimcc-a64（自作コンパイラ。Linux 上で開発。元は third_party/slimcc、cc/UPSTREAM.md 参照。中間表現のコード生成は ir.inc（ターゲットに依存しない）と ir_a64.inc・ir_a64_emit.inc（AArch64）。ターゲットに依存するところの一覧は docs/slimcc-codegen.md の 0.1）
├── lib/             SD カードの /lib に入れる共有のライブラリ（gpio: GPIO（自作）。i2c: I2C のマスター（自作）。spi: SPI のマスター（自作）。softuart: ソフトウェアの UART（自作）。pwm: ハードウェアの PWM（自作）。inflate: gzip・zlib の展開（自作）。tls: TLS でつなぐ薄い層。bearssl: BearSSL 用の compile_flags.txt と、入れるファイルの一覧）
├── third_party/     picolibc、FatFs、slimcc、lwIP、bearssl（TLS。/lib/bearssl に入れる）、firmware（Wi-Fi のファームウェア）（原則として無改造。変更はパッチで管理）
│   └── patches/     third_party に当てるパッチ（fatfs/ は作者の公式パッチ）
├── samples/         SD カードの /samples に入れるサンプルプログラム
├── bin/             SD カードの /bin に入れる、Monocs に付いてくるアプリ（名前だけで動く。ls、cat、cp、mv、rm、mkdir、less、grep、host、passwd、curl、tar、gunzip、zcat、pinctrl、i2cdetect、i2cget、i2cset、spi、softuart、pwm など。フォルダのものは共有のライブラリを使う）
├── services/        SD カードの /services に入れるサービス（裏で動くプログラム。httpd.c、telnetd.c、sntp.c、wifid.c、sshd/）と、起動時に始める一覧 services.txt
├── tools/           補助スクリプト（slimcc/: slimcc の出すコードの比べと実機での速さ（cmp_s.sh・cmp_o.sh・devbench.sh。docs/slimcc-codegen.md）、ssh/: SSH のサーバーを Linux で動かして本物の ssh で試す、tls/: /lib に入れるライブラリを集める・Linux で試す、inflate/: 展開と tar のテスト、perf/mksym.py: perf record のカーネルの関数名、mksd.sh: 実機用 SD カードの中身、mkimg.sh: SD カードイメージ、selfhost/: セルフホスト、smp/: マルチコアの実機確認、uart/nos.py: USB・UART での操作と更新、test/: 検証一式と端末の再現（README.md）、apps/push.sh: プログラムのフォルダを実機の /apps に送る、apps/mi.sh: 端末のエディタ mi を実機に送る、pi5/: Pi 5 の準備（setup-usbnet.sh: USB イーサネットのアドレス、setup-chrony.sh: 時刻の配り手）、gen_wcwidth.py: 文字の幅の表、gen_wctype.c: 文字の分類の表（glibc から）、make/: Makefile の解釈器の試験、libc/: glob・wctype を glibc と比べる試験、test/linuxapi・netapi・sockets/: Linux との互換の試験）
├── docs/            リファレンスマニュアル manual/（使い方と仕組みの全体）、再開の手引き resume.md（まずこれを読む）、slimcc の出すコードを良くする続き slimcc-codegen.md、手順書（programming-guide.md: Monocs でプログラムを書くための手引き（クセ・注意点）、hardware-test.md: 実機での動作確認）、メモ（memcpy.md: memcpy の実装と FEAT_MOPS）
└── build/           生成物（git 管理外）
```

## ビルドと実行

### 必要なパッケージ（Pi 5）

```sh
sudo apt install build-essential gdb qemu-system-arm qemu-user
gcc --version   # GCC 14 以降なら -std=c23 が使える。13 以前なら -std=c2x で代用する
```

### コンパイルオプション（カーネル）

```
-std=c23 -mcpu=cortex-a53 -ffreestanding -nostdlib -nostartfiles
-mstrict-align -O2 -g -Wall -Wextra
```

- **`-mcpu=cortex-a53` を必ず指定する**。`-march=native` は禁止（Pi 5 の Cortex-A76 にしかない命令が混入する）。
- **`-mstrict-align`**：MMU を有効にする前は全メモリがデバイスメモリ扱いになり、非整列アクセスで例外になるため。
- 割り込み・例外ハンドラの経路にある C コードは `-mgeneral-regs-only` でビルドするか、ベクタで FP/SIMD レジスタを退避する。
- リンクは `ld -T os/boot/linker.ld`、イメージは `objcopy -O binary` で `build/kernel8.img` を作る。

### Make ターゲット

| コマンド | 内容 |
|---|---|
| `make` | `build/kernel8.img` をビルド |
| `make qemu` | `qemu-system-aarch64 -M raspi3b -kernel build/kernel8.img -serial stdio -display none -drive file=build/sdcard.img,if=sd,format=raw` |
| `make debug` | 上記に `-s -S` を付けて起動し、`gdb build/kernel.elf` で `target remote :1234` |
| `make clean` | カーネルの生成物を削除（picolibc のビルド結果は残す） |
| `make distclean` | `build/` を削除 |
| `make sdcard` | 実機用の SD カードの中身を `build/sdcard/` に集める（手順は `docs/hardware-test.md`） |
| `make sdimage` | SD カードイメージ `build/sdcard.img`（64MB、FAT32）を作る。QEMU の SD カードになり、実機用に Rufus などで書き込むこともできる |
| `make sdimage-self` | セルフホスト用のイメージ `build/sdcard-self.img`（/src にカーネルのソース一式、picolibc のソースとヘッダー、`libc.a`、ビルドスクリプト `build.c`・`buildlibc.c`） |
| `make sdimage-self-auto` | 上に `/autorun.c`（`tools/selfhost/autorun.c`）を加え、起動時に自動でビルドして結果を LED で知らせるイメージ（実機の確認用） |
| `make sdimage-smp-auto` | 起動時に 4 コアで計算し、結果を LED で知らせるイメージ `build/sdcard-smp-auto.img`（`tools/smp/autorun.c`。実機でのマルチコアの確認用） |
| `make sdimage-blink` | `samples/blink.c` を起動時に自動実行するイメージ `build/sdcard-blink.img`（UART なしで、実機での slimcc-a64 の動作を LED で確認する） |
| `make cc` | slimcc-a64（`cc/slimcc`）をビルド |
| `make cc-test` | slimcc-a64 のテスト（`cc/test-a64/run.sh`。型のレイアウト、ヘッダー、構文解析、test/ の実行、Cortex-A53 での実行、GCC との呼び出し規約の互換性、2・3 世代目の一致、内蔵アセンブラと ELF の .o が GNU as と一致） |
| `make kernel-slimcc` | カーネルを slimcc-a64（`cc/slimcc-monocs`）でビルドし、自作のリンカ slimld で `build/kslim/kernel8.img` を作る |
| `make qemu-slimcc` | `build/kslim/kernel8.img` を QEMU で起動する |
| `make ld-test` | slimld と GNU ld の結果を比べる（GCC 版と slimcc 版のカーネル。`cc/test-a64/ld_test.sh`） |
| `make picolibc-slimcc` | picolibc を slimcc-a64 でビルドし、`build/picolibc-slimcc/libc.a` を作る（`tools/picolibc/build.py`。`make kernel-slimcc` が使う） |
| `make picolibc` | `third_party/picolibc` を meson でビルドし、`build/picolibc-install` に入れる |

### 実機用 `config.txt`（SD カードの FAT 領域）

実体は `os/boot/config.txt`。以下は最低限の内容。

```ini
arm_64bit=1
kernel=kernel8.img
enable_uart=1
dtoverlay=disable-bt
init_uart_clock=48000000
```

SD カードを差し替えずに試すときは、Pi 5 から `rpiboot` で USB 経由で起動する。

## 起動コード（boot.S）の要件

1. `MPIDR_EL1` を見て、コア 0 以外は `wfe` ループで待機させる。
2. `CurrentEL` を確認し、EL3 または EL2 から EL1 へ降りる（QEMU と実機で開始 EL が違う可能性があるため、どちらにも対応する）。
3. `CPACR_EL1.FPEN` を設定して FP/SIMD を有効にする（C コードより前に行う）。
4. スタックを設定し、BSS をゼロクリアしてから `kernel_main()` を呼ぶ。

## ロードマップ

### OS 本体
- [x] 1. QEMU `raspi3b` で起動し、UART に文字を出す
- [x] 2. 例外ベクタ、タイマー割り込み、MMU（恒等マップ）
- [x] 3. メモリ割り当て、picolibc の組み込み
- [x] 4. 実機（Zero 2 W）での起動確認（2026-11-06、UART で確認）
- [x] 5. SD カードのドライバ、FatFs、VFS
- [x] 6. シェルと libtcc の組み込み（ここで「その場で C が動く OS」が完成。2026-11-04 に slimcc-a64 へ置き換え）

### コンパイラ（cc/、並行して進める）
- [x] 1. slimcc のフロントエンドを AArch64 Linux 向けに調整（`__aarch64__`、AAPCS64 の `va_list`、`char` は unsigned、`long double` は当面未対応）
- [x] 2. AArch64 のコード生成（アセンブリテキストを出力し、Pi 5 の as/ld でリンク）
- [x] 3. slimcc のテスト（`test/`）をすべて通す
- [x] 4. Emitter を抽象化し、機械語を直接出力（テキスト出力を as でアセンブルした結果とバイト単位で比較する）
- [x] 5. ELF の .o 出力、セルフホスト（第 2 世代と第 3 世代のバイナリが一致すること）
- [x] 6. メモリ上での実行 API を作り、OS の libtcc を置き換える（tinycc は 2026-11-04 に削除）
- [x] 7. 中間表現（SSA）を使うコード生成に作り直し、既定にする（2026-11-06）
- [x] 8. インライン展開（2026-11-06）
- [x] 9. 命令の並べ替え（Cortex-A53 向け、ループの中のブロック。2026-11-15）
- [x] 10. ループの最適化（LICM・反転・2 回の展開。2026-11-15）

### マルチコア（ユーザーと合意、2026-11-06）
- [x] 1. コア 1〜3 を起こし、関数を動かす（`os/kernel/smp.c`）
- [x] 2. C ライブラリ（picolibc、FatFs）をスレッド安全にする（errno などのコアごとの状態は段階 3 の TLS で）
- [x] 3. セルフホストのビルドを並列にする（slimcc の状態をコアごとに持つ。2026-11-06）

### 端末とアプリ（2026-11-07）
- [x] 省電力: 待つ間はコア 0 を wfi で眠らせる（下記「省電力」）。クロックを下げるのは、ファームウェアが受け付けても実際には変わらなかったので見送り
- [x] 中間表現で扱える関数を増やす（goto のラベル、nullptr_t、アトミック操作）。long double は見送り
- [x] 端末のアプリ: curses（ncurses 互換）、複数ファイルの run、POSIX の補い、wcwidth。ユーザーの端末のエディタ mi がソースのまま動く（下記「端末のアプリ（curses）を動かす」）
- [x] 起動を速く: まとめてコンパイルするときの使い回し、コンパイルのキャッシュ（SD カード）
- [x] 名前を SeedOS にする（2026-11-12 に Unux、2026-11-21 に Hibana、同じ日に Monocs に変えた）
- [x] シェルの行の編集（カーソルの移動、履歴、Tab の補完）
- [x] /apps のアプリを名前だけで動かす（`mi ファイル`）
- [x] ls を GNU の ls に近づける。日時（date、nos.py が Pi 5 の日時を送る、ファイルの日時）、SNTP（Pi 5 の chrony）
- [x] シェル: cp・rm -r などの基本コマンド、リダイレクト、パイプ、&&・||・;、引用符。アプリ: less・grep・head・tail・wc
- [x] システムのアプリ（/bin）と自分のアプリ（/apps）を分ける。PATH、export、env、/.profile
- [x] ls・cat・cp などのファイルの操作をシェルの外（/bin のアプリ）に出す

### 守りと道具（2026-11-12）
- [x] カーネルのコードを読み出し専用に、スタックにガードページ（下記「システムを守る」）
- [x] アプリ用のヒープを分ける（アプリが終わったら丸ごと捨てる）。アプリの中のコンパイルは途中で止めない
- [x] 性能カウンタ（perf、perf record）。VFS のロックを分ける（キャッシュに当たるものは SD カードのロックを取らない）
- [x] 端末の設定（termios）を Linux と同じに（アプリの stdin は既定で 1 行ずつ・エコーあり。tcsetattr、ioctl(TIOCGWINSZ)）。方針「使い勝手は Linux に合わせる」
- [x] プログラムを書くための手引き（docs/programming-guide.md）
- [x] `run DIR` でサブフォルダの .c も（除くものは .runignore）。共有のライブラリ（/lib/名前、-l名前。ライブラリ自身の compile_flags.txt、ライブラリから別のライブラリ）

### Linux との互換（2026-11-14。ユーザーの方針: Linux で作ったコードを、ソースも Makefile も変えずに動かす）
- [x] system()（入れ子のアプリ）、アプリの chdir・環境変数はアプリの終わりに戻す、readdir の . と ..、curses の nl
- [x] Linux の Makefile をそのまま読む（`run DIR`、`DIR/プログラムの名前`）
- [x] getopt・setenv・sleep・glob・正規表現・ワイド文字（glibc と同じ分類の表、UTF-8 の入出力）など
- [x] picolibc をマルチバイトあり・ワイド文字の printf ありに（`%ls`。ロケールは Linux と同じ扱い）
- [x] シグナル（Ctrl-C で SIGINT の関数、alarm）、popen
- [x] ソケット・select・poll・getaddrinfo
- [x] pthread（2026-11-15。スレッド 1 つに空いたコアを 1 つ。スケジューラなし）
- [x] OpenMP（2026-11-15。-fopenmp。slimcc が parallel を関数に切り出す。ランタイムは pthread の上）
- ないもの: fork・exec（設計と合わない）、dup・dup2、ほかのプログラムへの pipe（同じプログラムの中の pipe は 2026-11-21 から）、IPv6、ループバック、sendmsg・recvmsg、`<endian.h>` の be32toh など（slimcc に __builtin_bswap がない）

### 拡張（後回し・見送り）
- HDMI での画面出力（ユーザーと合意して最後に回している）
- ESP32（ESP-AT）を UART でつなぐネットワークモジュール
- 見送り（ユーザーと合意）: long double を中間表現で扱う、printf の double の 17 桁・long double の 34 桁より先（picolibc のパッチが要る）、CPU のクロックを下げる、実行ファイルの形（キャッシュで足りている。ソースを渡さずに配りたくなったら、キャッシュの中身を 1 つのファイルにまとめる形で足せる）

## 作業ルール

- 1 ステップずつ進める。各ステップの終わりに、ビルドが通り `make qemu` で動作することを確認する。
- 確かめるときは `sh tools/test/verify.sh`（QEMU で検証一式。カーネルだけなら `quick`）と、実機の `sh tools/test/hwcheck.sh`（tools/test/README.md）。コンパイラ（cc/）を変えたら、`make cc-test` に加えて、カーネルとセルフホストの一致（verify.sh）も必ず確かめる。
- 実機のテストでシェルにコマンドを送るときは、前に空白を付ける（ユーザーのシェルの履歴に残さない）。
- 動作確認ができたら、ステップ単位でコミットする。
- C は C23 で書く。ただし Pi 5 の GCC で通らない機能（`#embed` など）は使わない。
- ハードウェアのレジスタアクセスは `volatile` を使った小さなアクセサ関数にまとめる。
- マジックナンバーにはレジスタ名の定数を付け、出典（BCM2837 ペリフェラル資料の章など）をコメントに残す。
- slimcc-a64 のテストは、Pi 5 でのネイティブ実行に加えて `qemu-aarch64 -cpu cortex-a53` でも実行し、A53 にない命令（LSE など）を出していないことを確認する。
- Pi 5 の Raspberry Pi OS は 16KB ページのカーネルなので、ページサイズを 4KB と決め打ちしたテストを書かない。
- third_party のライセンスを守る：slimcc（MIT）、picolibc（BSD 系）、FatFs（BSD 1 条項相当）、lwIP（BSD 3 条項）、BearSSL（MIT。LICENSE.txt も /lib/bearssl に入れる）、Wi-Fi のファームウェア（Synaptics・Cypress のバイナリの再配布の許諾。変更・逆アセンブルは禁止。third_party/firmware/README.md）。

## 注意事項

- SD カードが EMMC（Arasan、`0x3F300000`）と SDHOST（`0x3F202000`）のどちらにつながるかは GPIO48〜53 の機能設定で決まる（ALT3 で EMMC、ALT0 で SDHOST）。
  - 2026-11-13 から SD カードは SDHOST（`os/drivers/sdhost.c`。ポーリング、PIO。レジスタと手順は Linux の drivers/mmc/host/bcm2835.c による）。EMMC を内蔵 Wi-Fi の SDIO（GPIO34〜39）に空けるため（ユーザーと合意）。`os/drivers/sd.c` が選び、SDHOST の初期化に失敗したら前の EMMC のドライバ（`os/drivers/sd_emmc.c`）で動く（そのときは Wi-Fi を使えない。`mem` の `sd (SDHOST)`・`sd (EMMC)` でわかる）。`sd.c` の `SD_BOOT_SDHOST` を 0 にすると EMMC で起動する。
  - SDHOST の不具合で、実機の FAT を壊したことがある（2026-11-12。最初の版。fsck で直せない壊れ方で、取り出しておいた区画から書き戻した）。原因は、書き込みで各ブロックの終わり（SDHSTS_BLOCK）を待たずに次を渡していたこと。複数ブロックの書き込みの最後のブロックを送り終わる前に CMD12 を出し、そのブロックが書かれなかった。Linux と同じくブロックごとに待つ（最後だけ待つのはだめ。前のブロックの印が残っている）。ほかに Linux と同じく、FIFO に 8 語たまってから読み、転送の前後で FIFO に残った語を捨てる（どちらも、外しても壊れなかった）。
  - SDHOST のドライバを直すときは、ファイルシステムを使う前に `sdtest [N]`（`os/drivers/sd_test.c`）を通す: `SD_BOOT_SDHOST` を 0 にした（EMMC で起動する）カーネルで、試すときだけピンを SDHOST に切り替え、区画の外の空いているセクタ（区画の終わりから 32MB 先の 2MB。元の中身は書き戻す）を、いろいろなセクタ数・バッファの境界で読み書きして、EMMC でも読み直して比べる。VFS のロックを取ったまま行う（`vfs_exclusive`）。2 つ目の引数で直しを外せる（1: FIFO の 8 語、2: ブロックの終わりの待ち、4: 残った語を捨てる）。EMMC に戻すときは、ピンをいったん入力にしてから ALT3 にする（QEMU 10 の bcm2835_gpio は、入力のときにカードを EMMC につなぎ直す。実機は ALT3 だけでよい）。
  - 確認（2026-11-13、実機）: sdtest 30 回、SDHOST で起動してセルフホストのビルド（C ライブラリ・カーネルが Linux 上のビルドと一致）、その後の fsck.vfat、hwcheck（GCC 版・slimcc 版）、verify.sh quick。速さは EMMC と変わらない（C ライブラリのビルド SDHOST 36・35 秒、EMMC 36・40 秒。カードの書き込みの時間は、カードの中の状態で 2〜3 倍変わる）。
- 内蔵 Wi-Fi（CYW43436s）: 外に出ていくのに使える（接続・DHCP・DNS・NTP・起動時の自動の接続。2026-11-13。ユーザーと合意。段階: 1. SD カードを SDHOST に移す（済み）、2. EMMC で SDIO、チップの初期化とファームウェア（済み。下）、3. 接続（WPA2）と DHCP、4. telnet・HTTP の守り、5. DNS・NTP。Bluetooth は後で相談）。
  - SDIO（`os/drivers/sdio.c`）: EMMC を GPIO34〜39（ALT3。CLK はプルなし、ほかはプルアップ）でチップにつなぐ。WL_ON（GPIO41）を Low → High にしてチップをリセットし直し、CMD5・CMD3・CMD7、4 ビット、25MHz。CMD52・CMD53（バイトモード・ブロックモード）。
  - チップ（`os/drivers/wifi.c`。手順は Linux の brcmfmac（ISC）の sdio.c・bcmsdh.c・chip.c・firmware.c。コードは写さず手順だけ）: 機能 1 の 32KB の窓でバックプレーンを読み書きする。ALP のクロック → ChipCommon の番号（実機は 43430 の版 1 = CYW43436s）→ EROM でコアを数える（7 個）→ ARM（Cortex-M3）を止め、無線と SOCRAM をリセット（43430 はバンク 3 の付け替えを外す）→ RAM（512KB）の 0 番地にファームウェア、最後に nvram（brcmf_fw_nvram_strip と同じ形と長さの印）を書いて読み直して確かめる → ARM を動かす → HT のクロック → 機能 2 が使えるまで待つ。ファームウェアは RAM の最後の語を自分の情報（sdpcm_shared）の番地に書き換え、そこからログ（rte_console）を読める。
  - シェルの `wifi`（作っている途中の確認用）: 上の全部をして、ファームウェアのログを表示する。実機（2026-11-13、GCC 版・slimcc 版）で `RTE (SDIO-CDC) 7.45.96.s1 on BCM43430 r1`。ファームウェアの読み込みから動くまで 0.24 秒。
  - ファームウェアは `third_party/firmware`（Pi 5 の firmware-brcm80211 から無改造で写した。ライセンスは Synaptics・Cypress の、チップで使うためのバイナリの再配布の許諾。README.md）。SD カードの `/lib/firmware/brcm`（Linux と同じ場所。`tools/mksd.sh`。2026-11-13 までは `/firmware`）。
  - MAC アドレス: nvram の macaddr は全部の基板で同じ仮の値（00:90:4c:c5:12:38）だが、ファームウェアの `cur_etheraddr` は基板の固有の値（実機 e4:5f:01:xx:xx:xx。前の 3 バイトは Raspberry Pi の番号）を返すので、それを使う。
  - 段階 3 で決めたこと（2026-11-13、ユーザーと合意。telnet・HTTP は、段階 4 でパスワードを付けて Wi-Fi からも受けるようにした。下）: telnet・HTTP は USB からだけ受ける（`monocs_tcp_listen_addr("192.168.7.2", …)`。Wi-Fi は外に出ていくのに使う。Wi-Fi から受けたくなったら、段階 4 でパスワードを付けてから）。Wi-Fi の設定は Linux と同じ `/etc/wpa_supplicant.conf`（country、network={ ssid psk key_mgmt priority }。`os/drivers/wifi_conf.c`）。Wi-Fi のドライバはカーネルに入れる（ネットワークがカーネルにあり、その 2 つ目の口なので。「起動に関係ないドライバは /drivers」の方針の例外。USB も /drivers の仕組みができたら移すかを改めて決める。UART があるので USB が壊れても戻れる）。
  - 段階 3 で作ったもの（2026-11-13）: SDPCM の枠（機能 2、送ってよい番号）・BCDC の dcmd と iovar・イベント（escan、SET_SSID、LINK、PSK_SUP、DEAUTH・DISASSOC）・CLM（clmload）・国（country）・WLC_UP・スキャン（`wifi scan`。実機で 11 個、0.6 秒）・接続（ファームウェアの sup_wpa にパスフレーズを渡す。`wifi connect [SSID [PASS]]`、`wifi disconnect`、`wifi status`）・データ（チャンネル 2 と BCDC の頭。lwIP の 2 つ目の口 w0、DHCP、つながったら既定の経路を Wi-Fi に）。SDIO のバスのロック（`bus_lock`。タイマー割り込みの `wifi_net_poll`（10ms ごと）と lwIP の送信は空いているときだけ使い、送信は待ち行列に入れる）。lwIP は LWIP_SINGLE_NETIF 0、LWIP_DHCP 1（dhcp.c を足した）。
  - 段階 3 の確認（2026-11-13、実機、GCC 版・slimcc 版）: `wifi connect`（`/etc/wpa_supplicant.conf`）で接続 2.8 秒、DHCP 5〜9 秒でアドレス。Pi 5 から Wi-Fi のアドレスへ ping、Monocs から外（`run /samples/http_get.c 1.1.1.1 80 /`）の TCP、Wi-Fi のアドレスのポート 80・23・7 は断る、USB の HTTP・エコーは今までどおり、hwcheck、verify.sh quick。
    - 最初は `join failed (-11)`（E_SET_SSID の status 1）だった。wpa_auth に WPA と WPA2、wsec に TKIP と AES を両方立てていたため。brcmfmac と同じく、WPA2 なら WPA2_AUTH_PSK と AES だけにする（スキャンで RSN がなく WPA だけとわかったときは WPA_AUTH_PSK と TKIP・AES。`wifi_connect` の security）。失敗したときは、接続の間に届いたイベント（番号・status・reason）を表示する（`wifi_join_trace`）。
    - 確認用の TCP エコー（ポート 7）も USB のアドレスだけで受ける（`os/net/net.c`。前はどのアドレスでも受けていて、Wi-Fi から届いた）。
  - 自動の接続（2026-11-13。ユーザーと合意: 設定ファイルがあれば起動時に自動でつなぐ）: サービス `services/wifid.c`（/services.txt）がコア 3 から `monocs_wifi_auto_poll`（`os/drivers/wifi_auto.c`）を呼び続ける。設定にネットワークがあれば、チップを動かし、スキャンして、見えるものに priority・強さの順でつなぎ、DHCP を始める。切れたら 3 秒後、つながらなかった・設定がないときは 30 秒後にやり直す。画面には出さない（`wifi status` に状態と、うまくいかなかったときの結果・イベント）。
    - ほかのサービスを待たせないよう、スキャンと接続は「始める」と「調べる」に分けた（`wifi_scan_begin`・`wifi_scan_poll`、`wifi_connect_begin`・`wifi_connect_poll`。イベントはコア 0 のタイマー割り込みの `wifi_net_poll` が読む）。待つのは最初のファームウェアの読み込み（約 0.5 秒）だけ。
    - シェルの `wifi start・scan・connect・disconnect` とは `wifi_manage_lock` で重ならないようにし、シェルが操作したら自動のほうは最初からやり直す。`wifi disconnect` の後は `wifi connect` まで自動ではつながない。チップがない（QEMU）・設定がないときは何もしない。
    - 不具合（直した）: 自動の接続が、つながっているのに時間切れになることがあった。ファームウェアは、続けて送る枠を 1 つにまとめる（glom。SDPCM のチャンネル 3 の案内と superframe）ことがあり、ドライバはそれを読めずに ASSOC・LINK のイベントを落としていた（手動の接続は 1ms ごとに読むのでまとまらず、10ms ごとのタイマー割り込みで読むとまとまる）。`wifi_setup` で `bus:txglom` を 0 にして、まとめさせない（実機で、まとめた枠の数が 2 → 0。設定の名前は記憶によるもので、資料では確かめていない。効いたことは `wifi events` の glom の数で確かめた）。`wifi events` は、枠の数と最後の接続のイベントを表示する。
    - 確認（実機、GCC 版・slimcc 版）: 再起動から約 10 秒でつながる（5 回続けて成功）、hwcheck の後もつながっている、`wifi disconnect` の後はつなぎ直さない。アクセスポイントの側から切られたときのつなぎ直しも確認した（2026-11-13、ユーザーがルーターを再起動。Monocs は再起動せずにつなぎ直し、同じアドレス、DNS も引けた。切れている間に送ろうとしたフレームは捨てる: `wifi status` の dropped が 5,986）。
  - 段階 5（2026-11-13）: DNS と NTP。
    - DNS: lwIP の dns.c（LWIP_DNS 1。サーバーは Wi-Fi の DHCP から）。`os/net/dns_api.c`（`monocs_resolve`。答えを置く枠は静的で、世代が合うときだけ書く。待つ側が Ctrl-C でいなくなってもよい）。`monocs_tcp_connect` は名前も受ける（timeout まで待つ）。`monocs_udp_sendto` は名前を待たずに引く（引けるまで -1）。`/bin/host`（Linux の host と同じ形の出力）。MEMP_NUM_UDP_PCB を 16 に（DHCP・DNS の分）。
    - NTP: `services/sntp.c` はサーバーを 4 つまで受け、答えがなければ次に聞く。/services.txt は `192.168.7.1 pool.ntp.org`（Pi 5 がいれば Pi 5、いなければ Wi-Fi の先）。
    - 確認（実機）: `host example.com`、`run /samples/http_get.c example.com 80 /`（200）、pool.ntp.org だけにした sntp で時計が合う。verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版）。
  - 段階 4（2026-11-13。ユーザーと合意: Wi-Fi から HTTP と telnet をパスワード付きで受ける）:
    - USB から届いた接続は今までどおり認証なし、Wi-Fi から届いた接続は認証する。どちらから届いたかは、受け付けたときの口（`monocs_tcp_netif`。`os/net/tcp_api.c` の on_accept で `ip_current_input_netif`）で決める。アドレスでは決めない: lwIP は、ほかの口のアドレスあてのパケットも受け取るので、Wi-Fi の側から 192.168.7.2 あてに送れる（Pi 5 の経路を一時的に変えて確かめた）。そのため `monocs_tcp_listen_addr` とカーネルの TCP エコーは、アドレスに加えて口も決める（`tcp_bind_netif`）。
    - パスワード: `/etc/htdigest` の 1 行（Apache の htdigest と同じ形。`ユーザー名:Monocs:MD5(ユーザー名:Monocs:パスワード)`。パスワードそのものは保存しない）。作るのは `/bin/passwd`（`passwd [名前]`、`passwd -d` で消す。長さは問わない（空でもよい。2026-11-21 に 8 文字以上の決まりを外した、ユーザーの希望）。打った文字は表示しない）。読む・比べる・MD5（RFC 1321 を自分で書いたもの。md5sum と一致）は `os/api/include/monocs_auth.h`（static の関数。カーネルには入れない）。なければ Wi-Fi からは断る（HTTP は 403、telnet は 1 行出して閉じる）。
    - HTTP（`services/httpd.c`）: ダイジェスト認証（RFC 7616・2617、MD5、qop=auth）。nonce は「時刻 + MD5(時刻:起動ごとの秘密)」で、覚えずに確かめ、5 分で古くなる（stale=true）。`curl --digest -u 名前 http://アドレス/...`。telnet（`services/telnetd.c`）: `Monocs login:`・`Password:`（1 回の接続で 3 回まで、まちがえたら 1.5 秒待たせる、60 秒打たれなければ閉じる）。どちらも続けて 5 回まちがえたら 30 秒は Wi-Fi から断る（サービスごと）。
    - Windows からつなぐとき（2026-11-13、ユーザーが確認）: Windows の telnet.exe（PowerShell から）では、mi（curses）の画面とキーが崩れる（端末が ANSI・VT100 相当で、UTF-8 でもないため。Monocs の側は直していない）。端末ソフトの RLogin では正しく動く。
    - 限界: 暗号化はない（telnet のパスワードと画面、HTTP の中身は Wi-Fi の中をそのまま流れる。のぞいた HTTP の要求は 5 分の間は送り直せる）。/etc/htdigest の 1 行が読めれば HTTP の認証は通る（ダイジェスト認証の性質）。ユーザーは 1 人。
    - 確認（実機）: パスワードなしで Wi-Fi から 403・断り、USB は 200。仮のパスワードで、HTTP の GET・PUT（300KB の一致）・DELETE、誤ったパスワード・ユーザー名・Basic は 401、5 回で 429 → 31 秒後に 200、その間も USB は 200。telnet の正しいログイン・誤り → やり直し・3 回で切断・USB はログインなし。Wi-Fi 経由で 192.168.7.2 あてでも 401・ログイン・ポート 7 は拒否。verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版）。確認の後、仮のパスワードは消した（ユーザーが `passwd` で決める）。
  - **再開するとき（2026-11-13 の続き）**: 段階 1〜5、自動の接続、HTTPS（下の「HTTPS」）は済み。Bluetooth は見送り（ユーザーと合意。チップはコンソールの PL011 につながっていて、使うにはコンソールを mini UART に移すなどの変更が要る。使い道が決まってから）。
- キャッシュの偽の共有を減らす（2026-11-14。ユーザーの「CPU キャッシュのヒット率を高くする仕組みは？」から）:
  - 見つけたもの: コアごとの「持っている OS のロックの数」（`os/kernel/lock.c` の depth。ロックを取る・外すたびに各コアが書く）が 4 つの int で 1 本のキャッシュラインにあり、`interrupt_pending` やファイルのロックの表とも同じラインだった。ファイルのロック（`pool`、1 つ 12 バイト）は 1 本に 5 つ並び、コンパイラのように各コアが別々のファイルに fprintf すると、別々のロックが同じラインを奪い合う。malloc のロックも時計の変数と同じライン。
  - 直し: `struct klock` を 1 つでキャッシュライン 1 本（`alignas(CACHE_LINE)`、64 バイト。`os/kernel/lock.h`）、depth をコアごとに別のラインに（`depth_of[コア].n`）、`smp.c` のコアごとの状態も境界にそろえる。増えたメモリは約 5KB。
  - 効果（実機）: `tools/perf/fsbench.c`（3 コアがそれぞれ fprintf と malloc・free）で 3 コアの時間 150〜155 → 129〜134ms（1 コアなら 123ms）、コア 1 の L1 データのミス 112 万 → 29 万回。セルフホストのビルドの L1 データのミス 5,090 万 → 4,310 万回（約 15% 減）。ビルドの時間は変わらない（5.6〜5.7 秒。SD カードとロックの順番待ちが大部分で、揺れはカードの書き込みの時間による。前後のカーネルを交互に 6 回ずつ測った）。
  - 入っていないもの: 先読み（prefetch）、malloc のキャッシュラインへのそろえ（16 バイト単位）。キャッシュに効く作りとして前からあるもの: RAM はライトバック・2MB ブロック（TLB のミスがほとんどない）、malloc の空きリストは最近 free したものから使う、コンパイラは 1MB の塊から順に切り出す、コアごとの状態は TLS（別々の 1MB の領域）。
- アプリからほかのプログラムを動かす `system()`（2026-11-14。ユーザーと合意: ユーザーのファイルマネージャー fd（`~/dev/tools/file-dir-manager`、C11 + ncursesw、1 ファイル）をソースのまま動かすため、system() まで作る。fd は less・mi・rm -rf・任意のコマンドを system() で呼ぶ）:
  - 中身: `shell_system`（`os/shell/shell.c`）がシェルの 1 行（`run_line`。パイプ・リダイレクト・&&・PATH の検索も）を動かし、Linux と同じ形の終了状態（終了の値 << 8。例外・Ctrl-C は 1 << 8）を返す。子のアプリは親の中で入れ子に動く（app_call の入れ子）。コア 0 のアプリからだけ（ほかは -1・ENOSYS）。
  - 入れ子にしたもの: アプリの文脈（`os/kernel/app.c` の `struct app_ctx` の積み重ね。戻る場所・終了の値・atexit。exit・例外・Ctrl-C はいちばん内側だけを終わらせる）、アプリ用のヒープ（子は親の領域のすぐ下から。`memory_app_push`・`pop`、`malloc_app_begin` が親のヒープを退避。子の間、親の領域は広がらない。子の間はカーネル用のヒープの空きに逃げない）、持ち主（子は -1、-2…。`app_owner`。カーネルの印 VFS_OWNER_KERNEL は -1 から -1000 に）、curses（`curses_push`・`pop`。全部の状態を X マクロ `CURSES_STATE` で退避）、termios（`termios_push`・`pop`）、リダイレクト（`console_redirect_push`・`pop`。子は親のリダイレクトを受け継ぎ、子が開いたものだけ閉じる）、ほかのコアの仕事（`smp_abort_level`。子が終わったら子が始めた仕事だけ止める）、getpid、カレントディレクトリ、パイプの一時ファイルの名前（`/.pipeN` の番号を段ごとにずらす）。8 段まで。
  - Ctrl-C: system() の中（シェルの解析・コンパイル）は後回しにし（abort_defer）、子のアプリの間だけ受け付ける。子だけが止まり、親は system() から戻って続く。
  - あわせて: アプリが終わったら、シェルのカレントディレクトリを元に戻す（Linux と同じ。`os/slimcc/run.c`。それまではアプリの chdir がシェルに残った。変わったときだけ chdir するので、ふだんの負担は getcwd 2 回）。readdir は最初に「.」と「..」を返す（Linux と同じ。`os/libc/dirent.c`。fd・mi のファイルの画面は「..」があることを前提にしていた。/bin の rm・cp・ls と httpd は飛ばす）。curses の `nl()`・`nonl()`（既定の nl では Enter を '\n' で返す。それまでは '\r' だけで、fd が Enter に反応しなかった）、`def_prog_mode`・`reset_prog_mode`。UTF-8 の変換関数（`os/libc/mbchar.c`: mbrtowc・wcrtomb・mbstowcs など、MB_CUR_MAX = 4。picolibc はマルチバイトなしのビルドなので 1 バイト = 1 文字だった。`tools/picolibc/test_mbchar.c` で glibc と比べた。違いは、glibc が U+10FFFF より大きい値を受け付けるところだけ（RFC 3629 に合わせて誤りにした））。`lstat`（stat と同じ）・`strcasestr`・`basename`・`dirname` を OS API 表に。curses.h が stdio.h・stdint.h を読む（ncurses と同じ）、`ncursesw/ncurses.h`。/bin の rm・cp・wc・grep・head・tail が `--` を、less が `+g`・`+G`・`--` を受け付ける。
  - 確認（QEMU）: 子の出力と終了状態、2 段の入れ子、子の atexit、子の 5MB の malloc、Ctrl-C で子だけ止まる、親のメモリ・開いたファイル・カレントディレクトリが残る、親のリダイレクトを子が受け継ぐ。fd: 一覧、Enter で less → q で fd に戻る、x のコマンド（`ls -l %f; pwd` → Enter で戻る）、d のファイルの削除とフォルダの再帰の削除（`rm -rf --`）、r の名前の変更、終わった後のシェルのカレントディレクトリ。実機（2026-11-14、GCC 版）: fd の一覧・Enter で less・q で戻る・終了、hwcheck（GCC 版・slimcc 版）、速さは前と同じ（primes 46〜47ms、primes_mc 497ms、`ls /bin` の往復 0.47〜0.48 秒、カーネルのビルド 5〜6 秒。できたカーネルは Linux 上のビルドと一致）。`tools/picolibc/test_malloc.c` に入れ子（親のものが残る・子は親の領域に入らない・捨てた後に親の realloc・free）を足した。
  - 不具合（2026-11-21 に直した。前からのもの）: アプリの中の system() でソースのプログラムを動かしてコンパイルが起きると、そのアプリが終わった後の次のコンパイルで `free: invalid pointer … (ignored)` が並んだ。コンパイルを手伝うコア 1〜3 の仕事（compile_worker・exe_worker）は malloc_kernel_begin の深さ 0 で始まるので、呼んだアプリのヒープから取り、コンパイラがコアごとの状態に覚えたものが、そのアプリのヒープと一緒に捨てられていた（コア 0 は system() がカーネル用にしている）。仕事の間はカーネル用のヒープから取る（os/slimcc/run.c）。確かめ: system("cache clear; lua -v") を 2 回呼ぶアプリの後で `cache clear; lua -v`（直す前は 8 行出た）。
  - fd の置き方: `/apps/fd` に Linux のフォルダの main.c と Makefile をそのまま（下の「Linux の Makefile をそのまま読む」。それまでは compile_flags.txt に `-std=c11` を書いていた）。`fd [DIR]`。fd の e・g は、2026-11-14 から PATH の `mi`・`gt` を呼ぶ（ユーザーと合意して fd のほうを直した。fd の 0260d9d。Pi 5 は `~/.local/bin` に mi・gt へのリンクを作った）。Monocs では /apps/mi が呼ばれる（実機で確認）。gt は git がないので Monocs では動かない。
- Linux の Makefile をそのまま読む（2026-11-14。ユーザーの方針: 「fd のオリジナルのコードは変更せず、なるべくそのままコンパイルできる環境にしたい。Linux 用のコードと共有できるようにしたい」。Makefile を読む案と compile_flags.txt を置く案から、ユーザーが Makefile を選んだ）:
  - `os/slimcc/makefile.c`: GNU make の「読む」部分だけ（代入・条件・define・規則の行、展開と関数。規則のコマンドは動かさず、`$(shell)` は空、include は 2026-11-20 から読む）。既定のゴールから前提をたどり、前提に .c か .o を持つ最初の目標をプログラムとし、その .c（.o はその .o の規則の前提の .c、なければパターン・サフィックスの規則、なければ同じ名前の .c、なければ前のフォルダを外したもの）をコンパイルする。オプションは .o を作るコマンドの行（なければ `$(CPPFLAGS) $(CFLAGS)`）の -std・-D・-U・-I・-include（シェルと同じく引用符を解く）、`-l名前` は /lib にあるものだけ。
  - `run DIR`・`svc start DIR`・`/lib` のライブラリ: Makefile にプログラムの規則があればそれを使い、なければ前と同じ（compile_flags.txt とフォルダの .c、.runignore）。`os/slimcc/run.c` の load_folder。
  - シェル: `/` を含む名前が見つからないとき、親のフォルダの Makefile のプログラムの名前と同じなら、そのフォルダを動かす（`./fd`、`/apps/mi/mi`。Linux で make した後と同じ形。`os/shell/shell.c` の find_path_app。見つからないときだけ Makefile を読む）。
  - 確かめる: `sh tools/make/test.sh`（Linux。Makefile の後ろに `$(info 式)` を足して GNU make に展開させ、`tools/make/make_eval.c`（makefile.c をそのまま読み込む）の展開と比べる。t1.mk の 35 個の式とユーザーの fd・mi・gt の Makefile で一致。t1 のプログラムの決め方は t1.info と比べる。乱数で壊した Makefile 2,000 通りで落ちない（GCC の検査付き））。QEMU で、Linux のフォルダからそのまま写した fd（main.c・Makefile）と mi（.c・.h・Makefile。compile_flags.txt・.runignore なし。test_buffer.c は Makefile にないので入らない）が `./fd`・`/apps/mi/mi` で動く。実機でも、Makefile を置いた /apps/fd・/apps/mi で動いた（2026-11-14）。
  - fd の e・g は、fd を直して PATH の mi・gt を呼ぶ形にした（上の「system()」の fd の置き方）。
- Linux のプログラムがよく使う関数を足す（2026-11-14。ユーザーと合意:「Linux で作ったコードをそのまま動かす」の続きで、足りない関数の一覧のうち、小さなものをまとめて）:
  - OS API の表に足したもの（picolibc にあるもの）: getopt・getopt_long（optarg などの変数も）、sleep、strsep・stpcpy・strlcpy・memmem・strchrnul・strverscmp・strerror_r（GNU 版と POSIX 版 `__xpg_strerror_r`）など、asprintf・dprintf・open_memstream・fmemopen、qsort_r（GNU 版と BSD 版 `__bsd_qsort_r`。api.c では GNU 版を `__asm__("qsort_r")` の別名で取る）、reallocarray・getsubopt・mkstemp、random など、ctime・asctime・timegm・strptime、fnmatch、regcomp など、ワイド文字の文字列・数・入出力の関数。宣言がヘッダーの規格の条件で隠れるものは API_EXT。
  - 自分で書いたもの:
    - アプリごとの状態（`os/libc/posix.c` の posix_app_begin・posix_app_end。前の posix_new_app・posix_set_pid）: getpid の番号、getopt の状態（アプリを始めるとき optind を 0 にして、空の引数で getopt を 1 回呼ぶと、picolibc の中の状態も初めに戻り optind が 1 になる。Linux と同じ）。system() の子が終わったら親の optind・optarg などに戻す。
    - 環境変数（`os/libc/env.c`）: setenv・unsetenv・putenv は値をカーネル用のヒープに置き、アプリが最初に変えたときに一覧を控えて（カーネル用のヒープに）、アプリが終わったら戻す（Linux で子のプロセスの変更が親に残らないのと同じ。変えないアプリには負担がない）。putenv は写しを入れる。最初の版は控えをアプリ用のヒープに取っていて、子が終わった後に親の環境変数が消えた（試験で見つけた）。
    - usleep（posix.c）、glob（`os/libc/glob.c`。picolibc に本体がない。picolibc の glob.h は BSD 版で定数が違うので、glibc と同じ形の `os/api/include/glob.h` を /include に入れる）。
    - ワイド文字の分類（`os/libc/wctype.c`）: picolibc はマルチバイトなしのビルドで ASCII しか分類しなかった（この後マルチバイトありにしたが、glibc と同じにするため置き換えたまま）ので、glibc（C.UTF-8、2.41、Unicode 16.0）のすべての文字を調べて作った表（`os/libc/wctype_table.h`、`tools/gen_wctype.c`。約 32KB）で引く。wcswidth も wcwidth.c に（picolibc の wcswidth は picolibc の wcwidth と同じ部品にあり、引き込むとぶつかる）。
    - ワイド文字の入出力（`os/libc/wstdio.c`）: picolibc は wchar_t の 4 バイトをそのまま書くので、ファイル・端末は UTF-8 で読み書きする。swprintf・swscanf が中で作る文字列のストリーム（`__SWIDE` の印が初めからある）は picolibc と同じく wchar_t のまま（picolibc の vfwprintf は putwc で書き、vfwscanf は getwc_unlocked で読む。これらの部品を引き込むとぶつかるので、getwc_unlocked・ungetwc_unlocked も書いた）。
  - 確かめる: `sh tools/test/linuxapi.sh`（`tools/test/linuxapi.c` を Linux の gcc と Monocs の QEMU で動かして出力を比べる。2 回続けても同じ。入れ子 `linuxapi_nest.c` で子の変更が親・シェルに残らない）、`sh tools/libc/test_glob.sh`（glibc と 404 通り、検査付き）、`sh tools/libc/test_wctype.sh`（1,114,113 文字で glibc と同じ）。
  - printf の `%ls`・`%lc`（2026-11-14。ユーザーと合意して picolibc の設定を変えた。ソースは変えていない）: Makefile の PICOLIBC_OPTS に `-Dmb-capable=true -Dio-wchar=true`（それまでは両方なしで、`%ls` は ASCII も正しく出なかった）。picolibc の printf はロケールの文字の種類で変換するので、Linux と同じロケールの扱いにした:
    - アプリは「C」で始まる（posix_app_begin。前のアプリの setlocale を残さない。入れ子の子が終わったら親のロケールに戻す）。
    - アプリの setlocale（`app_setlocale`、posix.c）は "" を環境変数 LC_ALL・LC_CTYPE・LANG の順で選ぶ（picolibc の setlocale は "" をいつも C にする）。既定の環境変数に `LANG=C.UTF-8`（syscalls.c）。
    - mbrtowc などの Monocs の変換（mbchar.c）、fputws（wstdio.c）、isw*（wctype.c）は、今までどおりロケールを見ない（いつも UTF-8・glibc の表）。
    - 文字の分類のマクロ（isalpha など）は表引きのまま（picolibc がロケールで切り替えるのは、Unicode 以外の文字コードを入れたとき（mb-extended-charsets）だけ）。
    - フォルダを移したときと同じく、picolibc の設定を変えたら `rm -rf build/picolibc build/picolibc-install build/picolibc-slimcc` してから作り直す。
  - カーネルは 931KB → 1,087KB（ワイド文字の入出力・正規表現・strptime・表、マルチバイト）。
  - 実機（2026-11-14、1000MHz、変える前（1ac6265）と後のカーネルを交互に）: primes 46〜47ms・primes_mc 497ms・`ls /bin` の往復 0.47〜0.48 秒はどちらも同じ。セルフホストのビルドの往復は前 7.3〜8.7 秒（平均 8.0）、後 7.3〜8.7 秒（平均 7.8）で、SD カードの揺れの範囲。hwcheck（GCC 版・slimcc 版）、linuxapi.c の出力が Linux と同じ（55 行）。
- シグナルと popen（2026-11-14。ユーザーと合意: 「よく使う小さな関数」の次に、シグナル（形だけ）と popen）:
  - シグナル（`os/libc/signal.c`。picolibc の signal・raise・sigprocmask より先にリンク）: signal・sigaction（SA_SIGINFO・SA_RESETHAND・SA_NODEFER、sa_mask）・raise・sigprocmask・sigpending・sigsuspend・pause・kill（今のアプリにだけ）。アプリごとに表を持ち（posix_app_begin・end の PosixApp に控え。カーネルは ISO の C23 なので、控えは大きさだけの箱 SignalState）、アプリは SIG_DFL から。既定の動きは Linux と同じ（SIGCHLD・SIGWINCH・SIGURG・SIGCONT と止めるものは無視、ほかは app_exit(128 + 番号)）。
    - 届くもの: Ctrl-C（raw でないとき。app_request_interrupt → signal_on_interrupt_key。関数があれば印だけ、SIG_IGN なら無視、SIG_DFL なら今までどおり止める）、SIGWINCH（contap_set_size で大きさが変わったとき）、raise・kill。
    - 関数を呼ぶのは、コア 0 が待つところ（cpu_idle の後で IRQ が入る状態のとき、uart_wait_input の終わり）の signal_point で、アプリが動いていてカーネルのロックを持っていないとき。割り込みの中からアプリの関数は呼ばない（戻り方が難しく、壊れたとき戻れない）。そのため、待たずに計算し続けるアプリには届かない。
    - Ctrl-\ は、raw でないときもアプリを止めるようにした（`uart_interrupt_is_quit`。Linux の SIGQUIT の既定と同じ。SIGINT に関数を登録して計算し続けるアプリの逃げ道。それまでは raw のときだけ）。
    - 読み込みは、関数を呼んだ後も続ける（SA_RESTART と同じ）。nanosleep・sleep は残りを返して EINTR で早く戻る（nanosleep を、1 ティックより長い間は cpu_idle で眠る形に直した。最初の版は 10ms ずつ timer_delay_us を呼び、1 ティック以下は眠らずに回るので、シグナルが渡らなかった）。
  - popen・pclose（posix.c）: 一時ファイル `/.popenN`（入れ子の段ごとに別の名前）。"r" はその場で `shell_system_redirect`（行全体の標準出力をファイルに。リダイレクトを 2 段積んで子のコマンドの土台にする）で動かしてから読む、"w" は pclose のときにファイルを標準入力にして動かす。終了状態は system() と同じ形。pclose せずに終わったアプリの一時ファイルは posix_app_end で消す。
  - あわせて: system() の行の export・cd も、終わったら戻す（env_snapshot_now。Linux と同じ）。
  - 確認: `tools/test/linuxapi.sh`（シグナル・popen を足した。QEMU で Linux と同じ）、QEMU と実機で Ctrl-C の関数・SIG_IGN・計算し続けるアプリの Ctrl-\・関数のない Ctrl-C、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版）、実機の linuxapi（69 行が Linux と同じ）。速さ（実機）は前と同じ（primes 46〜47ms、primes_mc 497ms、`ls /bin` 0.47 秒、ビルドの往復 7.6〜8.6 秒）。
  - 残り: alarm・setitimer（時間のシグナル）、system() の結果の WIFSIGNALED（シグナルで終わった子も、ふつうの終了 128 + 番号として返る）。
  - 実機へのカーネルの送り込み（`python3 tools/uart/nos.py kernel *`）と hwcheck は、ユーザーの許可で `.claude/settings.local.json` に許可のルールを足した（2026-11-14。コミットしない）。
- Linux と同じソケット・select・poll・alarm（2026-11-14。ユーザーと合意: 設計の案を見てもらってから。bind(INADDR_ANY) は Linux と同じくすべての口（USB と Wi-Fi）から受ける）:
  - `os/libc/socket.c`: BSD ソケット（AF_INET の TCP・UDP）を、Monocs の TCP・UDP の API の上にかぶせた。fd はファイルと同じ番号の並び（2026-11-20 から。前はソケット 16〜39、ファイル 3〜15）。VFS の read・write・close・lseek・fstat（S_IFSOCK）、fcntl（vfs.c に新しく。O_NONBLOCK はソケットだけ）、ioctl（FIONBIO・FIONREAD）から振り分ける。待つ操作は「待たずに試す → cpu_idle」を繰り返す共通の形（時間切れ SO_RCVTIMEO・SO_SNDTIMEO は EAGAIN、待たない形は EAGAIN・EINPROGRESS、SA_RESTART のないシグナルの関数で EINTR）。つながっていない TCP への送りは EPIPE と SIGPIPE（MSG_NOSIGNAL がなければ）。UDP は bind か最初の送り・受けで口を開く。アプリ・サービスの終わりに socket_close_owner（run.c・svc.c）。
  - TCP・UDP の API に足したもの: 待たずにつなぎ始める tcp_api_connect_start・connect_state、待たずに調べる tcp_api_ready・udp_api_ready、tcp_api_local・udp_api_local、tcp_api_shutdown_write、tcp_api_recv_peek（MSG_PEEK）、tcp_api_refused（ECONNREFUSED と ETIMEDOUT の区別。on_err の ERR_RST）、tcp_api_available（FIONREAD）、udp_api_open_addr（bind のアドレス）。lwIP・ドライバには手を入れていない。
  - `os/libc/netdb.c`: getaddrinfo（glibc と同じく ai_socktype 0 で TCP・UDP・RAW。サービスを名前で指定したら RAW を除く。数字のサービスは範囲を調べず下の 16 ビット）・freeaddrinfo・gai_strerror（glibc と同じ文字列）・getnameinfo（数字だけ）・gethostbyname・getservbyname（よく使うものの表）・hstrerror・inet_pton・inet_ntop・inet_aton（glibc と同じく a、a.b、8 進・16 進も）・inet_addr・inet_ntoa・gethostname（"monocs"）。名前は dns_api_resolve（10 秒）。
  - `os/libc/select.c`: select（残りの時間を書く）・pselect・poll。fd 0 は termios.c の term_input_ready（ICANON でも 1 文字で読める）。待つ間は cpu_idle、シグナルの関数が呼ばれたら EINTR。
  - alarm（signal.c）: 時刻を覚え、signal_point で来ていたら SIGALRM を届ける。アプリごとに控える。
  - ヘッダー（`os/api/include`。mkimg.sh が /include に入れる）: sys/socket.h・netinet/in.h・netinet/tcp.h・arpa/inet.h・netdb.h（Linux の glibc 2.41 と同じ形と値。Pi 5 の glibc で値を拾った）。picolibc の poll.h は __size_t を宣言せずに使っていて単独で読めない（picolibc の不具合）ので、poll.h・sys/poll.h も置き換えた。picolibc の arpa/inet.h（htonl などだけ）も置き換え。htons などは自前のインライン関数（picolibc の endian.h の __bswap32 は __builtin_bswap32 を使い、slimcc にない。`<endian.h>` の be32toh などはアプリで使えないまま）。
  - 確かめる: `sh tools/test/netapi.sh`（相手のいらないもの 50 行。Linux と実機で比べる。QEMU は USB の機器がなく lwIP が動かないので実機で）、`python3 tools/test/sockets/run.py example.com`（Pi 5 が TCP・UDP のエコー、Monocs の client.c が 1MB のエコー（0.06 秒）・待たない connect と poll・SO_ERROR・ECONNREFUSED・shutdown・UDP と SO_RCVTIMEO の時間切れ・DNS と HTTP、Monocs の server.c（select で複数の接続）に Pi 5 から 3 つ同時に 300KB ずつ）。
  - ないもの: IPv6、AF_UNIX、SOCK_RAW、マルチキャスト、sendmsg・recvmsg、dup・dup2、ループバック（127.0.0.1 は ECONNREFUSED）、setitimer。
  - hwcheck に Linux との互換の試験を入れた（2026-11-14。ユーザーと合意。`tools/test/compat_dev.sh`: linuxapi と入れ子・netapi・sockets/run.py・SIGWINCH。カーネルごとに約 2 分）。あわせて、確かめていなかったもの（pselect、accept4 の SOCK_NONBLOCK、SA_RESTART のない SIGALRM で accept が EINTR、MSG_PEEK、telnet の大きさの変更で SIGWINCH）を試験に足した。そのとき見つけたもの: picolibc の sys/ioctl.h に struct winsize がなく、Linux のように `<sys/ioctl.h>` だけでは画面の大きさを取れなかった → `os/api/include/sys/ioctl.h` で置き換え（struct winsize は picolibc の sys/termios.h、ioctl は Linux と同じく可変長の引数）。tnterm.py の until が、キーを送った直後に届いた応答を見逃していた（qterm.py と同じく、最後にキーを送り始めたところから探す形に）。
    - sys/ioctl.h を置き換えたら、実機のセルフホストのビルドが失敗した: セルフホストのビルドは実機の /include（アプリ用のヘッダー）でカーネルもコンパイルするので、カーネルの termios.c の ioctl の定義（picolibc の void * の形）と、置き換えた宣言（可変長）がぶつかった。termios.c は sys/ioctl.h を読まずに、定数と可変長の ioctl を自分で書く形に直した。**アプリ用のヘッダーを置き換えるときは、カーネルが <…> で読んでいないかを確かめる**（`grep -rn '#include <sys/ioctl.h>' os cc`）。hwcheck は、セルフホストのビルドに失敗しても何も出していなかった（done の行を grep するだけ）ので、`self build: FAILED` とエラーの行を出す形にした。
  - 確認（2026-11-14）: 上の 2 つの試験、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版）。速さ（実機）は前と同じ（primes 47ms、primes_mc 497ms、`ls /bin` 0.47 秒、ビルドの往復 7.4〜8.4 秒）。カーネルは 1,090KB → 1,109KB。
- Pi 5 の Claude Code から操作しやすくする（2026-11-15。ユーザーの「Claude Code から Monocs を操作するのに、あったら楽になる機能は？」から、ユーザーと合意して 5 つまとめて）:
  - シェルの変数の展開（`os/shell/shell.c`）: `$NAME`・`${NAME}`・`$?`・`$1`〜`$9`（空）。引用符の外と `"..."` の中（`'...'` と `\$` はそのまま）。字句の切り分けでは印だけを残し、パイプラインを動かす直前に展開する（`false; echo $?` が 1。bash と同じ）。それまでは変数をまったく展開していなかった（`$PATH` もそのまま）。ファイル名の展開（`*`）はない。
  - 終了の値を Linux のシェルと同じに（`os/slimcc/run.c`）: 例外 139、Ctrl-C 130、Ctrl-\ 131、timeout の期限 124、動かせなかった（コンパイルの誤り）1、コマンドがない 127、書き方の誤り 2。system() の戻り値もこれ << 8（それまでは止まったら 1 << 8）。
  - 組み込みのコマンド: `unset`、`true`・`false`、`timeout [-s SIG] [-k DUR] [--preserve-status] [-v] DURATION CMD...`（GNU と同じ書き方。期限を過ぎたら Ctrl-C と同じ道筋でアプリを止める（シグナルの関数は呼ばない。-s・-k は受け付けるだけ）。`os/kernel/app.c` の app_timeout_push・pop・due。期限ごとに「その timeout が始めるアプリの深さ」を覚え、タイマー割り込みでその深さ以上のアプリが動いていれば止める。入れ子は内側から 1 つずつ止まり、system() から戻ったときにも調べてその場で止める（割り込みを待つと、親が 10ms の間に動いた）。組み込みのコマンドは止めない。8 段まで）、`dmesg [-c|-C]`。
  - カーネルの記録（`os/kernel/klog.c`。dmesg、httpd の `GET /.log`、OS API の `monocs_klog_read`、`nos.py log`）: 64KB の輪、行ごとに `[秒.マイクロ秒]`。起動からシェルの最初のプロンプトまでは、コンソールの表示をすべて記録する（uart_putc から。USB のコンソールはつながる前の表示が見えないため）。その後は、klog_printf で書くできごと（サービスの開始・停止・失敗、例外で止まったアプリ、Wi-Fi の接続・失敗（同じ失敗の繰り返しは書かない）・アドレス・切断、USB の構成・バスのリセット、SD カードの読み書きの誤り）と、klog_tee_begin〜end の間の表示（例外・スタックのあふれ・コアが止まらないの表示）。コンソールにも出す文は、起動中は書かない（klog_printed。表示がそのまま入るので 2 回になった）。MMU の前は排他アクセスを使わない（lock_take が SCTLR_EL1.M を見る）。記録しない間の uart_putc の負担は、比べる処理 1 回。
  - nos.py: `cmd`・`run` の終了の値は、そのコマンドの終了の値（httpd の `/.status` に足した `status N`。OS API の monocs_shell_status。HTTP がなければ 0）。`log`（打ち込まないので、使っている最中の実機でもよい）。出力の UTF-8 を読み込みの切れ目をまたいで解く（codecs の incremental decoder。1 文字が 2 回の読み込みに分かれると化けていた）。
  - 確かめる: `sh tools/test/shell.sh`（QEMU。`tools/test/shell/lines.sh` の 14 行を bash と比べる、timeout の 5 通り、例外・Ctrl-C の $?、dmesg）。hwcheck に「shell status, timeout, log」。
  - 続き（2026-11-15。ユーザーと合意して、残していた違いを埋めた）:
    - 値の分け直し: 引用符の外で展開した値（`$X`・`$(...)`）を空白で語に分ける。字句の切り分けで、引用符の外と中の展開を別の印にし（M_VAR・M_QVAR・M_CMD・M_QCMD）、引用符があったことも幅なしの印（M_QUOTE）で残す（`""` だけの語を残すため。展開のない語からは、切り分けの直後に除く）。代入の形の語とリダイレクトの行き先は分けない。
    - 代入: `NAME=値 コマンド`（そのコマンドの間だけ環境変数。終わったら戻す）、`NAME=値` だけ（シェルの変数。svars。展開は環境変数、なければシェルの変数。`export NAME` で環境変数に移す。`unset` は両方を消す。system() の子には見えない）。代入だけの行の $? は、最後の `$(...)` の値（なければ 0）。
    - `$(...)`・`` `...` ``: 中の行を入れ子で動かし、標準出力を一時ファイル `/.subN` に取る（capture。console_redirect_push を 2 段。アプリがなくても使える）。中の cd・export・代入は戻す（shell_save・shell_restore: シェルの変数と環境変数の写し）。`$((...))` はない（書き方の誤りにする）。
    - timeout のシグナル: 期限を過ぎたら、いちばん内側のアプリに `-s` のシグナル（既定 TERM）を 1 回ずつ届ける（signal_send_timeout。関数があれば印を付け、コア 0 が待つところで呼ぶ。無視なら何もしない。既定の動きで終わるなら、今までどおり止める）。入れ子のアプリは、内側が終わると次の外側に届ける（深さごとの印 signaled）。`-s KILL` と `-k` の時間を過ぎたら必ず止める。
    - nos.py は、出力を head などで途中まで読まれたら黙って終わる（SIGPIPE を既定に。それまでは BrokenPipeError の表示）。
    - 確認: verify.sh quick（一致）、linuxapi.sh、hwcheck（GCC 版・slimcc 版。全部 ok）。実機で分け直し・`$(...)`・`` `...` ``・`NAME=値 コマンド`。速さ（前のカーネルと交互に）: シェルの往復 7ms・primes 47ms・primes_mc 497ms でどちらも同じ。
    - 確かめる: shell.sh の bash と比べる行を 33 行に（分け直し・代入・シェルの変数と export・`$(...)`・`` `...` ``・入れ子）。timeout のシグナル 6 通り（TERM を関数で受ける、--preserve-status、-s KILL、無視して -k、-s INT、不正な名前）。
  - さらに続き（2026-11-15。ユーザーと合意: ファイル名の展開・`$((...))`・`${NAME:-x}` などの形・if・for まで）。シェルを「行を順に流す」作りから「構文の木を実行する」作りにした（`os/shell/shell.c`）:
    - 切り分け（tokenize_line）→ 構文（parse_list・parse_and_or・parse_pipeline・parse_command・parse_if・parse_for・parse_while。Node の木。予約語は、コマンドの頭の引用符のない語のときだけ）→ 実行（exec_node。単純なコマンドは、その段を動かす直前に展開する）。改行は ; と同じ区切り（T_NL）。語の頭の # は注釈。
    - 閉じていないとき（if・for の途中、引用符・$(・${ が閉じていない、行の終わりの \・|・&&・||）は TOK_INCOMPLETE。shell_main はプロンプト "> " で続きの行を読んでつなぐ（text_incomplete。8192 文字まで）。/.profile も同じ。system() の文字列も複数の行でよい。行の長さの上限 256 → 1024（LINE_MAX_LEN・LINE_EDIT_MAX）、引数 16 → 256（ARGS_MAX。ファイル名の展開で増えるため）。
    - 展開に足したもの: ${NAME:-語}・-・:=・=・:+・+・:?・?、${#NAME}、${NAME#パターン}・##・%・%%（fnmatch）、$((式))（ar_*。long long。名前は変数の値）。${...} の中の語は expand_raw（引用符・$ を解く）。ファイル名の展開: 引用された * ? [ ] \ には M_LIT を付け、語ごとにパターン（引用された特別な文字は \ を前に）を作り、引用符の外の * ? [ があれば glob（os/libc/glob.c。合わなければそのまま）。
    - test・[（POSIX の test）、break・continue（N も）、!。
    - Ctrl-C: アプリの実行中でないときに押された数を数える（uart_shell_interrupts。os/drivers/uart.c）。行の実行中に増えたら（組み込みのコマンドだけの while など）、行の残りを動かさず $? を 130 に。Ctrl-C で止められたアプリ（130・131）の後も、行の残りを動かさない（bash と同じ）。
    - 確認: shell.sh（81 行が bash と同じ）、linuxapi.sh、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版。全部 ok）。実機でファイル名の展開・計算・${…}・if。速さ（前のカーネルと交互に）: シェルの往復 7ms・primes 47ms・primes_mc 497ms でどちらも同じ。
    - 確かめる: shell.sh の bash と比べる行を 81 行に（if・for・while・until・break・continue・計算・${…}・test・複数の行・ファイル名の展開・注釈）と、while を Ctrl-C で止める。
  - その続き（2026-11-15。ユーザーと合意: 残りの case・関数・( )・{ }・$# $@ $$・${NAME/a/b}・ヒアドキュメント・~・{a,b}。& は入れない（2026-11-15、ユーザーと合意して見送り。理由: シングルタスク・「速さが命」の方針とぶつかる（裏のアプリが手前のアプリのコアを減らす）、すべてのアプリが通るところ（文脈・ヒープ・持ち主・シグナル）を変えるので壊れたときの影響が大きい、裏では curses・system() などが使えず動かせるものが限られる、動き続けるものはサービス（svc start）、並列は monocs_core_start で足りる。書くと書き方の誤り）:
    - 切り分け: ( ) ;; を演算子に（T_LPAREN・T_RPAREN・T_DSEMI）。<<WORD（T_HEREDOC と本文の語。本文は改行の後の行から読む。read_heredoc。区切りの行がまだなければ続きがいる）。~（語の頭・NAME= の直後）は M_QVAR HOME。引用符の外の { があれば brace（展開のときに {a,b}・{x..y[..step]} を開く。brace_expand_into。印の中と M_LIT の文字は飛ばす）。"$@" だけの語は引用の印を付けない（引数がなければ語がなくなる）。トークンに元の文の位置（off・end）。
    - 構文: case（N_CASE・N_CASEITEM。パターンは fnmatch。引用された * などは \ を付けたパターンにする（expand_single）。引用された特別な文字のある語は印を残す）、( … )（N_SUBSHELL。shell_save で変数・関数・位置の引数・環境変数・cd を戻す）、{ …; }（N_GROUP）、NAME() 本体・function NAME（N_FUNCDEF。本体を元の文の文字列で覚え、呼ぶたびに読み直す）。
    - 関数: run_command の最初に探す。位置の引数（pos_argv）を入れ替え、local（LocalSave で戻す）・return（ret_pending）。system()・sh の子には関数・位置の引数・export していない変数を見せない。
    - 組み込みのコマンド: :、set --、shift、local、return、unset -f、source・.（今のシェルで）、sh FILE・sh -c（別のシェルとして）。
    - 不具合（直した）: リダイレクト付きの if・for・{ }・( ) が、パイプラインの段でないとき（`{ …; } > f`）にリダイレクトを使っていなかった（exec_node が直接 exec_compound を呼んでいた）。関数・sh・source をリダイレクト付きで呼ぶと、中の最初のコマンドが終わったところで外の向け先まで戻していた（向け先を変えた段は console_redirect_push で土台にしてから動かす）。
    - 確かめる: shell.sh の bash と比べる行を 132 行に（`sh script.sh x y` のスクリプトのファイルも bash と比べる）。& を断る。
    - 確認: shell.sh（132 行が bash と同じ）、linuxapi.sh、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版。全部 ok。slimcc 版の互換の試験で一度だけ `nos.py: timed out`（HTTP で送るときの時間切れ）が出たが、流し直した 2 回は出なかった）。速さ（前のカーネルと交互に）: シェルの往復 7ms・primes 47ms・primes_mc 497ms でどちらも同じ。
  - 確認（2026-11-15）: shell.sh、linuxapi.sh、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版。全部 ok）。実機の dmesg に起動の表示・サービスの開始・USB の構成・Wi-Fi の接続とアドレスが時刻付きで残る。速さ（1000MHz）: primes 47ms、primes_mc 497ms。`ls /bin` の往復（シリアルだけ、nos.py の起動を除く）は前のカーネルと交互に測って、どちらも 7ms。
  - つまずいたこと: 新しい httpd（monocs_shell_status を使う）を送ったら、実機の /include/monocs.h が古く、コンパイルに失敗して httpd が動かなかった（何も記録されなかった）。**サービス・/bin のアプリが新しい OS API を使うときは、先に `nos.py push os/api/include/monocs.h /include/monocs.h`**。サービスを読み込めなかったことも記録するようにした（`svc: cannot load ...`）。
- pthread（2026-11-15。ユーザーの「シングルユーザーでも余っているコアは使いたい」から。`&` はやめて、ユーザーと合意して pthread から。基本設計（同時に動くアプリは 1 つ、スケジューラなし）は変えない）:
  - `os/libc/pthread.c`・`os/api/include/pthread.h`（Monocs の型。ソースの互換だけ）: スレッド 1 つに空いたコア（サービスがあればコア 1・2）を 1 つまるごと渡す（smp_run_thread）。空きがなければ EAGAIN。記録（Thr、32 個）はコアとは別に持ち、終わって join されていないものの戻り値を残す。待つ関数は条件を調べては wfe（外す・知らせる・終わるたびに sev）。コア 0 は待つ間もタイマーの割り込みで起き、signal_point でシグナルの関数も呼ぶ。時間切れのある待ちは、コア 1〜3 では割り込みで起きないので yield で回る。
  - mutex（NORMAL・RECURSIVE・ERRORCHECK、持ち主はコアの番号 + 1）、条件変数（seq を増やして全部を起こす）、once、key（スレッドの終わりに destructor）、rwlock、barrier、spin、sched_yield、pthread_sigmask・pthread_kill（main に届ける）、attr。pthread_cancel は ENOSYS。
  - アプリ全体に広げるもの: スレッドの中の exit()（app_exit_with_handlers・app_exit がコア 0 以外なら app_request_abort(APP_EXITED) して smp_exit_job。atexit はコア 0 が呼ぶ）、例外（smp_fault が thread の印を見て app_request_abort(APP_FAULTED, 139)）。コア 0 は次のタイマー割り込みで止める（exception.c は止める理由を app_pending_reason で見る。Ctrl-C のときだけ「^C at」）。それまでは、ほかのコアで exit を呼ぶと、コア 0 の戻り先に longjmp していた（危なかった）。
  - アプリの終わり: 動いているスレッドのコアは app_call の smp_abort_level が止める。記録は pthread_app_end（run.c）。サービス（コア 3）からは作れない。
  - アプリに渡す定義に `_POSIX_THREADS`、`_SC_NPROCESSORS_CONF`・`_SC_NPROCESSORS_ONLN`（picolibc にないので 1000 番台。os/libc/syscalls.c の sysconf と同じ値）、`_SC_PHYS_PAGES`・`_SC_AVPHYS_PAGES`（run.c の app_defines。それまでは 1 つ目しか渡していなかった）。sysconf を OS API の表に（それまで表になかった）。`<sys/sysinfo.h>` の get_nprocs。`tools/mkimg.sh` は /include に入れるヘッダーを 1 つずつ書く（pthread.h・sys/sysinfo.h を足した）。
  - 気づいたこと: slimcc に GCC の `__atomic_*`・`__sync_*` の組み込み関数がない（C11 の stdatomic.h はある）。
  - 確認（2026-11-15）: pthread.sh、shell.sh（132 行）、linuxapi.sh、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版。pthread を含めて全部 ok）。速さ: primes 47ms・primes_mc 497ms（前と同じ）。
  - 確かめる: `sh tools/test/pthread.sh`（QEMU。pthtest.c の出力を Linux と比べる 16 行、スレッドの exit(5)・例外 139・EAGAIN）、compat_dev.sh に pthread（実機）。`samples/primes_pthread.c`: 実機で 1・2・3 スレッド 1155・714・497ms（primes_mc.c と同じ）。
- OpenMP（2026-11-15。ユーザーと合意: pthread の次に。コンパイラ（slimcc）に手を入れる）:
  - `cc/omp.inc`（parse.c の終わりで取り込む）: `-fopenmp`（opt_openmp。main.c・libslimcc の SlimccOptions.openmp。`_OPENMP` 201511）のときだけ、preprocess3 が `#pragma omp` の行の字句を TK_PRAGMA の omp からパーサーに渡す（ないときは GCC と同じく読み捨て）。compound_stmt2 で omp_directive。
  - parallel: 文を `static void __omp_fnN(void **ctx)` に切り出す。外の関数の中にいるまま読み（enter_fn_scope の親は今のスコープなので、型・タグ・列挙の名前も見える）、外の関数の局所変数を名前で見つけたら（find_var が関数のいちばん外のスコープの外で見つけた。find_var_past_base）、その変数へのポインタを局所変数に作り、`*ptr` に置き換える（omp_ref。既定の共有）。読み終えたら、ptr = ctx[i] の文を前に置いて emit_text。外の関数には「ctx に &変数 を並べて __monocs_omp_parallel(関数, ctx, スレッドの数, if) を呼ぶ」文を、生成した文字列から読ませる。切り出しの中の切り出しも同じ形でつながる。
  - for: 繰り返しの頭を字句で読み（for (型 VAR = LB; VAR op UB; inc)）、`__monocs_omp_loop_init`・`loop_next`（受け持ちの範囲）・内側の 1 回だけの for で VAR を決めて、元の文をそのまま本体にする字句に書き換える（continue もそのまま効く）。文の終わりは omp_skip_stmt（字句で）。private・firstprivate・reduction は、__typeof__ で影の変数を作る文字列。critical・atomic・single・master・barrier なども文字列に置き換える。
  - ランタイム `os/libc/omp.c`（pthread の関数だけで作る。MONOCS_OMP_LINUX で Linux でも動く）: チーム（作れたスレッドの数で決まる。作ったスレッドは人数が決まるまで待つ）、static・dynamic の分け方、バリア（pthread_barrier）、single、critical のロック、omp.h の関数、omp_lock_t。OS API の表に。`os/api/include/omp.h` を /include に（mkimg.sh）。`run -fopenmp`、compile_flags.txt・Makefile の CFLAGS の -fopenmp（run.c の add_flag、makefile.c）。
  - 不具合（作る途中で直した）: 生成した字句を new_file で作ると、前処理の領域（もう捨ててある）に名前を足して落ちた（pragma の字句のファイルを写して使う）。omp_addf が 512 バイトの入れ物で、長い宣言が切れた。生成した字句の attr_next（alloc_next と同じ場所）が残り、ほかの宣言の属性（noreturn など）が付いて、切り出した関数の本体が消えた（空にする）。cc/Makefile が omp.inc を parse.o の依存に入れていなかった（ir.inc と同じく足した）。
  - 確認（2026-11-15）: make cc-test（2・3 世代目の一致を含む）、openmp.sh、pthread.sh、shell.sh、linuxapi.sh、verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版。openmp を含めて全部 ok）。速さ: primes 47ms・primes_mc 497ms（前と同じ）。hwcheck の slimcc 版で、互換の試験の最初に `nos.py: timed out` が 2 回出た（どちらも結果は ok）。調べた（2026-11-15）: 出たのは HTTP の PUT・HEAD の時間切れ（socket の「timed out」。60・30 秒。シリアルの待ちなら「timeout waiting for」になる）で、どちらも slimcc 版がその hwcheck の最初のカーネルのときの、互換の試験の最初の push（linuxapi.c）。同じ流れ（GCC 版 → slimcc 版、キャッシュが空の状態）を 11 回、push だけを 30 回流して一度も出ず、push はいつも約 0.5 秒。原因はわかっていない。push が失敗しても、実機に前の回のファイルが残っていて試験は ok になっていたので、compat_dev.sh は push の失敗を失敗に数える。nos.py の時間切れの表示に、どの段（connect・PUT send・PUT response・HEAD）で何秒止まったかを出す（次に出たときの手がかり）。
  - 確かめる: `sh tools/test/openmp.sh`（tools/test/openmp/omptest.c: Linux の slimcc -fopenmp + os/libc/omp.c が GCC の -fopenmp（libgomp）と同じ出力（スレッド 1・2・3・既定）、QEMU の Monocs も同じ（17 行））、compat_dev.sh に openmp（実機）。`samples/primes_omp.c`: 実機で 1・2・3 スレッド 1172・585・391ms。
- 命令の並べ替え（2026-11-15。ユーザーの「GCC の方が速いのはなぜ」から、ユーザーと合意して。`cc/sched.inc`、`-fschedule-insns2`（既定）・`-fno-schedule-insns2`）:
  - 中間表現から出した関数のうち、ループの中のブロック（並び（逆後順）で前に戻る辺の範囲。`ir_hot_blocks`）の出力を取り込み（`ir_sched_begin`・`end`）、基本ブロックの中で Cortex-A53 に合わせてリスト・スケジューリングする（レジスタの割り当ての後。行を並べ替えるだけで、中身は変えない）。区切り（動かさない）: ラベル、ディレクティブ、分岐・呼び出し・ret、表にない命令、sp・x29・x30 を書く命令（割り込みが同じスタックを使うので、sp の下への読み書きを sp を動かす命令の向こうへ出さない）、インラインアセンブリと volatile・アトミックの読み書き（ir_emit_inst が印の行 `\x01`・`\x02` で囲む）。ロードどうしは入れ替え、ストアとは同じ土台（間で書き換えていない）・即値のオフセットで重ならないときだけ。並べ替えた順が同じ模型で速くならなければ元の順のまま。2 命令までの並びは読み解かない。
  - 遅延と同時に出せる組は、実機で測った（`tools/perf/a53lat.c`。ARM は A53 の表を公開していない）: 1 サイクルに 2 つ（ロード・ストアは 1 つ、ldp・stp は 2 サイクル、x の掛け算は 2 サイクルに 1 つ、浮動小数点の足し算・掛け算は 2 つ、積和は 1 つ）。結果はふつうの演算 1、ロード 2（浮動小数点のレジスタ 3）、掛け算 w 2・x 3、浮動小数点 4、積和 8（足す側は 4）、割り算 w 約 11・x 約 19（値が大きいとき）。ロード・ストアの番地・ずらす命令の入力・ずらす側の入力・掛け算の入力は 1 サイクル早く要る。ほかに見つけたこと: 依存のないふつうの演算は 1 サイクルに 2 つ出せる、fabs・fneg も 4 サイクル、内蔵アセンブラに fcsel がない。
  - 効果（実機、同じカーネルで `-fno-schedule-insns2` と交互に）: nbody 5.66 → 4.98 秒（12%）。primes・strhash・samples/primes.c・primes_mc は変わらない（整数・分岐の多いループはブロックが小さく、動かせる命令がほとんどない。ループを回す形を変える（ループの反転・展開）などは別の仕事）。
  - 既定で使う（2026-11-15、ユーザーと合意。コンパイルの 2.5〜3% は、2 回目からはコンパイルのキャッシュでほとんど見えないため）。
  - コンパイルの時間: 最初は関数全体にかけて、実機で 10〜12% 遅くなった（Pi 5 で 12%）。書き出しを fwrite に、2 命令までは読み解かない、元の順がもう最短なら並べ替えない、ループの中だけ、にして実機で 2.5〜3%（`cc -c -o /dev/null` の parse.c 525 → 540M サイクル、codegen.c 791 → 810M サイクル。SD カードへの書き込みの揺れを除くため /dev/null に書いた）、Pi 5 で 2%。
  - 不具合（作る途中で直した）: b.cond の表の項目を if の中の複合リテラルに置き、外で読んでいた（AddressSanitizer で見つけた。出力が実行ごとに変わり、ストアが分岐の後ろに出た）。slimcc に `__builtin_ctzll` がなく、自分をコンパイルできなかった（sch_ctz）。
  - 確認: make cc-test（2・3 世代目の一致、softfp128 の 690 万件、test/ 106 本）、verify.sh（全部。セルフホストの一致、libctest）、shell.sh・pthread.sh・openmp.sh・linuxapi.sh、hwcheck（GCC 版・slimcc 版）。オプションは run・compile_flags.txt・Makefile の CFLAGS・シェルの cc・SlimccOptions の no_schedule。
- ループの最適化（2026-11-15。ユーザーの「ループの反転・展開も」から。`cc/ir.inc`）:
  - 先に手で書き換えた C を実機で測った（`build` の外に残していない。和・積和・ふるいのループ）: 一番効いたのは、ループの中で毎回くり返していた大域の配列の番地の計算（adrp・add）を外に出すこと（和 65.8 → 32.9ms）。反転だけは最初は効かなかったが、原因は SSA を抜けるときに分けた空のブロックで戻りの分岐が 2 つになっていたことで、それを直すと効いた。
  - LICM（`ir_licm`）: 戻る辺から自然ループを見つけ、内側から、副作用がなく止まらない整数の計算・定数・番地で入力がループの外のものを、プリヘッダーへ（前任が条件分岐なら辺を分けて作る）。割り算・浮動小数点・ロード・比較は動かさない。1 ループで 6 個まで。関数の呼び出しを含むループには使わない（退避の要るレジスタが足りず、スタックに逃がしてかえって遅くなった。strhash の main）。
  - 分岐: 中身が分岐だけ（何も出さない命令はあってよい）のブロックを飛ばして先へ直接飛ぶ（`ir_thread`）、落ちる先は間の空のブロックを通った先（`ir_fallthrough`）。
  - 反転（ND_FOR）: 条件を前で 1 回、後ろでもう 1 回。条件の式が小さく、ラベル・文の式・呼び出し・メモリの読み出しを含まないとき（`ir_dup_ok`。メモリを読む条件は、反転すると本体の同じ読み出しを省けなくなり、hash がインライン展開の大きさを超えた）。
  - 展開（`ir_unroll_for`、`-funroll-loops`（既定）・`-fno-unroll-loops`）: `for (...; V op E; V += s)`（V は SSA の符号つきの整数、s と op の向きがそろう、E は定数か書き換えない SSA の変数）で、本体がループ・ラベル・switch・asm などを含まず小さい（40 節点）とき、2 回分ずつ回し、後ろで (V + s) op E だけを調べる。残りは V op E なら 1 回。符号つきのあふれは元でも未定義動作なので、本体の回数は元と同じ。符号なしは展開しない。インライン展開の大きさは、2 か所目以降に出した分を除いて数える（`ir_dup_range`）。
  - レジスタの割り当て（並べ替えのため）: ヒントのない値は、空いたばかりのレジスタを避け、空いてから長いものを選ぶ。ループのある関数では、ほかに空いたばかりのものしかなければ、退避の要るレジスタも新しく使う（`IR_REUSE_GAP`）。並べ替えはどこからも飛んでこないラベルで区切らない（`SCH_FREE_LABEL`）。
  - 効果（実機）: nbody 4.98 → 3.68 秒（並べ替えの前は 5.66）、配列の和 65.8 → 20.6ms、積和 81.8 → 55.6ms、ふるい 1460 → 1377ms、primes（ベンチマーク）4.20 → 4.11 秒、strhash 3.45 → 3.43 秒、samples/primes.c 47 → 45ms、primes_mc 2.366 → 2.371 秒。Pi 5 の bench.sh で nbody 447 → 404ms（GCC -O2 は 300ms）。
  - 全部既定で使う（2026-11-15、ユーザーと合意。ループの計算が 1.3〜3 倍速くなり、コンパイルの増えた分は 2 回目からキャッシュで見えないため）。
  - コンパイルの時間（実機、`cc -c -o /dev/null`）: 並べ替えも含めて、全部入れる前より parse.c 525 → 558M サイクル（+6%）、codegen.c 791 → 856M（+8%）。Pi 5 で自分のソース一式 604 → 644ms。増えた分は、展開・反転でコードが増え、後の全部の段の仕事が増えたため（新しい部分そのものはどれも 1% 未満）。
  - 確かめる: `make cc-test` の 8b（`cc/test-a64/loop_gen.py`: いろいろな型・向き・増やす量・境界・continue・break・return のループ 300 個 × 6 通りの引数を、GCC と同じ結果か。種 4 つ、既定と `-fno-unroll-loops -fno-schedule-insns2`）。確認: make cc-test、verify.sh（全部。セルフホストの一致、libctest）、shell・pthread・openmp・linuxapi、hwcheck（両方）。
- 葉の関数のフレームを省く（2026-11-15。ユーザーの「ほかに GCC との差を縮める方法は」から。`cc/ir.inc` の ir_is_leaf）: 呼び出しになりうる命令（呼び出し・大きな memcpy・memset・TLS・asm・errno のために sqrt を呼ぶ平方根・構造体の引数）がなく、スタックの上の変数・逃がした値・退避するレジスタもない関数は、`stp x29, x30`・`mov x29, sp`・`mov sp, x29`・`ldp` を出さない（GCC と同じ）。slimcc 自身のソースで tokenize.c の 207 関数のうち 156。slimcc 版のカーネルが 7.8KB 小さくなり、その中のコンパイラが実機で約 1.5% 速くなった（parse.c 1016 → 1001M サイクル、codegen.c 1556 → 1529M。省くあり・なしのカーネルを交互に）。Pi 5 では変わらない（順不同に実行するので待ちが隠れる）。perf record -g の呼び出し元は x30 からわかるので、そのまま出る。
  - 先に測ったこと（実機の perf record）: strhash は main（インライン展開した hash）44%・strcmp 29%・printf 12%（strcmp・printf はカーネルの GCC のコード）、nbody は全部 main。どちらも熱いところはインライン展開済みで、葉の省略は効かない。GCC との差が残るのは浮動小数点の計算（Pi 5 の bench で nbody 404 対 300ms。GCC もベクトル化と積和をしないと 444ms だった）。
- slimcc 版のカーネル（中のコンパイラ）を速くする（2026-11-15。ユーザーの「できればすべて Monocs の中で完結したい」から）:
  - 調べ方: 実機で 2 つのカーネルの `perf record -e cycles cc -c -o /dev/null /src/cc/parse.c` を比べた。差の大きいもの: vfprintf（slimcc 版で 2.7 倍）、open_memstream の 1 文字ごとの `__open_mem_put`・`__open_mem_grow`、ir_linear_scan（2.4 倍）、`__slimcc_bitint_mul`。中間表現で扱えず今までのコード生成になる関数を `SLIMCC_IR_WHY=1 make kernel-slimcc` で数えた（カーネル 76 個）。
  - 中間表現で扱うようにしたもの（`cc/ir.inc`）: 配列の代入（局所変数の配列の初期化 `char buf[N] = "..."`。前処理の expand_tokens など 22 関数が丸ごと今までのコード生成だった）、va_arg・va_copy（`ir_va_arg`。gen_va_arg と同じ手順。va_start のある可変長引数の関数そのものは今までどおり。HFA の va_arg も）、`__builtin_add・sub・mul_overflow`（`ir_ckd_arith`。2 つの入力と結果が同じ 32・64 ビットの整数のとき。64 ビットの符号つきの掛け算は除く。それまでは picolibc の fwrite・strtoull が丸ごと今までのコード生成で、`_BitInt` の掛け算を呼んでいた）。レジスタに置いた変数の番地を取る形（`(int){3} = 5`）は内部エラーにせず、今までのコード生成に任せる（配列の代入を扱うようにしたら、test/complit.c で表に出た）。vfprintf は long double を使うので、まだ今までのコード生成。
  - `os/libc/memstream.c`: open_memstream を picolibc のバッファ付きの FILE（stdio-bufio.h の FDEV_SETUP_BUFIO、関数のポインタ）で作り直した（picolibc より先にリンク。third_party は無改造）。4KB ごとに書き出し、fwrite・fputs は memcpy。`tools/test/memstream/mstest.c`（書く・ftell・前に戻って上書き・終わりより先・1MB）が実機で Linux と同じ。
  - `cc/codegen.c` の emit_out: 書式のない文字列は fputs、書式が %s・%c・%d・%i・%u・%x とその l・ll だけなら自分で文字列を作って fwrite（ほかは vfprintf）。出力の .s は 1 バイトも変わらない（前後のコンパイラで比べた）。
  - 効果（実機、`cc -c -o /dev/null`、葉の関数の省略を含む）: slimcc 版 parse.c 1016 → 843M サイクル（-17%）、codegen.c 1556 → 1285M（-17%）。GCC 版 558 → 521M、856 → 804M（-7%）。slimcc 版と GCC 版の差は 1.82 → 1.62 倍。
  - 確かめる: make cc-test の 8c（`cc/test-a64/gccmatch/*.c`: va_arg・あふれの確かめが GCC と同じ結果）。確認: verify.sh（全部）、shell・pthread・openmp・linuxapi、hwcheck（両方）。
  - その続き（2026-11-15）: 実機の命令数で比べると、命令あたりのサイクルは同じ（0.74・0.76）で、slimcc 版は命令が 1.66 倍（命令キャッシュのミスも 2.2 倍）。`perf record -e inst` で多いところを直した:
    - 内蔵アセンブラ（`cc/assembler.c`）が命令の名前を `strcmp` の連なり（196 か所）で判定していて、全部の命令の 12.5% が strcmp だった（GCC 版でも約 2 割）。`MN_IS("add")`（1 文字目が違えば strcmp を呼ばない）に置き換え、表を引くループも同じく。parse_reg は x12・w9・d16 の形を先に自分で読む（strcmp・strtol を呼ばない）。
    - 書式の処理を `cc/strings.c` の slimcc_vformat（%s・%c・%d・%i・%u・%x と l・ll・%% を 1 回でたどる。ほかは知らせて vsnprintf でやり直す）にまとめ、emit_out と、slimcc.h の `#define snprintf slimcc_snprintf` でコンパイラの中の snprintf 全部が使う。出力の .s は変わらない。
    - 効果（実機）: slimcc 版 parse.c 843 → 726M、codegen.c 1285 → 1122M。GCC 版 521 → 450M、804 → 704M。今日の始め（slimcc 版 1016・1556M、GCC 版 558・856M）から、slimcc 版は約 29% 速く、今日の始めの GCC 版の 1.30 倍。今の GCC 版との差は 1.6 倍のまま（同じ改良で GCC 版も速くなった）。
- 機械語を直接作る形への作り替え（2026-11-16。ユーザーと合意: 「コンパイラがアセンブリの文字列を書き、内蔵アセンブラが読み直す」流れを、中間表現の関数について、命令の記録から機械語を直接作る形にする。段階 1: 記録の形、段階 2: 記録から機械語、段階 3: 対応を広げて測る）:
  - 調べたこと: parse.c のアセンブリの文字列（約 790KB）は、中間表現の関数の命令 60%・古いコード生成の命令 13%・ラベル 14%・ディレクティブ 13%。実機の slimcc 版では、内蔵アセンブラ約 14%・文字列を作る約 7%。見込み: 段階 2 までで 1 割強速くなる。
  - 段階 1（済み）: 中間表現の関数の出力を、文字列の流れではなく記録の並び（`cc/ir.inc` の IrRec）にためる。命令は型付きの記録（IRR_INSN: 命令の名前と AOp のオペランド。AO_REG・AO_IMM・AO_SHIFT・AO_COND・AO_TEXT（ラベル）・AO_SYM（"name"・:lo12:・:got:）・AO_MEM（[b, #off]・[b, x, lsl #k]・前置・後置・:got_lo12:）。E0〜E4 で作る）。型付きの補助（ir_srcA・ir_dstA・ir_fsrcA・ir_memop・ir_add_imm・ir_mov_imm・ir_e_symaddr・ir_call_sym・ir_emit_extA・ir_labelA）。文字列の行のまま残したもの（IRR_LINE。emit_out が ir_rec_on のとき ir_rec_text で行に分けて記録する）: ラベル、並べ替えの印、アトミック（cas・xchg）、構造体の語ごとの読み書き（load_gp_exact など）、memcpy・memset、TLS、インラインアセンブリ（本体は output_file に直接書くので、作業用の文字列に受けてから記録に移す。ir_rec_take_side）。関数の終わりに ir_rec_write が書く（ループの中の範囲は sch_lines で並べ替える。今は型付きの記録も文字列にしてから渡す）。途中であきらめたとき・エラーで抜けたときは ir_rec_abort。ao_print は今までの Printftn と 1 文字も違わない文字列を作る（sprintf は picolibc の vfprintf を通るので使わない）。
  - 確かめ方: 書き換えの前のコンパイラと今のコンパイラで、いろいろなファイル（コンパイラ・カーネル・test/・ベンチマーク）の `-S` が 1 バイトも変わらないこと（build/schedwork/cmp_s.sh を使った）。見つけた誤り: 分岐の条件の名前の表が 10 個で、浮動小数点の比較の条件（mi・pl など。cc_name は 15 個）で表の外を読むところだった（_Static_assert を足した）。
  - 速さ: 段階 1 だけでは、記録に写す分だけ遅い（実機 slimcc 版 parse.c 655 → 674M、GCC 版 411 → 422M）。段階 2 で取り返す見込み。
  - 段階 2（2026-11-16）: 中間表現の関数の記録を「かけら」にして、内蔵アセンブラが文字列を経ずに読む。
    - 同じプロセスで .o まで作るとき（opt_direct。libslimcc（Monocs の中のコンパイル）と、Linux のドライバの `-c`（内蔵アセンブラ、-g・-M・-MD などなし。`-fno-slimcc-direct` で今までの道））。関数の出力（ir_rec_write）は、並べ替えた後の順の記録をゾーンの外に写して（frag_make）、アセンブリの文字列には `.slimcc_frag 番号` だけを書く。内蔵アセンブラは、その文で記録を読む（assemble_frag）。
    - よく出る命令（mov・movz・movk・movn、ldr・str の仲間、ldp・stp、add・sub・adds・subs（即値・:lo12:・シフトなしのレジスタ）、cmp・cmn、adrp、b・bl・b.cond・cbz・cbnz、ret、sxt*・uxt*）は、記録から直接組み立てる（frag_insn。オペランドの文字列を作って読まない。組み立ての本体は文字列の道と共有: enc_ldst_mem・enc_ldstp_mem・enc_addsub_immv・enc_mov_imm・emit_bitfield）。ほかの型付きの記録はオペランドごとに文字列にして assemble_insn へ、文字列の行（ラベル・インラインアセンブリなど）は文に分けて読む（assemble_chunk。インラインアセンブリは 1 行に ; で複数の文を書ける）。
    - 文字列の道と同じ結果にするため: codegen.c の peep_redunt_jmp（「b L」のすぐ後のラベルに L があれば消す）を、かけらの記録にも同じようにかける（frag_peep_jmp。かけらの中は文字列の探索から見えないため）。Linux のドライバでは、関数は構文解析の中で出力されるので、opt_direct は parse の前に決める（最初は codegen の前に立てていて、かけらの道を通っていなかった。比べた結果がすべて同じだったのは、両方とも文字列の道だったため。比べる試験は、かけらの道を通っていることも確かめる）。
    - 確かめ方: `build/schedwork/cmp_o.sh`（cc/ で。コンパイラ・test/・ベンチマーク・カーネルの一部など 111 ファイルの .o が、かけらの道と `-fno-slimcc-direct` で 1 バイトも変わらない）、make cc-test、verify.sh（Monocs の中のセルフホストのビルド（かけらの道）が Linux 上のビルド（カーネル・picolibc は -MMD なので文字列の道）と一致）。
    - 速さ（Pi 5、slimcc の 5 ファイルの -c）: 文字列の道 408〜413ms → かけら 373〜378ms（約 8.5%）。
    - 速さ（実機、`perf -e cycles cc -c -o /dev/null`、3 回の最小）: slimcc 版 parse.c 674 → 599M、codegen.c 1051 → 935M（段階 1 の後から約 11%、段階 1 の前の 655・1022M から約 9%）。GCC 版 422 → 380M、666 → 602M。slimcc 版と GCC 版の差は 1.58 倍。
    - 確認: cmp_o.sh（111 ファイル）、make cc-test、verify.sh（全部。セルフホストの一致、gen2、libctest）、shell・pthread・openmp・linuxapi、hwcheck（GCC 版・slimcc 版）。
  - 段階 3（2026-11-16）: かけらの対応を広げ、実機の perf record で目立つところを直した。出力は変わらない（`build/schedwork/cmp_s.sh` で HEAD のコンパイラと -S が同じ、cmp_o.sh で .o が同じ）。
    - かけら: 256 バイトまでの構造体のコピー・0 埋め（IR_MEMCPY・IR_MEMZERO）を型付きの記録に（-S の `[x, #0]` を変えないよう、0 でも #0 を書く番地の形 AOM_OFF0）、`add`・`sub` の `#N, lsl #12`、ラベルだけの行は文として読まずに定義する。残る文字列の道は、codegen.c で全体の約 2%（cset・レジスタの lsl・mul など）。
    - 確保: ir_grow は前の中身を写す部分を 0 にしない（ir_alloc_nz）、記録の配列は関数をまたいで使い回す（ir_rec_reserve。コンパイルの終わりに ir_pool_forget で忘れる）、記録の文字列の塊・ir_cse の chain も 0 にしない、ir_cse の使っていない配列を消した。
    - ir_load_elim: ブロックの出口の表は使った分だけ持つ（前は 1KB の表を丸ごと写し・0 にしていた）、表がいっぱいのときは窓を進めるだけ（前は毎回 64 個をずらしていた）。
    - レジスタの割り当て: 生存区間は作る間は逆の順に持ち最後に 1 回並べ直す（前は先頭に足すたびに全体をずらしていた）、ir_covers・ir_next_intersection は終わった区間を飛ばす（rcur）、free_until は使うレジスタの組の分だけ決める、退避の要るレジスタかを表で引く（ir_callee_tab。ヒントの相手のレジスタは組に入っているかを ir_order_tab で確かめる）、ビット集合の番号は de Bruijn の表で（ir_ctz）。
    - ラベルの文字列を snprintf を通さずに作る（ir_label）。内蔵アセンブラの記号の表は、文字列の長さとかけらの記録の数から大きさを見込んで最初から確保する（hashmap_reserve。何度も作り直していた）。
    - 速さ（実機、段階 2 の後 → 段階 3）: slimcc 版 parse.c 599 → 534M、codegen.c 935 → 834M（約 11%）。GCC 版 380 → 339M、602 → 538M。今日の始め（段階 1 の前: slimcc 版 655・1022M、GCC 版 411・652M）から、slimcc 版は約 18% 速い。slimcc 版と GCC 版の差は 1.57 倍。
    - 残りの時間（slimcc 版、perf record）は広く散らばっている: レジスタの割り当て（ir_linear_scan 4%・ir_intervals 2%）、名前の表（fnv_hash・get_entry 5%）、字句解析、中間表現の各段。
  - その続き（2026-11-16。ユーザーの「もう少し続けて縮めることできる？」から）:
    - コンパイラの中: トークンに名前のハッシュを覚える（Token の hash。詰め物の 4 バイト。tok_hash・hashmap_get2h。マクロ・キーワード・変数のスコープごとの表を引くたびに FNV を計算し直していた。位置・長さを書き換える所（UCN の変換、EOF・印のトークン）で 0 に戻す）、equal は 1 文字目が違えば strlen を呼ばない、equal_ext は snprintf で `__名前__` を作らない、アセンブラの tmp_format は slimcc_vformat で、ディレクティブの名前は 2 文字目で外す（DIR_IS）、かけらの並び（frag_order）を使い回す、open_memstream は 4KB の入れ物を 0 にしない（os/libc/memstream.c）。
    - slimcc の出すコード（slimcc 版のカーネルの中のコンパイラが速くなる。出力は変わる）:
      - switch: case の範囲が 5 個以上なら、下端の順に並べて二分探索（ir_switch_tree）。前はいつも上から順に比べていた（50 個なら平均 25 回）。試験は make cc-test の 8d（`cc/test-a64/switch_gen.py`: 型・端の値・範囲・default の有無・落ちる流れの switch 300 個 × 種 4 つを GCC と比べる）。
      - TLS の番地: スレッドポインタを別の命令 IR_TP（mrs tpidr_el0）にし、番地は TP + 変数のオフセット（local-exec は :tprel_hi12:・:tprel_lo12_nc: の add 2 つ、initial-exec は GOT から x17 に読んで足す）。CSE で関数の中の mrs が 1 つにまとまり、LICM でループの外に出る。前は変数を使うたびに mrs・add・add・mov の 4 命令を x0 で作り、ループの中でもくり返していた（カーネルの slimcc は状態がすべて TLS）。IR_TLSADDR は葉の関数の判定から外した。
      - 葉に近い関数（関数を呼ばず、x0〜x7 を作業用に使う命令（CAS・XCHG・インラインアセンブリ・memcpy・構造体の引数）がない。ir_low_ok）では x1〜x7 も割り当てる（ir_alloc_order_low。x0 は番号 0 が「なし」なので使わない）。引数 k は xk にだけ置け、空いていれば xk のまま使う（mov なし）。ほかの値は引数を全部取り出した後に始まるものだけ。前は小さな葉の関数（fnv_hash など）でも x19〜 を使い、入口と出口で保存・復元していた。
      - どこからも飛んでこず、前から落ちてもこないブロック（分岐を飛び越すようにして使われなくなった辺）を出さない。slimcc 版のカーネルが 31KB 小さくなった。
    - 速さ（実機、`perf -e cycles cc -c -o /dev/null`、3 回の最小）: slimcc 版 parse.c 534 → 490M、codegen.c 834 → 775M（約 8%）。GCC 版 339 → 327M、538 → 523M。slimcc 版と GCC 版の差は 1.57 → 1.50 倍。今日の始め（slimcc 版 655・1022M）から slimcc 版は約 25% 速い。内訳（slimcc 版の parse.c）: コンパイラの中の改良 534 → 523M、switch 523 → 515M、TLS 515 → 501M、x1〜x7 501 → 498M、届かないブロック 498 → 490M。
  - さらに続き（2026-11-16。ユーザーの「お願いします」から。slimcc の出すコード）:
    - 型による別名の判定（strict aliasing。GCC・clang の -O2 と同じ。`-fno-strict-aliasing` で切る。run・compile_flags.txt・Makefile の CFLAGS・シェルの cc・SlimccOptions の no_strict_aliasing）: 読み書きに型の組（ir_tbaa: 大きさの同じ整数（符号は問わない）、ポインタ、float、double。char・_Bool・ビットフィールドなどは 0 で何とでも重なる）を付け、組の違うものは重ならないとみなす（ir_may_alias。土台が同じ読み書きは前の判定が先なので、共用体の読み替え・`*(int *)&f` のような同じ局所変数の読み替えは今までどおり）。GCC 版のカーネルも picolibc も GCC の -O2・-O3（strict aliasing あり）で作っているので、コードはこの規則を前提にしている。
    - ループの外への読み出しの移動（ir_licm_load）: 番地がループの中で変わらず、ループの中に重なりうる書き込み・アトミック・errno を書く sqrt・インラインアセンブリの出力がなく、必ず読める番地（局所変数・TLS・弱くない大域変数）か、ループを出るまでに必ず通るブロックの読み出し。呼び出しを含むループは今までどおり何も動かさない。
    - 下位 32 ビットだけを使う拡張（mov になる）は、元の値と同じレジスタをヒントにする（int の引数を写す mov が消える）。
    - 直した不具合（今日の変更で表に出た）: 添字が定数 0（xzr、番号 31）の番地を、文字列にする ao_print が表の外を読んでいた（`-S` と `-MMD` の付いたカーネルのビルドで落ちた。かけらの道は xzr として正しく組み立てていた）。検査付き（ASan・UBSan）のコンパイラで、コンパイラ・test/・カーネルの一部の -S・-c を通して見つけた前からの未定義動作 2 つ（長さ 0 の memcpy に NULL、ビットフィールドのマスクの符号つきのシフト）も直した。
    - 試験: `cc/test-a64/gccmatch/alias.c`（許される別名と、外に出してはいけない読み出しを GCC の -O2 と比べる。make cc-test の 8c）。手引き（docs/programming-guide.md）に strict aliasing を書いた。
    - 速さ（実機、slimcc 版）: parse.c 490 → 486M、codegen.c 775 → 764M（1% 前後）。出すコードの細かい直しは、1 つあたり 0.2〜1% まで小さくなった。
- 標準のカーネルを slimcc 版にした（2026-11-16。ユーザーの方針「Monocs の中で完結させたい」。それまでは GCC 版が普段のカーネルだった）:
  - `make`（all）・`make sdimage`・`make sdcard`・`make qemu`・`nos.py kernel`・QEMU の試験（shell・pthread・openmp・linuxapi.sh）の既定を build/kslim/kernel8.img に。GCC 版は `make kernel-gcc`・`make qemu-gcc`・`KERNEL=build/kernel8.img tools/mksd.sh`。hwcheck は GCC 版 → 標準の順に送り、最後に標準が残る。verify.sh は両方を作って両方で確かめる（今までどおり）。
  - 実機の上での作り直しと起動を確かめた: 標準のカーネルの上で `cd /src`、`run buildlibc.c`（43 秒）、`run build.c`（8 秒）。できた libc.a・kernel8.img は Linux 上のビルドと一致。それを /boot/kernel8.img に写して再起動し、起動した（前のものは /work/kernel8.prev）。手順は docs/resume.md の 4。
  - 切り替えで見つけた不具合（前からあった）: シェルの `"$@"` と `~` の展開が slimcc 版のカーネルで動いていなかった（shell.sh の 5 行が bash と違った）。`put_raw(…, "HOME", "HOME" + 4)` のように、同じ文字列リテラルを 2 回書いて範囲の始めと終わりにしていた。GCC は同じ文字列リテラルを 1 つにまとめるので動いていたが、slimcc はまとめない（C の規格ではどちらでもよい）。同じ配列から取る形に直した。QEMU の試験を GCC 版でしか流していなかったので気づかなかった（今は既定が標準の slimcc 版）。
- GPIO（2026-11-16。ユーザーの「GPIO を使ったプログラムを作れるようにしておきたい」から。方針どおりライブラリ型。関数の形は、Linux の libgpiod 互換ではなく Monocs の簡単な関数にした（ユーザーが選んだ。Linux の互換の層は、動かしたい Linux のプログラムが出てきたら上にかぶせる））:
  - `/lib/gpio`（`lib/gpio/gpio.c`・`monocs_gpio.h`。`-lgpio`）: gpio_mode・gpio_get_mode・gpio_pull（6.1 節の GPPUD・GPPUDCLK0 の手順）・gpio_write・gpio_read・gpio_toggle・gpio_read_all・gpio_write_mask・gpio_wait_edge・gpio_pin_ok。レジスタを直接触る。変えられるのは GPIO0〜27 から 14・15（コンソールの UART）を除くもの。ヘッダーの外（LED 29、Wi-Fi 34〜39・41、SD 48〜53 など）は GPIO_PIN_MAX で外れる。
  - エッジは同期のエッジの検出（GPREN0・GPFEN0）を待つ間だけ入れ、GPEDS0 の印を調べ続ける。GPU の GPIO の割り込み（49〜52）は OS が許可していないので、印が立つだけ。Ctrl-C で止まると GPREN・GPFEN は入ったまま残るが、印が立つだけで害はない。
  - 気をつけること: GPPUD・GPPUDCLK の手順は OS の sdio.c（Wi-Fi を始めるとき、コア 3 の wifid から）と同じレジスタを使い、同時に動くとプルが誤ったピンに当たりうる（ロックはない。起動の直後だけなので見送り）。GPFSEL の読み書きは、アプリが触る GPFSEL0〜2 のうち OS は起動時（UART・LED）にしか書かない。
  - `/bin/pinctrl`（Raspberry Pi OS の pinctrl と同じ書き方: get・set（ip op a0〜a5 pu pd pn dh dl）・lev・funcs・poll、-p・-e。出力の形も同じ。名前は Zero 2 W のデバイスツリーの gpio-line-names、機能の名前は Pi 5 の `pinctrl -c bcm2835 funcs`）。違うところ: プルの欄はいつも `--`、no（機能なし）はない、poll の出力の形は Monocs のもの。
  - `/usr/bin/gpio`（2026-11-20、ユーザーの「gpio readall コマンドみたいなのがあったらいい」から。`bin/gpio/`）: WiringPi の gpio と同じ書き方。`gpio readall`（40 ピンの表: BCM・wPi の番号、名前、機能、値。表の形は記憶にある WiringPi 2.5x の readall に合わせた。Pi 5 に WiringPi がなく本物とは比べていない）、`gpio [-g|-1] read・write・toggle・mode PIN`（番号は既定が wPi、-g が BCM、-1 がヘッダーのピン。mode は in・out・up・down・tri・alt0〜5）。実機で readall（電源を入れたときの既定どおり: GPIO0〜8 が 1、9〜27 が 0、14・15 が ALT0）、GPIO5 の out・write・toggle・read、GPIO14 を変えようとすると EBUSY。
  - 例 `samples/gpio_blink.c`。確かめる: `sh tools/test/gpio.sh`（QEMU。QEMU の GPIO は機能・出力・値だけ）、実機は `run -lgpio /work/gpiotest.c hw`（GPIO5・6 でプル・エッジも。2026-11-16 に 29 項目すべて ok）。
- I2C（2026-11-16。ユーザーの「I2C のライブラリも同じ形で」から。GPIO と同じくライブラリ型・Monocs の簡単な関数）:
  - `/lib/i2c`（`lib/i2c/i2c.c`・`monocs_i2c.h`。`-li2c`。compile_flags.txt に `-lgpio`）: BSC0（0x3F205000、GPIO0・1）と BSC1（0x3F804000、GPIO2・3）のマスター。ポーリング。i2c_open・close・write・read・write_read・read_reg・write_reg・read_regs・write_regs・probe。分周・DEL・CLKT は Linux の i2c-bcm2835.c と同じ決め方。BSC のクロックはコアのクロックで、400MHz と決め打ち（config.txt の force_turbo=1 で固定。アプリからメールボックスで聞く API はない。コアのクロックが変わるとバスの速さも変わる）。
  - リピーテッドスタート（i2c_write_read）: 資料 3.3 の 10 ビットのアドレスの読み出しの手順（書きを始める → TA を待つ → 書きが終わる前に READ と ST）。書くものは TA の後に FIFO に入れる（QEMU の BSC は転送の外の FIFO への書き込みを捨てる）。その間は割り込みを止める（最大 2ms）。
  - 片付けで C に I2CEN を立てない（QEMU は I2CEN だけでも転送を始める。最初の版はこれで i2cdetect に幻のアドレスが出た）。
  - `/bin/i2cdetect`・`i2cget`・`i2cset`（i2c-tools 4.4 と同じ書き方・文・出力。Pi 5 の i2c-tools で確かめた）。共有の部品は `lib/i2c/monocs_i2c_tools.h`（static の関数）。i2cdetect はいつも 1 バイト読んで調べる（長さ 0 の書き込みが BSC で正しく出るかを確かめていない）。
  - 確かめる: `sh tools/test/i2c.sh`（QEMU に at24c-eeprom と tmp105 をつなぐ。qterm.py の `QEMU_EXTRA`。QEMU 10 の bcm2835_i2c はリピーテッドスタートを真似ない）。QEMU の tmp105 は -device の temperature が効かない（読むと 0）ので、電源を入れたときの T_LOW（0x4B00）で比べる。
  - 実機（2026-11-16）: `run -li2c -lgpio /work/i2ctest.c empty`（何もつないでいないバス 1）で NACK（読み・書き・書いて読む）、SCL の速さ 94kHz（100kHz の設定）・375kHz（400kHz）、i2cdetect -y 1 が全部 --。`clock` の CORE は 400MHz。**実際の機器でのリピーテッドスタートはまだ確かめていない**（機器がない）。
  - あわせて: Makefile の sdcard.img の依存に `bin/*/*`・`lib/*/*` を足した（足りず、lib/ を直しても `make sdimage` が古いイメージのままだった）。
- SPI（2026-11-16。ユーザーの「SPI のライブラリも同じ形で」から。GPIO・I2C と同じくライブラリ型・Monocs の簡単な関数）:
  - `/lib/spi`（`lib/spi/spi.c`・`monocs_spi.h`。`-lspi`。compile_flags.txt に `-lgpio`）: SPI0（0x3F204000）のマスター。ポーリング（資料 10.6.1）。spi_open・close・get_hz・transfer・write・read・write_read。mode は Linux の spidev と同じ値（CPHA・CPOL・CS_HIGH・NO_CS。LSB_FIRST は EINVAL）。分周は Linux の spi-bcm2835.c と同じ（切り上げて偶数、65536 以上は 0）。CS_HIGH は CS の CSPOL と CSPOLn。両方の CE の CSPOLn を残す（使っていない側がつかないように）。TA の前に CPOL を書く（SCLK の休みの値を先に）。受ける FIFO をあふれさせないよう、先に送るのは 16 バイトまで。補助の SPI1・2（mini UART と同じ AUX）はない。
  - `/bin/spi`（Monocs のコマンド。Linux の spidev_test・spi-tools は Pi 5 になく、比べられないので形を合わせていない）: `spi [-c CS] [-s HZ] [-m MODE] [-H] [-N] [-r N] BYTE...`（バイトは 16 進、オプションはどこでも）。
  - 確かめる: `sh tools/test/spi.sh`（QEMU の SPI0 に `-device w25q32,bus=spi`。QEMU 10 の ssi は、CS の線をつながない CS_LOW の機器をいつも選ばれたままにするので、フラッシュに通じるのは起動ごとに最初の命令だけ。JEDEC ID 0xEF4016 を読む）。実機は `run -lspi -lgpio /work/spitest.c hw`（何もつながない。コア 1 が gpio_read_all で SCLK・MOSI・CE0 の変化を記録し、送った 0xA5 0x3C・モード 0〜3・CS_HIGH・速さを確かめる。2026-11-16 に 81 項目すべて ok。100kHz の設定で 93kHz（バイトの間の空きを含む）、500kHz で 483kHz）。
  - つまずいたこと: nos.py cmd は出力の「> 」をプロンプトと見て止まる（試験の項目名「3MHz -> …」で途中で終わった）。実機で流す試験の出力に「> 」を書かない。
- UART（2026-11-16。ユーザーの「UART のライブラリも同じ形で」から。BCM2837 の UART は PL011 と mini UART だけで、ヘッダーに出せるのはどちらも GPIO14・15（コンソール）なので、ユーザーに選んでもらい、ソフトウェアの UART にした。mini UART を GPIO14・15 で使う案（コンソールの線を外す）は選ばなかった）:
  - `/lib/softuart`（`lib/softuart/softuart.c`・`monocs_softuart.h`。`-lsoftuart`。compile_flags.txt に `-lgpio`）。名前はコンソールの OS API `uart_putc` などとぶつからないよう softuart_*。8N1、300〜115200bps、4 つまで。
  - 時間は CNTPCT_EL0（実機 19.2MHz）を直接読み、ビットの境目は始めからの通し番号で計算（ずれが積もらない。1 ビットのカウント数 × 65536 で持つ）。送信は呼んだコアで、1 バイトの間は割り込みを止める。受信は空いたコアを後ろから探して `monocs_core_start` で裏の受信のループ（休みの High → 立ち下がり → 各ビットの真ん中で読む。ストップビットが Low ならフレームの誤りに数える）、4KB の輪（stdatomic の acquire・release）。空いたコアがなければ read・getc の中でだけ受ける。
  - `/bin/softuart`（Monocs のコマンド）: 端末のように使う（Ctrl-] で終わる）か、`-x TEXT [-w MS]` で送って返ってきたものを出す。
  - 確かめる: `sh tools/test/softuart.sh`（QEMU。使い方だけ。QEMU は仮想の CPU を交互に動かし、2 つのコアの時間が合わないので、1200bps でも受信が崩れた）。実機は `run -lsoftuart -lgpio /work/sutest.c hw`（線なし。TX と RX を同じ GPIO5 にする。2026-11-16 に 9600・38400・115200bps で 256 種類の値をすべて取りこぼしなく受け、256 バイトの時間は計算と 0.2% 以内（ただし時間を測る時計も同じ CNTPCT なので、外の機器との速さの一致は確かめていない））。`softuart -x` も実機で確かめた。
- PWM（2026-11-16。ユーザーの「PWM のライブラリも同じ形で」から。ライブラリ型・Monocs の簡単な関数）:
  - `/lib/pwm`（`lib/pwm/pwm.c`・`monocs_pwm.h`。`-lpwm`。compile_flags.txt に `-lgpio`）: PWM（0x3F20C000）の 2 チャンネルを M/S のモード（RNG のうち DAT 個 High）で。ピンは 12・13（ALT0）、18・19（ALT5）。pwm_set・set_ns・servo・set_polarity・off・get。
  - クロック: CM_PWMCTL・CM_PWMDIV（0x3F1010A0・A4。資料になく、Linux の clk-bcm2835.c の番地）を水晶（SRC 1、19.2MHz）・DIVI 2 で 9.6MHz に。資料 6.3 の手順（ENAB を外して BUSY が消えるのを待ち、分周を書き、ENAB）。もう 9.6MHz なら触らない（もう一方のチャンネルを乱さない）。変えるときは PWM を止めてから戻す。RNG・DAT の後に 1µs 待つ（続けて書くと BERR になることがある、という言い伝えによる。確かめていない）。
  - 状態はレジスタだけに持つ（アプリが終わっても波は出続け、`pwm` コマンドでも見られる）。pwm_off はピンを休みの値の出力にする（浮かせない）。
  - 確かめる: `sh tools/test/pwm.sh`（QEMU は PWM を真似ていない（読むと 0、書いても無視）が CPRMAN は真似ているので、クロックの設定は QEMU でも確かめる）。実機は `run -lpwm -lgpio /work/pwmtest.c hw`（何もつながない。2026-11-16 に全部 ok: 1kHz 25% が 1000.00Hz・250.0µs、サーボ 50.00Hz・1500.0µs、100kHz 50%、極性、同じチャンネルの 2 本、50kHz 25%、duty 0・1）。`pwm` コマンドも実機で確かめた。
  - SG90（サーボ）をつないで確かめた（2026-11-16、ユーザーが動きを見た。Wi-Fi だけで操作）: 茶 = 6 番（GND）、赤 = 2・4 番（5V。Zero 2 W と同じ電源で、電圧の低下（throttled）は出なかった）、橙 = 12 番（GPIO18）。`pwm 18 servo 1500` で真ん中、`run -lpwm /samples/servo.c`（1000〜2000µs）と `… 18 500 2400`（唸らずに端から端まで）。例 `samples/servo.c`（真ん中 → 端 → 反対の端 → 真ん中、最後にパルスを止める。`PIN US` で出し続ける）。
- Wi-Fi だけで開発する（2026-11-16。ユーザーの「Wi-Fi だけで接続して開発を続けることは可能？」から、ユーザーと合意して）:
  - ssh の exec（`ssh monocs@アドレス 'コマンド'`）: `os/kernel/rexec.c`（OS API の monocs_rexec_start・read・write・write_eof・status・release）。sshd（コア 3）が行を頼み、シェルがプロンプトで入力を待つ間（lineedit.c の read_byte_wait から呼ぶ idle の関数。shell_main がプロンプトとその続きの行の間だけ置く。コマンドの中の行の編集（`run -` など）では呼ばない）に shell_rexec_poll が取り出して動かす。サブシェルと同じく cd・変数・関数は戻し、$?・打ちかけの行（行の編集）・画面は変えない。標準出力・標準エラーは出力の輪（64KB）、標準入力は入力の輪（16KB）に向ける（syscalls.c の out_rexec・in_rexec・err_rexec。リダイレクトの土台にし、行の中のリダイレクトが勝つ。isatty は偽なので ls は 1 行に 1 つ）。出力の輪がいっぱいなら sshd が読むまで待つ（10 秒読まれなければ捨てる）。ssh が切れたら（rexec_release）Ctrl-\ と同じく止める（uart_raise_interrupt。SIGINT の関数があっても止まる）。exec の中の `exit N` は行を終えて N を返す。同時に 1 つ。シェルがコマンドを動かしている間は、終わるまで待つ。
  - sshd: exec を受け付ける（前は断っていた）。標準入力は sshd の中にため、行が受け取った分だけ窓を足し直す。終わったら exit-status・EOF・CLOSE。exec はコンソールの写しを使わないので、対話のシェルと同時に使える。
  - sshd を同時に 3 接続まで（2026-11-16、ユーザーの依頼。ユーザーが ssh でログインしたままだと nos.py が断られていたため）: 接続ごとの状態を構造体 Conn（`conns[SSHD_CONNS]`）にまとめ、service_poll が C を切り替えて、今までの関数をそのまま使う。対話のシェルはコンソールの写しが 1 つなので 1 つまで（2 つ目は「console is in use」）。exec は同時に 1 つしか動かせないので、ほかの接続の exec が動いていれば、終わってから始める（pump_exec が monocs_rexec_start を試し続ける）。exec の標準入力の入れ物（1MB）は exec の間だけ確保。4 つ目の接続は断る。test.sh に、対話を開いたまま exec・対話の 2 つ目・exec 2 つ同時・4 つ目を足した。実機で、対話の ssh を開いたまま nos.py が動くことを確かめた。
  - sftp（2026-11-16。ユーザーと合意。`services/sshd/sftp.c`）: subsystem "sftp" の SFTP 版 3（draft-ietf-secsh-filexfer-02）。OPEN・CLOSE・READ（1 回 32KB まで）・WRITE・STAT・LSTAT・FSTAT・SETSTAT・FSETSTAT（日時は utime、大きさは今と同じか 0 だけ。開いている手は lseek で今の大きさを見る: Monocs の stat は閉じる前は書く前の大きさを返し、scp の書いた後の FSETSTAT が失敗した）・OPENDIR・READDIR（50 個ずつ）・REMOVE・MKDIR・RMDIR・REALPATH（文字の上で正規化）・RENAME（行き先があれば失敗）、拡張 posix-rename・statvfs・fsync。READLINK・SYMLINK は OP_UNSUPPORTED。POSIX の関数だけで書き、Linux でも同じものを試す。sshd は受けたデータを sftp.c にためて、答えの入れ物（256KB）に空きがある間だけ処理し、処理した分だけ窓を足し直す。閉じるときは exit-status 0（scp は、ないと失敗とみなす）。tools/ssh/test.sh に sftp（mkdir・put・rename・ls・get・df・rm）・scp（2.5MB の往復）・sshfs（cp・書く・名前の変更・mkdir・rm・rmdir）。実機（Pi 5 から Wi-Fi で）: sftp・scp の 1.5MB の往復が一致（約 1 秒）、sshfs でつないで書く・写す・名前の変更・df。入れるときは、まず別の名前・ポート（-DSSHD_PORT=2222 の compile_flags.txt を /work/sshd_test に置いて svc start）で確かめてから /services/sshd を置き換えた（sshd が起動しないと Wi-Fi から入れなくなるため）。
  - exec を 2 秒以上待たせているときは、相手の標準エラーに 1 回だけ知らせる（2026-11-16、ユーザーの依頼。ユーザーが ssh で top を動かしたままだと、nos.py が黙って止まって見えた）: コンソールでアプリが動いていて行が始まっていない（OS API の monocs_rexec_queued。rexec の QUEUED）なら「Monocs: waiting for 'top' on the console to finish (quit it to continue)...」（アプリの名前は monocs_sysstat）、ほかの接続の exec が動いているなら「Monocs: waiting for another ssh command to finish...」。SSH_MSG_CHANNEL_EXTENDED_DATA（標準エラー）。実機で、対話の ssh で top を動かしたまま nos.py cmd を出すと 2 秒で知らせが出て、top を q で終えるとすぐ動いた。
  - 止めてから動かす（2026-11-16、ユーザーの許可:「top を動かしっぱなしにしていたときのようなときは、強制終了させていい」）: ssh の環境変数 `MONOCS_STOP_APP=1`（`ssh -o SetEnv=MONOCS_STOP_APP=1`、`nos.py --stop-app`・`NOS_STOP_APP=1`）を受けた接続の exec は、コンソールのアプリに 1 秒待たされたら、OS API の monocs_console_interrupt（uart_raise_interrupt。Ctrl-\ と同じ）で止めて「Monocs: stopped 'top' on the console to run this command」と知らせてから動く。ふだん（付けないとき）は待つ。mi のようなエディタは保存していない編集が消えうるので、止める前にユーザーに聞く（Claude の決まり）。実機で、ssh で top を動かしたまま `nos.py --stop-app cmd` が 1 秒で動き、対話の画面はプロンプトに戻った。
  - nos.py: `--host アドレス` か環境変数 NOS_HOST で ssh を使う（cmd は exec、push はふつうの sftp（2026-11-16 の sftp の後、ユーザーの「一般的な sftp を使おう」から。`sftp -b -` で途中のフォルダを `-mkdir`、`名前.tmp` に put してから rename（posix-rename で置き換える）。前の `cat > X.tmp && mv` より速い: カーネルの送信 837 → 2,905KB/s）、pushdir は束を送って unpack.c、kernel・reboot は reboot の後 3 秒で ssh を切り、つながり直すまで待つ、log は dmesg）。鍵は `~/.ssh/id_ecdsa_monocs`（Pi 5 で作り、実機の `/.ssh/authorized_keys` に登録した。NOS_SSH_KEY で変える）。`--uart`・`--port` のときは線を使う。打ち込まないので check_idle はしない。
  - 実機（2026-11-16、Wi-Fi だけで）: 1 行の往復 約 0.3〜0.4 秒、`exit 3` が 3、1MB の送って戻す往復が一致（送る 約 700KB/s）、run・pushdir、SIGINT を無視して回るアプリが ssh を切ると止まり次のコマンドがすぐ動く、カーネルの入れ替え（送る 2 秒、再起動から ssh まで約 11 秒）、シリアルで行を打ちかけている間の exec で打ちかけの行が壊れない・cd と変数が残らない。
  - 確かめる: `sh tools/test/ssh.sh` に exec（出力と終了の値、3MB を標準入力から標準出力へ、途中で切った後、出力なし）を足した（Linux の代わり部品 tools/ssh/linux_main.c は /bin/sh -c で真似る）。直したこと: test.sh が 11-14 の sys/ioctl.h の置き換えから通らなくなっていた（linux_main.c を Linux のヘッダーで別にコンパイルする）、代わり部品のソケットが exec の子に受け継がれてポートが離れなかった（SOCK_CLOEXEC）。
  - 気をつけること: 起動しないカーネルを Wi-Fi で送ると、SD カードを PC で直すしかない（UART の線がないとき）。起動の表示は見えない。Wi-Fi の速さ（約 1MB/s）。家のルーターでは、つないで最初の通信が 1 秒遅れることがある。
- poweroff・halt（2026-11-16。ユーザーの「いつでも電源を切ってもいいの？」から）: シェルの組み込みのコマンド。vfs_exclusive の中で（書き出してから SD カードを独り占めしたまま。ほかのコアのサービスが後から書かないように）LED を消し、表示を送り切り（ssh の exec なら rexec_drain）、PM_RSTS にパーティション 63（0x555。ファームウェアへの「止まる」の印）を立ててウォッチドッグでリセットする。Linux の bcm2835_wdt.c の bcm2835_power_off と同じ（PM_RSTS の番地と値は資料になく、Linux による）。Raspberry Pi は電源そのものは切れず、ファームウェアが止まったまま待つ。もう一度動かすには電源を入れ直す。`nos.py poweroff`（Wi-Fi なら、その後 25 秒 ssh が答えないことを確かめる）。
  - 電源を切ってよいか: 書き込みは最初の書き込みから 1 秒・アプリの終わり・sync で書き出すので、何もしていない状態が数秒続いていればよい。確実なのは sync か poweroff。
- top（2026-11-16。ユーザーの「top みたいな CPU とかメモリの状況が見れるコマンドがほしい」から）:
  - `/bin/top.c`（curses。q で終わる。`-b`（端末でなければ自動）・`-n 回数`・`-d 秒`は Linux の top と同じ。最初の回は起動からの平均）。プロセスがないので、コアごとに「忙しさ・割り込みの数・何が動いているか（シェル・アプリの名前・アプリの仕事・pthread・サービス）」、メモリ（全体・空き・使用・ファイルのキャッシュ、カーネル用・アプリ用のヒープ）、サービスごとの %CPU・TIME+・POLL/s・WORK/s（仕事をしたと返した回数）と、サービスのコアが眠った回数を出す。
  - OS API の `monocs_sysstat`（`os/kernel/sysstat.c`、`os/api/include/monocs_sysstat.h`。mkimg.sh が /include に入れる）。数えるもの: コア 1〜3 の眠っていた時間（smp.c の仕事待ちの wfe。待っている途中の分も idle_since で足す）、コア 3 の wfe の時間と回数とサービスごとの poll の時間・回数・仕事をした回数（svc.c）、コアごとの割り込みの数（exception.c の irq_counts）、アプリの名前（app.c の app_ctx。argv[0] の最後の部分から .c を除く）、ヒープ（malloc.c の malloc_heap_info）。温度・クロック・throttled はメールボックス。
  - **top で見つけて直した不具合（前からのもの）**: サービスのコア（コア 3）が一度も眠っていなかった（いつも 99%、wfe が 1 秒に 36 万回すぐ戻る）。どのサービスも仕事をしたとは返していなかった。原因: Cortex-A53 では、アトミック操作（ldaxr・stlxr。ロックの取り外し、アトミックな足し算、失敗した CAS）の後の wfe がすぐ戻る（実機で確かめた。`tools/perf/wfe.c`: 10ms の間に wfe だけなら 4 回、mutex の取り外しの後なら 4 万回、失敗した CAS の後なら 17 万回。clrex を入れても変わらない。`sevl; wfe` で溜まったイベントを消すと 3 回。資料での裏付けは取っていない）。直し: 眠る前に `sevl; wfe; wfe`。ただし眠ると相手への返事が次の合図（コア 0 の 10ms ごとのタイマー）まで遅れ、ssh の 1 行の往復が 66 → 81ms になったので、コア 3 だけタイマーのイベントストリーム（CNTKCTL_EL1 の EVNTEN、EVNTI 13。実機で約 0.85ms ごとに wfe を起こす。割り込みではない）を入れた。結果（実機）: コア 3 99% → 0.4%（1 秒に約 1,200 回起きる）、温度 48 → 44℃、ssh の往復 70ms、1MB の ssh の送信・primes 44ms・primes_mc 495ms は前と同じ。
  - pthread の待ち（mutex・条件変数・join・once・rwlock・バリア）も直した（2026-11-16、ユーザーの依頼）: 起こす側は共通の通し番号（`os/libc/pthread.c` の wake_gen）を増やしてから sev（wake）、待つ側は条件を見る前に通し番号を読み（wake_mark）、合わなければ wait_change で眠る（`sevl; wfe` で溜まったイベントを消し、通し番号を `ldaxr` で見張って wfe。見張っている番地にほかのコアが書けば起きるので、取りこぼさない。Linux の __cmpwait と同じ考え）。時間切れのある待ち（timedlock・cond_timedwait）は、コア 1〜3 では待つ間だけタイマーのイベントストリームを入れて約 0.85ms ごとに起き、時間を見る（`os/drivers/timer.c` の cpu_event_stream。サービスのコアと共通。仕事の途中で止められたときに残さないよう、secondary_main が仕事を待つ前に切る。前は yield で回っていた: 89.8 → 0.1%、時間切れ 450ms ちょうど）。wfe で眠った時間も top の眠っていた時間に数える（timer.c の cpu_idle_account。前はスレッドの中の眠りを数えていなかったので、top の値は眠っていても 100% だった）。結果（実機、`tools/perf/pthwait.c`、どちらも眠った時間を数えるカーネルで）: 待っているスレッドのコアが mutex 93.6 → 1.3%、条件変数 92.9 → 1.8%、バリア 91.7 → 0.2%。条件変数のピンポン 1 万回 8 → 7ms。primes_pthread 495ms・primes_omp 382ms は前と同じ。コア 0 の join はループの中でアトミック操作をしないので、もとから眠れていた。
- 遅いアプリの起動・終わりを dmesg に（2026-11-16。ユーザーの「fd でファイルを開くのにたまにすごく時間がかかる」から）: slimcc_run_program（os/slimcc/run.c）が、読み込み（Makefile・コンパイル・キャッシュ・リンク）とアプリの終わりの vfs_sync がそれぞれ 1 秒以上かかったら `app PATH: slow start|end N s (compiled N, cached N; sd read N ms N KB, write N ms N KB; other N ms)` を klog に書く（other はロックの待ちなど）。あわせて、読み込みの間はコア 0 の割り込みのたびに止まっていた番地と x30 を記録し（os/kernel/exception.c の stall_sample_begin・end。4096 個まで）、遅かったときは多かった「関数<-呼び出し元」を 3 つ付ける（`; at 関数<-呼び出し元 N%`。/boot/kernel8.sym）。最初の記録（実機、fd の e で mi）: `slow start 13.801 s (compiled 0, cached 38; sd read 0 ms, write 0 ms; other 13801 ms)`。SD カードは動いていなかった。2 回目（3.2 秒）の記録で、コア 0 は 95% が smp_wait（キャッシュの読み込みを分担したコア 1・2 の終わりを待っていた）。そこで、記録の間はコア 0 の割り込みのたびに、仕事中のコア 1〜3 にもメールボックス 0 のビット 1（STALL_MBOX_BIT。ビット 0 は中断）で頼み、そのコアの番地も記録する（smp_handle_irq → stall_record）。行には「coreN 関数<-呼び出し元 N%」をコアごとに 2 つまで。原因はまだわかっていない。記録の基準は環境変数 MONOCS_SLOW_MS（ミリ秒、既定 1000）。調べている間、実機の /.profile に `export MONOCS_SLOW_MS=500` を置いた（2026-11-16。済んだら消す）。遅かった 2 回は、どちらも再起動の後に fd から mi を初めて開いたとき（私が先に make -C でキャッシュを作ってあった）。
    - 原因（2026-11-16、0.5 秒の記録で突き止めた）: コンパイルのキャッシュを使えるかの確かめ（os/slimcc/cache.c の dep_hash。ソース・ヘッダーの中身のハッシュを 1 回の読み込みの間覚える）が、覚えたものを 1 本のリストで頭から strcmp し、その間 1 つのスピンロック（atomic_flag の test_and_set を回し続ける）を持っていた。mi（38 ファイル、それぞれ百を超えるヘッダーと、なかったパス）を 3 コアで読むと、コア 1・2 がここで取り合い、ふだん 0.1 秒のものが 0.3 秒、20 回に 1 回は 3.4 秒（fd から開いたときは 13.8 秒も）になった（記録: core1・core2 とも dep_hash の中で 9 割）。直し: パスのハッシュで引く表（1024）、ロックは待つ間は読むだけ（test-and-test-and-set）。`make -C /apps/mi` の 40 回がすべて 0.09〜0.10 秒（前は 20 回で 0.10〜3.36 秒）。同じ atomic_flag の回し方は klog・wifi・rng・pthread・syscalls・socket・cc/tokenize.c にも残っている（どれも取り合いが少ないか短いので、そのまま）。いちばん疑わしいのは、カーネルを入れ替えた後の最初の 1 回（コンパイルのキャッシュはカーネルごと。mi は実機で 1.2 秒、2 回目から 0.43 秒）。あわせて、/bin/less は出力が画面でなければ中身をそのまま書いて終わる（Linux と同じ。前は見えない画面でキーを待ち、`less F > /dev/null` を ssh で流したら、コンソールのキーを吸い込んで実機が操作できなくなった）。
- シェルの `time`（2026-11-16。ユーザーの依頼）: bash と同じく、パイプラインの頭の予約語（parse_pipeline、N_TIME。`time ! cmd` も、`time -p` は POSIX の形）。終わったら標準エラーに bash の形（前に空の行、real・user・sys、ミリ秒まで）。real は経った時間、user はアプリが使えるコア（コア 0〜2。サービスのために取ってあるコア・起きていないコアは除く）の眠っていなかった時間の合計（timer_idle_us と smp_idle_ticks。シェル自身の分も入る）、sys はいつも 0。shell.sh の bash と比べる行に、終了の値・出力・`time -p` の形を足した。実機: `time make posix`（Lua の 33 ファイルを 3 コアで）real 1.49 秒・user 4.25 秒。
- Lua 5.4.9 をソースのまま動かした（2026-11-16。ユーザーと合意。CLAUDE.md の「次の候補」の「実際に使いたい Linux のプログラムを動かす」）: lua.org の lua-5.4.9.tar.gz（SHA-256 を確かめた）の src を手を加えずに実機の /apps/lua に置き、`lua` で動く（リポジトリには入れていない: third_party に足すときは相談）。直したもの:
  - Makefile の読み方（`os/slimcc/makefile.c`）: 既定のゴールからプログラムが見つからなければ `all` を試す（Lua の `default: $(PLAT)` → `guess` は、`uname` を動かして make を呼び直すので、何も見つからない）。`.a` の目標はプログラムに選ばず、プログラムの前提の `.a` はその `.a` の前提の `.o` に開く（`lua: lua.o liblua.a`）。試験 `tools/make/t2.mk`（Lua と同じ形）。
  - 足りなかった関数: localeconv・strcoll・strxfrm（picolibc にあり、表に足した）、tmpnam・tmpfile（`os/libc/posix.c`。`/tmp` に作る。tmpfile はアプリの終わりに消す）。picolibc の `L_tmpnam` が 8（P_tmpdir ""）で `/tmp/…` が入らないので、アプリに `__L_tmpnam=20`・`__P_tmpdir="/tmp"`（glibc と同じ）を渡す（run.c の app_defines）。
  - make の引数とコマンドの中の $(MAKE) の呼び直し（2026-11-16、ユーザーの「その仕組みを入れとこう」から）: makefile.c は規則のコマンドの行も覚え（Rule の recipe）、目標の前提からプログラムが見つからなければ、目標（なければ前提の目標を順に。follow_goal）のコマンドの中の make の呼び直し（`$(MAKE) 目標 変数=値`、`cd DIR && …`、`make -C DIR`。`$(MAKE)` は make、`$@` は目標、`` `uname` ``・`$(shell uname)` は Monocs）を読んで読み直す（read_level。4 段まで）。`変数=値` は GNU make と同じく Makefile の中の代入より強い（Var の cmdline）。目標を選んでいない（`make` だけ）ときだけ、見つからなければ all を試す。シェルの組み込みの `make [-C DIR] [目標] [変数=値]` は、引数をフォルダの `.monocs-make` に書いてコンパイルまで済ませる（失敗したら前の中身に戻す。`make clean`・`distclean` は消す）。make_read が `.monocs-make` を読むので、run DIR・./prog・PATH の名前だけのどれでも同じ選び方。`./prog` は、親のフォルダの Makefile がその名前のプログラムを作るなら、同じ名前の prog.c より先にそちら（前は ./lua で lua.c だけを動かしていた）。試験: tools/make/test.sh に make posix・linux（前提の目標のコマンド）・変数・ない目標・上のフォルダ（t2top.mk）。
  - 実行ファイル（2026-11-17。段階 5・6 も済み: 段階 5 は、実行ファイルの中のオブジェクトの読み解きを使えるコアで分担して（load_executable の exe_worker）、Lua の起動 0.22 → 0.13 秒（ソースからキャッシュで 0.15 秒）。段階 6 で run・`run -`・cache コマンド・コンパイルのキャッシュ（os/slimcc/cache.c、build/gen/compiler_id.h）・ソース（.c・フォルダ）を名前で動かす道・/autorun.c を消した。/autorun は実行ファイル（mkimg.sh の AUTORUN.c は /usr/src/autorun.c に入れて build_in_qemu.sh が作る）。svc は実行ファイルだけ。試験・nos.py（run は送って cc -o して動かす、pushdir の unpack も実行ファイル）・セルフホスト（`cc -o build-kernel build.c && ./build-kernel`。/src/build はフォルダなので build という名前は使えない）を書き換えた。ユーザーの方針: 「Linux と同じ方が多くの人に理解されやすい。そうでないと初めて使う人は毎回このシステムのことを学習しないとならない」。コンパイルのキャッシュと run も、すべて実行ファイルに移したら消す（段階: 1. 実行ファイルの形と make・cc -o・./prog（済み）、2. /lib のライブラリを取り込む（-l名前でソースごとコンパイルして入る。済み）、3. /bin のコマンドとサービスを実行ファイルに（イメージ作り・送り方も）、4. /apps の mi・fd・Lua、5. 速さを前後で比べる、6. キャッシュと run を消す）。「実行ファイルを持たない」の基本設計は、段階 6 で書き換える）:
    - 形（os/slimcc/run.c）: "MONOCSX\0"（2026-11-21 までは "UNUXEXE\0"）、u32 版 1、u32 オブジェクトの数、各 u64 大きさと slimcc_obj_save の中身。つなぐ前のコンパイルの結果（OS の関数は名前のまま）で、動かすたびに resolve でつなぐ。カーネルを入れ替えても作り直さずに動く（実機で、slimcc 版で作ったものが GCC 版でも動いた）。FAT に実行の許可の印がないので、先頭の印で見分ける（slimcc_is_executable）。
    - 作る: slimcc_make_executable（.c かフォルダ）・slimcc_make_executable_files（cc の複数の .c）。つなげるか（足りない関数がないか）を先に確かめてから書く（PROG.tmp に書いて置き換える）。
    - make（shell.c の make_build）: 実行ファイルが .c（Makefile のときはプログラムの .c 全部）より新しければ「'X' is up to date.」（時刻で。ヘッダーは見ない）。時計が合う前は（clock_known が偽）いつも作り直す: 起動から時刻合わせまで約 30〜40 秒かかり（sntp が先に聞く Pi 5 の USB のアドレスが今はない）、その間に書いたファイルは 2026-01-01 になる。作り方（目標・変数）が変わったら消して作り直す。表示は「cc -o NAME SRC」。Makefile もなく目標もない make は GNU と同じ誤り（前の版のフォルダをまとめてコンパイルする足しはやめた）。`make clean` は実行ファイルと .monocs-make を消す。
    - cc（cmd.c の cc_executable）: -c がなければ実行ファイル（既定 a.out）。-D・-U・-I・-std=・-l・-f は run と同じに、-O・-W・-g などは読み捨てる。-c のある今までの使い方（セルフホスト）は変わらない。
    - 動かす: ./prog・パスの名前（find_path_app）、PATH の名前（find_in。フォルダのアプリ・実行ファイル・NAME.c の順）。slimcc_load_program が実行ファイルなら読み込んでつなぐ（load_executable）。
    - 段階 3（済み、2026-11-17）: /bin のコマンドとサービスは実行ファイルだけ（Linux と同じ）。ソースは SD カードの /usr/src/monocs（bin・services と build.sh）。build.sh は tools/mkbuild.sh がリポジトリから作る Monocs のシェルのスクリプト（`sh /usr/src/monocs/build.sh [BINDIR] [SVCDIR]`。1 つのファイルは cc -o、フォルダは中の .c と compile_flags.txt）。イメージ（tools/mkimg.sh）は、ソースを入れてから QEMU で Monocs を起動して build.sh を流す（tools/build_in_qemu.sh。約 22 秒。記録は build/build_in_qemu.log）。実機は `NOS_HOST=… sh tools/update_bin.sh`（ソースを送り、実機で /bin.new・/services.new に作り、全部できたら sftp で /bin・/services と入れ替え、/etc/services.txt を置き換えて再起動）。/etc/services.txt は実行ファイルの名前（/services/httpd など）。sshd の起動 0.74 → 0.22 秒。
    - 段階 4（済み、2026-11-17。ユーザーと合意: 案 A）: 自分のアプリは、ソースを /usr/src/名前、実行ファイルを /apps/名前（PATH。Linux の /usr/local/bin と同じ役目）。実機の mi・fd・Lua を移した（/usr/src/mi・fd・lua、Lua は make posix）。`sh tools/apps/push.sh DIR [NAME]`（mi.sh も）は、ソースを /usr/src/名前 に送り、実機で `make 名前` して /apps/名前 に写す（Linux の make と make install と同じ形）。
    - あわせて: sntp はすべてのサーバーに同時に聞き、最初の答えで合わせる（前は 1 つずつ順に聞き、USB の Pi 5 の時間切れを待っていた）。Wi-Fi だけのとき、起動から時計が合うまで 30〜40 秒 → 17 秒以内。
  - コンパイルだけ（2026-11-17、ユーザーの「コンパイルしてキャッシュさせて実行はしない、があってもいい」から。ユーザーと合意して make にそろえた）: `make NAME` は、Makefile がないとき・Makefile にその目標がないとき、NAME.c をコンパイルしてキャッシュに入れる（GNU make の暗黙の規則。shell.c の make_implicit）。`./NAME` はキャッシュから動く（find_path_app の NAME.c）。Makefile のないフォルダで目標なしの `make` は、フォルダの .c をまとめて（make_folder。Monocs の足し）。案内は「run it with ./NAME」（ほかのフォルダならフォルダ付き）。
  - make posix で足りなかったもの: `_setjmp`・`_longjmp`（アプリに `-D_setjmp=setjmp`・`-D_longjmp=longjmp`。Monocs の setjmp はシグナルのマスクを保存しないので同じ）、flockfile・funlockfile・ftrylockfile・getc_unlocked・putc_unlocked・getchar_unlocked・putchar_unlocked・fseeko・ftello（picolibc にあり、表に足した）、fflush(NULL)（picolibc のこの設定では NULL を FILE として落ちる → アプリの fflush は app_fflush: NULL なら標準出力と標準エラー）、/tmp（起動のときに作る。mkstemp("/tmp/lua_XXXXXX")）。実機で `make posix` の後、io.popen・os.tmpname が動き、試したスクリプトの出力が Linux の Lua と同じ。
  - Monocs で `make` だけのときは、Lua は「汎用の C」の作り（`-DLUA_USE_LINUX` が付かない）になる: io.popen は Lua 自身が使わない（'popen' not supported。`make posix` なら使える）、C のモジュールの require（dlopen）はどちらでもない。Linux で同じ作り（gcc で src の .c、luac.c を除く）にした Lua と、試したスクリプトの出力が同じ。
  - 実機: 初めて 1.7 秒（34 ファイルのコンパイル）、2 回目から 0.49 秒（ssh の往復を含む）。fib(27) 0.47 秒、100 万回のループ 0.45 秒（Pi 5 の GCC -O2 は 0.02・0.01 秒）。
- アプリが落ちたときに関数名を出す（2026-11-16。ユーザーと合意）: 例外の入口（コア 0 のアプリの例外、アプリの実行中のコア 1〜3 の例外、Ctrl-C、スタックのあふれ）で、止まった番地・x30・フレームポインタをたどった戻り先を記録し（`os/kernel/fault.c`。最初に止まったところを残す）、アプリが止まって戻ってきたら run.c の print_fault が、アプリの記号表を捨てる前に関数名に直して出す（`at outer+0x3c` / `from main+0x6c`。アプリは slimcc_symbol_at、カーネルは /boot/kernel8.sym（`os/kernel/ksym.c`。perf.c から移した）で `[kernel]` を付ける）。app_call から下（シェルの側）は出さない。x30 が止まった関数の中を指すときは、前に呼んだ関数の戻り先が残っているだけなので出さない。dmesg の「stopped by exception」の行にも止まった関数を付ける。前からの番地とレジスタの表示もそのまま。
- dmesg を SD カードに残す（2026-11-16。ユーザーと合意。`os/kernel/klog_file.c`）: 起動のとき（klog_boot_done の後）/var/log/dmesg を /var/log/dmesg.1 に移して今の記録を書き、シェルがプロンプトで待つ間に増えた分を 5 秒に 1 回まで書き足す（klog_read_since。lineedit.c の read_byte_wait から）。アプリの終わり・reboot・poweroff の前（`reboot`・`poweroff` の行も記録する）にはすぐ書く。1MB を超えたら今の記録（64KB）で書き直す。nos.py kernel の試しの起動に失敗したときは、新しいカーネルの記録として /var/log/dmesg.1 を出す。
- 試しの起動（tryboot。2026-11-16。Wi-Fi だけでつないでいて、起動しないカーネルを送ると戻せないため。ユーザーと合意）:
  - ファームウェアの tryboot（次の 1 回だけ config.txt の代わりに tryboot.txt を読む）が Zero 2 W で効くことを、実機で確かめた（資料には対応する機種の一覧がない）。tryboot.txt に arm_freq=900 を足して `reboot tryboot` すると 900MHz で起動し、次のふつうの再起動で 1000MHz に戻る。印は Set reboot flags（0x00038064、ビット 0。mailbox_set_reboot_flags。Linux の reboot "0 tryboot" と同じ）。シェルの `reboot tryboot`。
  - `os/kernel/tryboot.c`: 起動の早いうち（timer_init の直後。kernel_main）にコマンドライン（Get command line、0x00050001。mailbox_get_cmdline）を読み、`monocs.tryboot` があれば試しの起動として、ウォッチドッグ（PM_WDOG 0xFFFFF ≒ 16 秒、PM_RSTC の FULL_RESET。Linux の bcm2835_wdt.c の start・stop）を入れる。タイマー割り込み（tryboot_tick）が餌をやるが、90 秒たっても `tryboot confirm` が来なければやめる → リセット → 前のカーネル。シェルの `tryboot [status|confirm]`。
  - `nos.py kernel`（Wi-Fi のとき）: /boot/kernel8.trial.img（と .sym）に置き、/tryboot.txt（実機の config.txt の kernel= と cmdline= を除き、`kernel=kernel8.trial.img`・`cmdline=tryboot-cmdline.txt` を足したもの）、tryboot-cmdline.txt（`monocs.tryboot`。os_prefix が付くかわからないので / と /boot/ の両方）を書いて `reboot tryboot`。ssh が通ったら `tryboot confirm`、成功したら kernel8.trial.img を kernel8.new に写して kernel8.img へ mv。確認できなければ（前のカーネルに戻った）dmesg を出して失敗にする。線でつないでいるとき（--uart など）は今までどおり直接書く。
  - 実機（2026-11-16）: 正しいカーネルは再起動から約 18 秒で確認され、kernel8.img になった（コマンドラインは `… vc_mem.mem_size=0x20000000  monocs.tryboot`）。わざと起動の途中で固まる（割り込みは動いたまま）カーネルを送ると、約 2 分で前のカーネルに戻り、nos.py が失敗を報告した（kernel8.img は変わらない）。
  - 戻せないもの: tryboot_init より前（起動から約 0.2 秒。boot.S、MMU、TLS、メモリの用意、UART、割り込みの用意）で止まるカーネル。ウォッチドッグはリセットのたびに止まるので、前のカーネルから入れておくことはできない。
- 何もしていないときの温度とクロック（2026-11-16。ユーザーの「使ってない時はもっと温度下げられないの？」から、ユーザーと合意して実験だけ）:
  - シェルの `clock` に電圧（GET_VOLTAGE、0x00030003、コア）の表示と `clock arm MHz`（SET_CLOCK_RATE、0x00038002、skip turbo 0。mailbox_set_clock_rate）を足した。force_turbo=1 のときは頼んでも変わらない（1000MHz のまま）。
  - force_turbo を外すと（実機、起動から 60 秒の後）、ARM 600MHz・VPU 250MHz・1.2500V になり、`clock arm 1000` で 1000MHz・1.2562V、`clock arm 600` で戻る（11-07 に効かなかったのは force_turbo か起動直後の 60 秒のためと思われる）。primes 44 → 56ms。
  - 何もしない状態の温度（8 分ずつ、1 分ごと）: 600MHz 40.2〜41.3℃、1000MHz 40.7〜41.8℃、force_turbo（VPU 400MHz も）41.3〜41.8℃。差は温度計の目盛り（0.5℃）ほど。コアは眠っていてクロックがほぼ止まり、ファームウェアが動かす電圧の幅も 1% ほどなので、効かない。前の 48℃ はサービスのコアが回っていたため（上の top の項）。
  - 決めたこと: force_turbo=1 のまま（速さを取る）。電圧をさらに下げる over_voltage のマイナスは、止まる危なさがあり（Wi-Fi だけでは戻せない）、効き目も小さい見込みなので試していない。下げるなら Wi-Fi の省電力（反応が遅くなる）か、ヒートシンク。
- 同じ文字列リテラルを 1 つにまとめる（2026-11-16。ユーザーの依頼。`cc/parse.c` の string_literal_var）: 同じ関数の中（FuncContext の strlits）と、関数の外どうし（file_strlits）で、要素の型（種類・大きさ・符号）と中身のバイト列が同じ文字列リテラルは同じ変数にする（GCC と同じく同じ番地。.rodata なので書き換えられない）。関数をまたいではまとめない（関数の中の文字列はその関数の静的な局所変数で、関数と一緒に出力され、使われない関数は出力しないため）。試験 `cc/test-a64/gccmatch/strlit.c`（GCC と比べる）。シェルの "HOME"・"HOME" + 4 は、まとめなくても動く形に直したまま。
- USB シリアルが黙る不具合（2026-11-14。hwcheck に互換の試験を入れたときに見つけて直した。前からのもの）:
    - さらに続き（2026-11-15）: `is_setjmp_call`（codegen.c）が名前に setjmp を含むかで見ていて、is_setjmp_call を呼ぶ ir_funcall・gen_funcall まで setjmp を呼ぶ関数とみなし、今までのコード生成にしていた（前の _ を除いて決まった名前と比べる、GCC と同じ）。ir_gen_fn は setjmp の部分と本体（ir_gen_fn_body）に分けた。内蔵アセンブラはよく出る命令（mov・movz・ロード・ストア・ldp・stp・b・bl）を最初に処理し（NEON の判定と長い連なりを通らない。どれも前のほうでは扱わない名前なので結果は同じ）、NEON の判定は名前が合ったときだけ最初のオペランドを読み解く。中間表現の確保の塊を関数をまたいで使い回し、切り出すたびに使う分だけ 0 にする（ir_pool。Monocs の中ではゾーンの free が何もしないので、関数ごとに新しい塊を calloc していた）。効果（実機）: slimcc 版 parse.c 726 → 679M、codegen.c 1122 → 1063M。GCC 版 450 → 420M、704 → 667M。
    - その次（2026-11-16）: 内蔵アセンブラの assemble_text は文の途中で特別な文字（改行・;・"・/）の手前までをまとめて写し、split_operands はオペランドを写さず元の文字列の中で区切る（呼び出し元はどれも写した文字列を渡す）。slimcc_vformat は書式のない部分と %s をまとめて memcpy（実機では差が見えない）。ir_optimize の 3 回の最適化は、1 回まわして中間表現の指紋（ir_fingerprint）が変わらなければ止める（出力は同じ。試したファイルの .s が 1 バイトも変わらない）。効果（実機）: slimcc 版 parse.c 679 → 655M、codegen.c 1063 → 1022M。GCC 版 420 → 411M、667 → 652M。時間の内訳（slimcc 版、perf record の関数を大まかに分けた）: 中間表現の最適化・割り当て・並べ替え 34%、構文解析・型など 20%、前処理・字句・表 14%、内蔵アセンブラ 14%、C ライブラリ・確保 12%、出力 7%。
    - 試して戻したもの（2026-11-16）: fnv_hash を 8 バイトずつ混ぜる形（コンパイラの名前はほとんど 8 バイトより短く、わずかに遅くなった）。
    - 試して戻したもの: 葉の関数では退避の要るレジスタを新しく使わない（fnv_hash の入口と出口の退避が減るが、コンパイルは速くならず、配列の和のループが 20.6 → 28.4ms に遅くなった）。
    - 試して見送ったもの: インライン展開の大きさ（IR_INLINE_SIZE 12）。カーネルを作るときだけ変えて（カーネルの中のコンパイラの仕事は同じ）比べると、8 は 1% 遅く、20・30・40 はどれも約 3% 速いだけ（カーネルは 20 で 4% 大きい）。既定を変えるとアプリ・自分のコンパイルの仕事も増え、全体では 9% 遅くなった（最初はこれを混ぜて測ってしまい、大きくすると遅くなると見誤りかけた）。カーネルのビルドだけ変えるにはセルフホストのビルドにも渡す仕組みが要るので、3% のために入れない。ループの展開を切ったカーネルも、速さは変わらない（723 対 728M）。
  - 起きていたこと: アプリが大量に出力している途中で Pi 5 が USB シリアルを読むのをやめる（nos.py が時間切れで終わるなど）と、その後 USB シリアルに何も出なくなった（シェル・UART・HTTP は生きている。`nos.py --uart reboot` で戻った）。
  - 原因: ホストが端末を閉じたとき（SET_CONTROL_LINE_STATE の DTR 0）、送信バッファの中身は捨てていたが、すでにコントローラに渡したバルク IN の送信（FIFO の中身）は残していた。開き直しても、その送信は 1 パケットも進まず（DIEPTSIZ のパケットの残り・DTXFSTS の空きが変わらない）、送信中の印（tx_busy）が外れないので、新しい出力を送らなかった。ホストがなぜ古い送信を受け取らないのかは確かめていない（usbmon は使っていない）。
  - 直し（`os/drivers/usb.c` の abort_tx）: DTR 0 のとき、Linux の dwc2 の dwc2_hsotg_ep_stop_xfr（IN）と dwc2_flush_tx_fifo と同じ手順で送信を取り消す（SNAK → INEPNAKEFF を待つ → EPDIS → EPDISBLD を待つ → TX FIFO の 1 番を flush）。`usb` コマンドに、シリアルの送信の様子（busy・stalled・queued、DIEPCTL・DIEPTSIZ・DTXFSTS）を足した。
  - 確かめる: hwcheck の「usb after stalled reader」（tools/test/device/flood.c を 2 秒だけ読んでやめ、出力が終わった後に USB シリアルが応答するか）。実機で 3 回続けて戻った。
- 実機を使っている最中に打ち込まない（2026-11-14〜15。ユーザーが実機で mi・less を開いていたとき、nos.py のコマンドの文字をその画面に打ち込んでしまった。ユーザーの許可で直した）:
  - httpd の `GET /.status`（`app 0|1`: コア 0 でアプリが動いているか、`remote 0|1`: telnet・ssh でつないでいるか）。OS API に `monocs_app_running`（app_is_running）・`monocs_console_remote`（contap_active。写しが開いている）。
  - nos.py の check_idle: 打ち込む前に /.status を見て、使われていれば 3 秒待ち直してから止まる（`--force` か NOS_FORCE=1 で続ける。/.status のない古いカーネル・HTTP が使えないときは調べない）。hwcheck の USB シリアルの確認（わざとアプリの実行中に UART から打つ）は --force。
  - `tools/apps/push.sh [-c] DIR [NAME]`: mi.sh を一般にしたもの。.c・.h（サブフォルダも）・Makefile・compile_flags.txt・.runignore を /apps/名前 に送る。名前は Monocs と同じ解釈器（tools/make/make_eval.c）で決めた Makefile のプログラムの名前（file-dir-manager → fd）、なければフォルダの名前。mi.sh は push.sh を呼ぶだけに。
  - 確認: アプリの実行中・telnet の接続中に nos.py cmd と push.sh が止まる、--force で進む、fd・mi を送れる。
- 全体の見直し（2026-11-14。ユーザーの依頼: 不具合を探す、重複・使われていないコードの整理、複雑な部分を簡単に、速さは守る）:
  - 調べ方: 厳しい警告（-Wshadow -Wunused-macros -Wmissing-prototypes -Wredundant-decls）、GCC の静的解析（-fanalyzer。OS・サービス・コマンド・ライブラリ。指摘なし）、リンカの --gc-sections --print-gc-sections で使われていない関数、長い関数の一覧、最近書いた部分（Wi-Fi・ネットワーク・SSH・TLS・展開・コマンド）を読む。古い部分（コンパイラ cc/、VFS、USB など）は機械的な検査だけで、1 行ずつは読み直していない。
  - 直した不具合:
    - `wifi_iovar_get`・`wifi_iovar_set` が、静的なバッファをバスのロックの外で組み立てていた（コア 3 の自動の接続とコア 0 のシェルが同時に呼ぶと壊れうる）。組み立てから答えを写すまでロックの中に。
    - atexit で登録した関数が呼ばれなかった（下の「アプリの終了処理」）。
    - curl: 展開に失敗したとき、エラーを 2 回表示していた。TLS のやりとりの時間切れの終了の値を 28 に（前は 35）。
  - 速くしたもの: TCP の API の受信（`os/net/tcp_api.c`）が、輪のバッファへ・からのコピーを 1 バイトずつ（そのたびに割り算）していたのを、memcpy 2 回に。hwcheck のアプリの TCP エコー 2MB が GCC 版 0.17 → 0.14 秒、slimcc 版 0.26 → 0.19 秒。Wi-Fi 経由の 3MB の取得 3.2 → 2.4〜2.6 秒。
  - 足したもの: 行頭の Ctrl-D で telnet・ssh の接続を閉じる（bash と同じ。シリアルでは何もしない）。
  - 消したもの: 調査用のコード（Wi-Fi の ARP・ICMP の記録 wtrace、`wifi pm`・`wifi clk`、読み出しの時間の計数）、使われていない関数（wifi_set_event_hook・wifi_down_reason・uart_is_raw・clock_tz・lock_pool_used・tls_offset・line_edit_width）とマクロ、重複した宣言。`wifi events`・`wifi irq`・`wifi iovar` は残した。
  - 分けたもの: シェルの wifi コマンド（1 つの関数に約 300 行）を `os/shell/wifi_cmd.c` に出して、サブコマンドごとの関数と表に。curl の request（145 行）を「つなぐ・送る・ヘッダーを読む・本文を読む」に、オプションの読み方を別の関数に。sshd のパケットの扱い（127 行）を段階ごとの関数に。
  - 速さ（前 → 後、実機、1000MHz）: primes 46ms・primes_mc 497ms・`ls /bin` 0.47 秒は同じ。カーネルのビルド 5.7〜5.9 → 5.6〜5.7 秒、C ライブラリ 33.9 → 36.0 秒（揺れの範囲。SD カードの書き込み）。
  - 確認: verify.sh quick（一致）、hwcheck（両方）、tools/ssh/test.sh、curl（Linux 版と実機）、atexit（QEMU）。
- フォルダの構成を Linux（FHS）に合わせた（2026-11-17。ユーザーと合意。下の 2026-11-13 の配置を置き換える）:
  - `ls /` は `boot config.txt etc root tmp usr var`（ファームウェアがルートから読む bootcode.bin・start.elf・fixup.dat・tryboot.txt・tryboot-cmdline.txt は隠し属性。nos.py kernel も tryboot の 2 つに +h を付ける）。
  - /usr/bin（付いてくるコマンド）、/usr/sbin（サービス。/etc/services.txt は /usr/sbin/httpd など）、/usr/local/bin（自分のプログラム。mi・fd・lua）。PATH は `/usr/local/bin:/usr/bin`。/usr/include（picolibc と monocs.h など）、/usr/lib/名前（共有のライブラリ。-l名前）、/usr/lib/slimcc/include（cc/platform/monocs.c）、/usr/lib/firmware/brcm（Wi-Fi。なければ前の /lib/firmware/brcm も探す）、/usr/share/monocs/samples、/usr/src/monocs（bin・services と build.sh、kernel = セルフホストの材料。前の /src）、/usr/src/名前（自分のアプリのソース）。
  - HOME は /root（.profile・.shell_history・.ssh/authorized_keys・.local）。作業用は /tmp（nos.py run・pushdir の unpack・試験も。シェルのパイプなどの一時ファイルも /tmp/.pipeN など）。TLS の認証局の覚えは /var/cache/tls。
  - 実機の移し方は tools/migrate_layout.sh（今のカーネルで新しい場所のコマンド・サービスを作り、sftp で移す。古いカーネルに戻っても入れるよう Wi-Fi のファームウェアと ssh の鍵は古い場所にも残し、新しいカーネルが動いたら `migrate_layout.sh cleanup`）。済み（2026-11-17）。
- Ctrl-S・Ctrl-Q で出力を止める・再開する（2026-11-19、ユーザーの依頼。Linux の IXON と同じ）: os/drivers/uart.c の flow_key（UART・USB・telnet・ssh のどこから打っても。raw のとき・アプリが IXON を外したときはふつうのキー。termios.c の term_ixon）。止めるのは syscalls.c の screen_putc（アプリとシェルの画面への出力）だけで、割り込みを止めている所とサービスでは待たない。中断のキー（Ctrl-C・Ctrl-\\）でも再開する。実機（対話の ssh）で、11 秒のうち 5 秒止めると届く行が 122 万 → 68 万。
- ssh の画面が止まったり動いたりする（2026-11-20。ユーザーの malloc を繰り返して出力し続けるプログラムを、Pi 5 の ssh で数時間見ていて）: Monocs が送る TCP の切れ端が小さく（平均 300 バイト。TCP は Nagle を切っていて、sshd はコンソールから読むたびに 1 パケットで送っていた）、Wi-Fi の送信の待ち行列（32 枠）があふれて黙って捨て、再送の時間切れ（1〜1.5 秒）まで止まっていた（Pi 5 で受けたパケットを見てわかった。数時間たってから起きる理由は確かめていない）。直し: sshd の pump_console は、出力が流れ続けているとき（前に送ってから 20ms の内）は 5ms までためて最大 8KB で送る（ぽつぽつ出るものは待たない。ためている間は写しを読みに行かない）、wifi.c の TXQ_LEN を 128 に。実機で同じプログラムの 20 秒で再送 0（前は 15 秒で 106 回）、切れ端は約 7 分の 1、中身の速さ（約 5,500 行/s。malloc・free で手いっぱい）は同じ。exec の出力（pump_exec）はためていない。
  - 続き（2026-11-20、ユーザーの「まだ何時間かすると 1 分に 1 回ぐらい止まる」から）: 起動から 6 時間 46 分の実機で、Wi-Fi の待ち行列で捨てた数は 0（送った 862 万）。それでも Pi 5 で受けたパケットを見ると 0.65% が落ちていた（起動の直後の 20 秒は 0）。落ちる場所は Monocs の外（チップ・電波・ルーター）で、数時間たってから落ち始める理由はわかっていない。ほとんどは TCP の速い再送で数 ms で戻るが、戻せないときに再送の時間切れまで 1〜1.5 秒止まっていた。lwIP は時間切れを TCP_SLOW_INTERVAL（2 × TCP_TMR_INTERVAL、既定 500ms）の刻みで数え、往復が短くても最小で約 3 刻みになるため。TCP_TMR_INTERVAL を 50 に（lwipopts.h。最小 約 0.3 秒）。確かめ: Pi 5 の nft でわざと 1% 捨てて、毎秒 約 300KB のまま止まらない（いちばん長い途切れ 0.27 秒）。verify.sh quick（一致）。
  - その後（2026-11-20）: それでも 10〜20 分に 1 回、1〜3 秒（一度は 11 秒）止まる。止まっている間、Monocs は送り直し続け Pi 5 に届いているが、Pi 5 の返事が Monocs に届かない。Wi-Fi のドライバに調べる記録を足した（wifi.c の diag_*: 送っているのにデータを 1 秒以上受け取らなかったとき・送ってよい番号を 0.2 秒以上待ったときを dmesg に。wifid から wifi_diag_report。`wifi events` に回数とイベントの種類ごとの数）。記録: 11:05:27 の 1.2 秒の止まりと同じ時刻に「no data received for 1226 ms while sending 10 frames (events: none)」、送ってよい番号の待ちは 0。チップから何も上がってこない（ルーター → Monocs の 2.4GHz が途切れている）。負荷なし・負荷ありの 20ms おきの ping（5 分・3 分）では続けて落ちる区間はなく、まれにしか起きない。malloc・free は関係ない（exec で asa と malloc なしのプログラムを交互に比べて同じ）。「イベント」の数（1 分に約 1.5 万）はイベントの形でない枠で、種類ごとの数には入らない（スキャンではない）。対話の ssh で 1 時間ずつ比べた（2026-11-20 13:40〜15:42。読みに行った回数などを記録に足した後）: asa 5 回・malloc なしの num 5 回（0.6 秒以上の止まり。1〜2.3 秒）で同じ。止まりはすべて dmesg の記録と時刻が合い、どれも polls・timer が 1 秒に約 100 回（10ms ごとのタイマーがチップを読み続けている）、read errors 0、bus busy 0、チップが知らせた枠はデータ以外の数個だけ。Monocs はチップを読み続けていて、チップに届いていない（malloc・コア 0 の止まりではない）。ユーザーと合意して、ここで終える（2026-11-20。Monocs の外の問題なので）。もう一度調べるなら、別のアクセスポイント（テザリングなど）・ルーターの 2.4GHz のチャンネルを変えて同じように長く流して比べる（記録はそのまま残してある）。
- 決まった大きさの上限を見直した（2026-11-20。ユーザーと合意: 上限の一覧を作り、黙って捨てるものと同時に開けるファイルから）:
  - fd: ファイルとソケットで番号を分け合い、空いているいちばん小さい番号から（POSIX と同じ）。VFS_MAX_FILES 16 → 64（fd 3〜63。64 は picolibc の FD_SETSIZE。それより大きい番号は select で扱えない）。ソケットは vfs_fd_take_socket・vfs_fd_put_socket で番号を取り（files[] の used は偽のまま、fd_sock で印）、socket.c の sock_at が fd → socks の番号。SOCKET_MAX 24 → 32。前はファイル 3〜15（13 個）、ソケット 16〜39。シェルのリダイレクトは向け先のファイルに fd を 1 つ使う（dup2 がないため。前から）。
  - 黙って捨てていたもの: `cc`（-c なし）のファイル・オプション 64 個（引数の数だけ取る形に）、`run` の -D・-U・-I 64 個（256 に。超えたら「too many -D options」でコンパイルしない）・-l 16 個（超えたら誤り）、シェルの `run` のオプション 31 個（それより後をファイル名と取り違えた。引数の数だけに）、`run` のキャッシュのキー 1024 文字（黙って切れ、オプションの違うコンパイルが同じキーになりえた。入りきらなければ終わりを全部の FNV-1a に）、Tab の補完の名前 128 個（512 に）。
  - 試験: tools/test/fds/fdtest.c（Linux の ulimit -n 64 と同じ出力。compat_dev.sh の 7）。compat_dev.sh・netapi.sh・sockets/run.py は、USB（192.168.7.2）の HTTP で /work の結果を読んでいた（今はプログラムが /tmp に書くので、/work に残った古い結果と比べうる）→ nos.py cmd で /tmp から読む、Wi-Fi（NOS_HOST）なら Pi 5 のアドレスもそこへ届くものに、SIGWINCH は USB の telnet が要るので Wi-Fi のときは飛ばす。
  - シェルの語の数（2026-11-20、続けて）: 展開した語・引数の表（expand_tokens の出力、Stage の argv・assign、for の語）を要るだけ広げる（上限 MAX_WORDS 65,536。前は 256 で「too many words」）。実機で /usr/src/kernel/build の `ls */*/*.o …`（1,088 個）。shell.sh に 1,200 個のファイルの `*`・for・ls・rm を足した（142 行が bash と同じ）。{a,b} は 1 つの語から 1,024 個まで（BRACE_MAX）のまま。
  - 残り（一覧で挙げて、まだのもの）: TCP の接続 16 個（サービスの分も含めて）・backlog 4、UDP のソケット 8 個、pthread のキー 64 個。
- sshfs の上の find（bfs）が一部のフォルダで EPERM（2026-11-20。ユーザーの依頼で調べた）: Monocs の sftp は手（ハンドル）が 16 個の決まった表で、足りないときと opendir の失敗を「理由のない失敗」（SSH_FX_FAILURE）で返し、sshfs はそれを EPERM にする。bfs は読んでいる途中のフォルダを開いたまま下へ進み、sshfs は bfs が閉じるまで手を返さないので、キャッシュの冷えた遅いときほど多く重なった（GNU find・Python の os.walk は 1 つずつで失敗しない。bfs も strace で遅くすると失敗しない）。カーネルの同時に開けるフォルダの数（VFS_MAX_DIRS）も 4 だった。直し: sftp の手の表を要るだけ広げる（上限 1024。OpenSSH の sftp-server と同じく決まった数にしない）、フォルダは開いたときに名前を全部読んで閉じる（カーネルのフォルダを開いたままにしない）、失敗の返事に理由の文（Too many open handles、strerror）、VFS_MAX_DIRS を 32 に。確かめ: 再起動の直後（冷えた状態）の bfs で 3,329 ファイルを 3 回とも失敗 0、tools/ssh/test.sh に sshfs でフォルダを 200 個開いたままにする試験、verify.sh quick（一致）。
- SQLite 3.54.1 をソースのまま動かした（2026-11-20。ユーザーの「次は SQLite を動かしてみよう」から。sqlite.org の sqlite-amalgamation-3540100.zip（SHA3-256 を確かめた）の shell.c・sqlite3.c・sqlite3.h・sqlite3ext.h を手を加えずに実機の /root/apps/sqlite3 に置き、`sqlite3` で動く。リポジトリには入れていない: third_party に足すときは相談）:
  - 初めの 1 回（27 万行の sqlite3.c のコンパイル）は実機で 12.4 秒、2 回目からキャッシュ。Pi 5 の slimcc で約 1 秒、使うメモリは約 300MB（実機のヒープは約 400MB）。
  - 先に Pi 5 で、実機と同じヘッダー（tools/mkimg.sh の /usr/include と同じものを作業用のフォルダに集める）を `cc/slimcc-jit-monocs --sysroot=DIR -std=gnu23 -c` に渡してコンパイルし、`nm -u` と api.def を比べて足りないものを出した（実機で 1 つずつ試すより速い）。
  - 足したもの（コンパイラ。cc/platform/monocs.c の gcc_builtin_macros）: GCC の `__atomic_*`・`__sync_*`（SQLite は `__has_extension(c_atomic)` が真なら `__atomic_load_n` などを使う。slimcc は C11 のアトミックだけで、これらがなかった。中身は stdatomic.h と同じ組み込み、順序の引数は見ずに seq_cst。`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_*` は定義しない: picolibc の stdio がそれで作り方を変える）、`__builtin_ceil`・`floor`・`trunc`・`round`・`fabs`・`sqrt`（と f。shell.c は __GNUC__ なら `__builtin_ceil`）。`__builtin_bswap*` は GCC_VERSION（4.2.1 を名乗る）で使われない。
  - 足したもの（C ライブラリ）: `<dlfcn.h>`（dlopen はいつも NULL と理由。os/api/include/dlfcn.h）、`<sys/resource.h>`（Linux と同じ形に置き換えた。picolibc は struct rusage が 2 つだけで getrusage を _XOPEN_SOURCE のときだけ宣言する）と getrusage（アプリを始めてからのアプリのコアの CPU 時間。シェルの time と同じ数え方）・getrlimit・setrlimit、ftruncate・truncate・fsync・fdatasync（f_sync と diskio の書き出し）、fcntl の F_SETLK・F_SETLKW・F_GETLK（いつも取れる。プロセスが 1 つなので）、chmod・fchmod・chown・fchown（何もしない）、readlink（EINVAL）・symlink・link（EPERM）・utimes、getuid などの 0・getpwuid・getpwnam（root）、mmap・munmap・msync・mprotect・madvise（os/libc/mman.c。確保したメモリにファイルを読み、MAP_SHARED の書き込みは msync・munmap で書き戻す。持ち主の終わりに mman_close_owner）、sysconf(_SC_OPEN_MAX) を 64 に（40 のままだった）。カーネルの posix.c は置き換えたヘッダーをパスで読む（セルフホストのビルドは実機の /usr/include を読むので、どちらでも同じものになるように）。
  - **直した VFS の不具合（前からのもの）**: ファイルの終わりより先への lseek で、FatFs の f_lseek が書き込みで開いたファイルをその位置まで広げていた（中身は SD カードに残っていたもの）。SQLite は空のデータベースの 24 バイト目を lseek して読むので、ファイルが 24 バイトのごみになり「file is not a database」になった。位置だけを覚え（vfs_file の past・past_pos）、次の write で間を 0 で埋める（POSIX と同じ）。あわせて stat の st_ino を絶対パスのハッシュに（前はすべて 0 で、SQLite が別々のファイルを同じファイルとみなしうる）。
  - 確かめ: Pi 5 の GCC で作った同じ SQLite と、2 つの SQL（メモリの中: 2 万行・索引・集計・窓関数・再帰の CTE・JSON・日付・printf の浮動小数点・UTF-8・UPSERT・トリガー・generate_series・EXPLAIN、88 行。ファイル: 削除と WAL のジャーナル・ATTACH・VACUUM・integrity_check）の出力が同じ。1 回ずつのコミット 200 回は 9.9 秒（WAL 6.6 秒。コミットごとに SD カードに書き出す）。tools/test/linuxapi.c に、終わりより先の lseek・ftruncate・fsync・ロック・mmap・st_ino・getrusage・dlopen・getpwuid・__atomic・__sync・__builtin_ceil などを足した（QEMU と実機で Linux と同じ 82 行）。make cc-test、verify.sh quick（一致）、shell.sh。
- Lua 5.4.9 の公式テストを動かした（2026-11-20。ユーザーの「次は Lua の公式テストを動かしてみよう」から。lua.org の lua-5.4.9-tests.tar.gz（SHA-256 を確かめた）を手を加えずに実機の /root/luatests に置いた。リポジトリには入れていない）:
  - 結果: `lua -e"_U=true" all.lua`（ふつうの形）と `lua -e"_port=true" all.lua`（重いテストも）が「final OK !!!」（実機 8 秒・62 秒。Pi 5 の GCC の make posix は 0.7 秒・7.3 秒）。比べた Pi 5 の Lua は、同じ make posix（Monocs の /root/apps/lua の .monocs-make と同じ）。
  - 残り（どちらも Monocs の設計で動かないもの）: `_port` なしでは、main.lua が libs/*.so の C のモジュール（dlopen。共有ライブラリはない）と、Ctrl-C のテストで `&`（裏で動かす。ユーザーと合意して入れていない）を使う。その 2 か所を除いた写し（実機の /tmp で、ファイルは残していない）では、main.lua の残りの出力が Linux と同じ（readline の入力の折り返しを見る 1 か所は、make posix の Linux でも通らない）。
  - 直したもの（どれも前からの不具合。Linux と違っていた）:
    - setlocale がどんな名前も受け付けた（picolibc）。literals.lua は pt_BR を受け付けたら「3,4」を数として読めると見て止まった。C・POSIX と文字の種類が UTF-8 のものだけ受け付ける（os/libc/posix.c の locale_name_ok）。LC_CTYPE 以外の分類を聞くと全体の名前を返していたので、分類ごとに名前を覚える（POSIX は C と同じ名前。glibc と同じ）。
    - ほかの fd で書いたものが、読み出し専用で先に開いた fd から見えなかった（キャッシュの fd は開いたときの中身、FatFs の fd は開いたときの大きさを持つ）。書いた fd の write（読む fd があるときだけ f_sync）・close・fsync・ftruncate で、同じファイルの読み出し専用の fd に stale を立て、その fd の次の read・lseek・fstat で開き直す（os/fs/vfs.c の mark_others_stale・refresh。fd に鍵（パス）を持たせた。キャッシュから開く速い道 open_cached_locked でも持たせる。最初は忘れて linuxapi で見つけた）。書いている間にほかが入れたキャッシュも、閉じるときに捨てる。O_APPEND の write はいつも終わりへ。
    - リダイレクトした stdin の終わりが「読み出しの誤り」になっていた: picolibc の FILE の get は終わりに _FDEV_EOF（-2）を返す決まりで、EOF（-1）は _FDEV_ERR と同じ値（os/libc/syscalls.c の stdin_getc）。feof が偽・ferror が真で、`lua - < FILE` が「cannot read stdin」で止まった。端末の Ctrl-D も同じだった。
    - `env` がシェルの組み込みで一覧を出すだけだった。GNU の env と同じく `env [-i] [-u NAME] [NAME=VALUE]... [COMMAND]` で、変えた環境で動かして戻す（os/shell/shell.c の cmd_env）。
    - 外側のリダイレクト（system() を呼んだアプリの `2>&1`）が、子の `2> FILE` より勝っていた（stderr_putc は err_to_out を先に見る）。内側のリダイレクトが勝つように、向け先を決めるときに前のもの（err_to_out・err_null・out_null・in_null）を外す。
  - 確かめ: tools/test/linuxapi.c にロケール・ほかの fd で書いたものが見える・O_APPEND を足した（QEMU と実機で Linux と同じ 85 行）、shell.sh に env と入れ子のリダイレクト（151 行が bash と同じ）、verify.sh quick（一致）、SQLite の 2 つの SQL（Linux と同じ）。
- vim 9.2 をソースのまま動かした（2026-11-20。ユーザーの「vim を試して」から。github の vim/vim の v9.2.1373（d399646c）を手を加えずに。実機の /root/apps/vim にビルドに使う src のファイル（369 個）、/root/.local/share/vim/vim92 に runtime（テストとアイコン・翻訳を除く 2,459 個）。リポジトリには入れていない）:
  - configure は Pi 5 で、代わりのコンパイラ（Monocs のヘッダーで slimcc-monocs を動かし、つなぐときは足りない関数が api.def にあるかだけを見る python のスクリプト）で、cross（--host=aarch64-none-elf）として流した。値: `CFLAGS="-O2 -DUSE_SYSTEM -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -include sys/wait.h"`（USE_SYSTEM は :! を system() で。_XOPEN_SOURCE・_DEFAULT_SOURCE は vim.h が __linux__ のときにすることと同じ。-include sys/wait.h は USE_SYSTEM のときに vim が sys/wait.h を読まずに waitpid を使う vim の不具合の回避）、`vim_cv_terminfo=yes vim_cv_tgetent=zero` など、`--prefix=/root/.local --with-features=huge --with-tlib=termcap --disable-gui --without-x --disable-channel --disable-terminal --disable-netbeans --disable-cscope --disable-nls` など。生成した auto/config.h・config.mk・osdef.h（osdef.sh）・pathdef.c（CC=cc で）を一緒に送る。`vim -`（標準入力から読む）は dup がないので使えない。
  - 実機: make（134 ファイル）36.6 秒、起動 約 3.7 秒（キャッシュから 8MB の部品を読んでつなぐ）。vim script（計算・リスト・浮動小数点・正規表現・substitute・strftime）が動く。
  - 足したもの: termcap（os/libc/termcap.c。tgetent はどの名前でも xterm-256color。表 termcap_table.h は tools/gen_termcap.c が Pi 5 の ncurses から作る。tgoto・tparm は terminfo の引数つきの文字列を解く。sh tools/libc/test_termcap.sh で ncurses と 3,425 項目が同じ）、既定の環境変数に TERM=xterm-256color、プロセスの関数（fork・vfork・exec*・pipe・dup・dup2 は ENOSYS、wait・waitpid は ECHILD）、umask（覚えるだけ）、getgrgid・getgrnam（root）、setitimer・getitimer（ITIMER_REAL。alarm と同じく待つところで届く）、environ・_exit を表に、utime.h（utime の宣言）、Linux と同じ形の struct winsize（ws_xpixel・ws_ypixel。os/api/include/sys/termios.h は picolibc の写しで、カーネルの termios.c はパスで読む）、slimcc の limits.h の SSIZE_MAX を数に（picolibc はキャストで #if に書けない）。
  - コンパイラ: `-include FILE`（SlimccOptions の forced_includes。run・Makefile の CFLAGS・compile_flags.txt）。
  - Makefile の読み方（os/slimcc/makefile.c）: include を読む（ないものは飛ばす）。.o の .c はその .o の規則の前提（objects/xdiffi.o: xdiff/xdiffi.c）、なければパターンの規則・サフィックスの規則。オプションは .o を作るコマンドの行から（$@ $< $^ $* を入れて展開。vim は $(CCC) $(ALL_CFLAGS)）、コマンドの行のない .o があるときだけ $(CPPFLAGS) $(CFLAGS)。
  - **SD カードの FAT を広げた**（2026-11-20、ユーザーの選択。63MB で空きが 9MB しかなかった）: カードを Pi 5 のカードリーダーにつなぎ、先頭 64MiB を build/sd-backup/sd-20261120-head64M.img に控え（ハッシュ一致）、ファイルを mtools で取り出し（build/sd-backup/sd-20261120-files、属性は sd-20261120-attrib.txt）、区画 1 をカードの終わりまで（28.9GB）に広げて mkfs.vfat -F 32 -s 8（クラスタ 4KB）、ファイルを戻して隠し・システムの属性を付け直し、全ファイルの中身・属性が元と同じことを確かめた。実機は起動し、df は 29G。
  - **直した不具合**:
    - シェルが 1 つの文（sh のスクリプト全体）を字句 512 個・節 512 個までしか読めず、137 行のスクリプトが「too many words」で動かなかった。字句は文の長さ、節は字句の数の 4 倍まで、要る分を取る（os/shell/shell.c の parse_text）。
    - vim の make（134 ファイルを 3 コアで）で実機が止まった（ping も返らない。2 回。電源の入れ直しで戻した）。QEMU で再現して、3 つのコアとも確保が NULL を返したまま書き込んでいた（コンパイラの new_pool・ir_insn。コア 0 はカーネルのコードの領域に書いて system halted）。原因は 2 つ: (1) コンパイラの中間表現の生存の解析（ir_liveness）が、ブロックごとに 4 つの集合を値の数の幅で持ち、vim の nfa_emit_equi_class（10,540 ブロック、47,278 個の値）だけで 240MB を使った → ブロックをまたいで生きうる値（どこかの use に入るもの）だけに値の番号の順に詰めた番号を付け、その幅で持つ（出力は変わらない: tools/slimcc/cmp_s.sh と vim のファイルの .o で確かめた。regexp.c 329 → 98MB、速さも 0.77 → 0.61 秒）。(2) まとめてコンパイルするときのファイルの覚え（cc/tokenize.c）に、1 回しか読まない本文の .c の中身と字句の記録まで入れていた → 本文は入れない（tokenize_main_file）。ほかに、ゾーンの確保が失敗したら「out of memory」の誤りにしてそのコンパイルだけを止める（cc/zone.c。前は NULL を返し、呼び出し元が確かめずに書いた）。QEMU の make のヒープの広がり 461 → 227MB、実機 224MB（ヒープ 391MB）。
    - 調べ方: コンパイラのゾーンに呼び出し元ごとの確保の量を数える仕掛けを足した写し（作業用。-O1 -g -no-pie）を Linux で動かし、addr2line で関数名に（ユーザーの「メモリを調べるツールも作るべきかも」。まだリポジトリには入れていない）。
- QuickJS をソースのまま動かした（2026-11-21。ユーザーの「次は QuickJS を動かしてみよう」から。github の bellard/quickjs の e585640f（VERSION 2026-06-04）を手を加えずに実機の /root/apps/qjs に（ビルドに使う .c・.h・Makefile・VERSION・tests と、Linux で作った repl.c。リポジトリには入れていない）。`make` で qjs（33 ファイル、実機 8〜10 秒。quickjs.c は 73MB）、2 回目から起動 0.02 秒）:
  - 公式テスト（Makefile の test）: closure・language・builtin・loop・bigint・cyclic_import・worker・rw_handler が通る。test_intl は `env -u TZ` で通る（Monocs の TZ は POSIX の形の JST-9 で、QuickJS の Intl は IANA の名前だけを読む。Linux でも TZ=JST-9 なら同じく失敗する）。test_std は os.exec（fork）・symlink・1970 年の日時（FAT は 1980〜2107 年）の部分を除いた写しで通る。bjson・point は .so（dlopen）なので試していない。
  - 速さ: fib(27) 1028ms（Pi 5 の GCC 版 33ms）。インタプリタの本体 JS_CallInternal が、スタックで渡す引数（10 語）・alloca・計算型の goto のため中間表現を通らなかった。
  - その続き（2026-11-21。ユーザーの「次はそれをお願いします」から）: slimcc の中間表現で、スタックで渡す引数（呼ぶ側・呼ばれる側）、`__builtin_alloca`、計算型の goto（&&label・goto *p）を扱うようにした（docs/slimcc-codegen.md の 5.1）。QuickJS（Pi 5、-g なし）fib(27) 173 → 143ms、ループ 302 → 236ms。実機 fib 1028 → 1001ms、ループ 1753 → 1517ms（大きな関数で逃がした値の読み書きが 2 命令になるのが残る。次の候補）。カーネルの中の今までのコード生成の関数 76 → 55 個。primes 44ms・primes_mc 495ms は同じ。さらに（2026-11-21。ユーザーの「この 3 つ全部お願いします」から）: フレームの読み書きを 1 命令に（sp か x28 を土台に）、定数は逃がさず作り直す、逃がす値を重み ÷ 生きる長さで選ぶ、構造体を受け渡しする小さな関数のインライン展開（docs/slimcc-codegen.md の 5.2）。実機の QuickJS fib(27) 1000 → 590ms、ループ 1513 → 722ms、カーネルの中のコンパイラ parse.c 575 → 566M サイクル。本当に生存区間を分けるのはまだ（候補に残した）。続けて SROA（構造体の受け渡しを 8 バイトずつの読み書きにし、アドレスの漏れない局所変数の場所を SSA の値にする mem2reg。docs/slimcc-codegen.md の 5.3）: 実機の QuickJS fib(27) 590 → 527ms、ループ 722 → 510ms。カーネルの中のコンパイラは parse.c +2%、codegen.c +6.4% 遅くなった（中間表現の命令が増えたため）。確かめ: make cc-test（gccmatch に stack_args・alloca・computed_goto・inline_struct）、verify.sh（全部。セルフホストの一致、gen2、libctest）、shell・pthread・openmp・linuxapi、実機の compat_dev.sh と QuickJS の公式テスト。slimcc は -g のとき中間表現を使わないので、Linux で QuickJS の Makefile のまま作ると最適化なしの版を測ることになる（一度これで見誤った）。
  - **直した slimcc の不具合（前からのもの）**: 即値の add・sub・cmp の入力に、定数 0 のレジスタ（xzr）を使っていた（`add w10, wzr, #1`）。即値の形ではレジスタ番号 31 は sp なので、sp + 1 になっていた。`s->n = 0; s->n++;` の形（QuickJS の文字列の rope の反復子）で起き、文字列の比べが assert で止まった。中間表現の出力は ir_base_srcA（0 は mov で x16 に）に、内蔵アセンブラは GNU as と同じく即値の add・sub・cmp・cmn の xzr・wzr を誤りにした（check_imm_zr）。試験 `cc/test-a64/gccmatch/zr_imm.c`（直す前の slimcc は落ちる）。
  - ほかのコンパイラの直し: 13,709 文字の文字列リテラル（intl/locale-data.c）で internal error（領域の塊 8KB より大きい確保。cc/alloc.c の big_pool で専用の塊に）、`_BitInt(128)`（__int128）と double・float の変換（bitint_builtins の __slimcc_bitint_from_f64・to_f64・to_f32。試験 gccmatch/int128_float.c）。
  - Makefile の読み方: レシピのない all・.PHONY の目標はプログラムに選ばず前提をたどる、`$(shell cat FILE)` はファイルを読む（tools/make/t3.mk）。
  - 足したもの: pipe・pipe2（`os/libc/pipe.c`。同じプログラムの中の pipe。QuickJS は poll で待つ間に自分を起こすのに使い、なければ起動で止まった。fd の read・write・close・fstat・fcntl・ioctl・select・poll、アプリ・サービスの終わりに閉じる）、fd の数 64 → 256（VFS_MAX_FILES。test_rw_handler は pipe を 100 個作る）、select はアプリの fd_set の n ビットだけを読み書きし（カーネルの FD_SETSIZE に頼らない）、アプリには `-DFD_SETSIZE=1024`（Linux と同じ。picolibc は 64）、struct tm の tm_gmtoff・tm_zone（cc/slimcc_headers/include/time.h が picolibc の time.h の写しに足す。localtime などはアプリに os/libc/posix.c の版を渡す）、`_GNU_SOURCE` で environ（unistd.h）・malloc_usable_size（stdlib.h）の宣言（cc/slimcc_headers/include）、setuid・setgid（0 だけ）、tzset・malloc_usable_size を OS API の表に、utime は FAT の範囲の外を端に寄せる（前は 1970 年が 2098 年になった）。
  - 確かめ: make cc-test、verify.sh quick（一致）、shell.sh、linuxapi.sh（pipe・tm_gmtoff を足した）、tools/make/test.sh、実機の compat_dev.sh（fdtest は 256 個に）。
- 止まったら自動で再起動・止まる直前の記録・watch・メモリの記録（2026-11-20。ユーザーの「claude さん的にあったらいい機能」から、ユーザーが「全部」を選んだ。順に 1 → 4 → 2 → 3）:
  - **ウォッチドッグ**（os/kernel/watchdog.c）: 起動したらいつも入れ、コア 0 のタイマー割り込みが餌をやる。固まって割り込みが止まると約 16 秒でリセット。カーネルが止まる道（exception.c の 3 か所、syscalls.c の _exit）は kernel_halt にまとめ、表示を記録にも残して 5 秒後に再起動する。試しの起動（tryboot.c）は期限を過ぎたら餌をやめる（watchdog_starve。前は自分でウォッチドッグを入れ・止めていた）。シェルの `watchdog [on|off|status]`（off は止まったままにする: UART・デバッガで見たいとき）、`watchdog test halt|hang`（試験用）。QEMU は PM_RSTC に FULL_RESET を書いた瞬間にリセットする（時間を数えない）ので、CNTFRQ が 19.2MHz でなければ使わない（起動を繰り返したので気づいた）。
  - **記録の輪を再起動をまたいで残す**（os/kernel/klog.c）: 輪をカーネルの下の固定の番地 0x60000〜0x70020 に置き、印・書いた量・終わり方（KLOG_END_RUNNING・REBOOT・POWEROFF・HALT）・確かめの値を持つ。1 行書くたびにキャッシュから RAM へ（dc cvac）。次の起動の最初に印が合えば写し、klog_file_boot が dmesg に「previous boot: ended by reboot」などを書き、ふつうに終わらなかったとき（固まった・止まった）は RAM の最後の記録を /var/log/dmesg.crash に。シェルの reboot・poweroff は終わり方を記録する。実機で確かめた（ファームウェアはリセットでこの番地を消さない）: reboot → 「ended by reboot」、watchdog test halt → 約 25 秒で戻り「the kernel halted」と止まる直前の行、watchdog test hang → 約 35 秒で戻り「reset without a reboot (a hang and the watchdog?)」。電源を切ると消える（「no log left in RAM」）。
  - **watch**（bin/watch.c）: GNU procps の watch と同じ書き方（-n -t -d -g -e -x）。代わりの画面に出し、Ctrl-C で戻す。`watch -n 1 gpio readall`。
  - **大きなプログラムの起動を速くする**（vim の起動 約 2.1 秒 → 2 回目から 0.01〜0.4 秒。内訳はキャッシュの確かめと読み込み 1.0 秒・つなぐ 0.9 秒・部品の解放 0.3 秒だった。遅い起動の記録 MONOCS_SLOW_MS に段階ごとの時間を足して測った）:
    - コンパイルのキャッシュの部品を fmemopen の FILE から読んでいた（picolibc の fread が 1 バイトずつ getc を呼ぶ）→ メモリから直接読む slimcc_obj_load_mem（cc/assembler.c の asm_obj_load_mem。形は同じ）。
    - キャッシュを使えるかの確かめで、ソース・ヘッダー（vim は約 13MB）を起動のたびにハッシュし直していた → ハッシュをアプリの実行をまたいで覚え（カーネル用のヒープ。2 万個まで）、VFS がそのパス（とその下）を書き換える・消す・名前を変えるときに忘れる（os/fs/vfs.c の fc_invalidate → os/slimcc/cache.c の cache_file_changed。読んでいる間に書き換えられたものは覚えない: dep_gen）。
    - **つないだ結果を覚えて使い回す**（os/slimcc/run.c の img_take・img_put・img_release）: run・名前で動かすソースのプログラムを、つないだ直後のメモリ（コード・データ・BSS のひと続き。slimcc_image_range）の写しと一緒に、写しと合わせて 64MB まで（カーネル用のヒープ。数は 32 まで。2026-11-21 までは 3 つまでで、小さなコマンドでも埋まって vim が押し出された）覚え、次は写しを戻して __clear_cache してそのまま動かす。使わないのは、覚えたハッシュのファイルが書き換えられた（cache_src_gen）、プログラムのフォルダか /usr/lib の中が変わった（image_cache_changed）、オプションが違う、入れ子のアプリから、同じものが入れ子で動いている。実機で、大域変数が毎回最初に戻る、ヘッダーを書き換えると新しい中身、フォルダに .c を足す・消すと変わる、を確かめた。実行ファイル（cc -o・./prog）は今までどおり毎回読んでつなぐ。
  - **メモリの記録**: mem に「peak in use」（カーネル用・アプリ用の使用中の量のいちばん多かったとき。os/libc/malloc.c の note_use。`mem -r` で今の量に）。1 回のコンパイルが 64MB 以上使ったら dmesg に「cc: FILE used N MB (the largest function in the IR: F, M MB)」（cc/libslimcc.c が、カーネルが定義する弱いシンボル slimcc_mem_report を呼ぶ。コンパイラの中の書き換える変数はコアごと（TLS）にする決まりなので、フックの変数を持たない。中間表現で関数ごとに確保した量は cc/ir.inc の ir_fn_bytes）。確保の失敗は「memory: cannot allocate …」（1 秒に 1 回まで。malloc.c から弱いシンボル malloc_out_of_memory）、ヒープの残りが 32MB を切ったら「memory: the heap is nearly full」（os/kernel/memory.c）。実機で vim をキャッシュなしで作り直して「cc: /root/apps/vim/regexp.c used 82 MB (… nfa_emit_equi_class, 27 MB)」、peak 約 208MB。
- 標準出力のロック（2026-11-19。ユーザーの malloc を繰り返して大量に出力するプログラムで、シェルが止まった）: syscalls.c の out_lock がただのフラグで、ssh の exec の出力の輪がいっぱいで待つ間に timeout・Ctrl-C でアプリが止められると、持ったまま残った。カーネルのロック（struct klock。持つ間は中断を後回し）にし、輪が空くのはロックの外で待つ（rexec.c の rexec_putc_locked）。sshd が知らせたコア 0 の番地（stdout_putc）でわかった。同じとき 1 回だけ出た ssh の MAC の誤り（Bad packet length）は再現しなかった（exec 5 分・対話 2 分 24MB・Ctrl-C 3 回）。
- 全部ソースのままに戻した（2026-11-19。ユーザーと合意: 「run aaa.c で実行できてキャッシュされてた時の方がプログラムしてて楽しかった」。Claude も「Monocs の目的（電源を入れたらすぐ C を書いて、その場でコンパイルして動かす）に一番合うのは全部ソースの形」と考え直した。フォルダの構成（FHS）と、make・cc -o・実行ファイルの仕組みは残す）: /usr/bin（bin/*.c とフォルダ）・/usr/sbin（services の .c とフォルダ）・/usr/local/bin（mi・fd・lua のソースのフォルダ）。シェルは名前を実行ファイル・フォルダ・名前.c の順に探し（find_in・find_path_app を 11-16 の形に）、slimcc_run_source でその場でコンパイル（キャッシュあり）。svc は slimcc_load_program（実行ファイルかソース）。/etc/services.txt は /usr/sbin/httpd.c など。/autorun.c に戻した。イメージで実行ファイルを作る段（build_in_qemu.sh・mkbuild.sh）と migrate_layout.sh を消した。update_bin.sh はソースを入れ替える。push.sh はソースのフォルダを /usr/local/bin/名前 に送る。
  - フォルダの構成（2026-11-19。ユーザーと合意: ソースのまま動かす今の形に合わせて、わかりやすく）: /usr は Monocs に付いてくるもの（更新で入れ替わる）: /usr/bin（コマンド）・/usr/sbin（サービス）・/usr/lib・/usr/include・/usr/share/samples（前の /usr/share/monocs/samples）・/usr/src/kernel（前の /usr/src/monocs/kernel）。/root はホーム（自分のもの）: /root/apps（自分のアプリのソース。前の /usr/local/bin。PATH は `/root/apps:/usr/bin`）。/etc・/tmp・/var はそのまま。ホームをまるごと写せば、自分のものの控えになる。
  - /tmp は起動のたびに空にする（2026-11-19、ユーザーの依頼。Linux の tmpfs と同じ。shell.c の clear_dir）。QEMU の試験は起動の前にイメージに入れるファイルを /root/test に置く（/tmp に置くと消える）。実機の /root/work（前の /work、試験の残り）と /.cache（前の TLS の控え）は消した。
  - make は実行ファイルを作らない（2026-11-19、ユーザーと合意）: 作り方を選ぶ（.monocs-make）・コンパイルして誤りを先に見る・キャッシュに入れるだけ（shell.c の make_build が slimcc_load_program）。前の形で作った実行ファイルがフォルダに残っていれば消す（./prog でソースより先に動くため）。実行ファイルは cc -o でだけ作る。
  - あわせて直した不具合: FatFs はフォルダを開くと FR_NO_FILE を返すので、VFS がフォルダを「ないパス」として覚え、続く stat がフォルダを「ない」と答えた（実行ファイルかを fopen で確かめたときに起きた）。フォルダなら覚えず EISDIR に（vfs.c）。slimcc_is_executable は stat でふつうのファイルのときだけ開く。
- Wi-Fi を速くする（2026-11-22。ユーザーの希望）: 先に内訳を測った。`wifi perf [reset]`（os/shell/wifi_cmd.c。wifi.c の poll_rx・lwIP に渡す・送る時間と、sdio.c の CMD52・CMD53 の数とコマンド・データの準備・PIO・転送の終わりの待ちの時間。timer_count（CNTPCT））と、`tools/perf/wifibench.c`（Monocs で 1 回だけ TCP を受け付け、送る・受ける）・`wifibench.py`（Pi 5 から）。わかったこと: 1 枠あたり CMD53 が約 4.5 回、「データの準備を待つ」時間は線の上の時間（25MHz・4 ビット = 12.5MB/s）とほぼ同じ、そして転送の間の半分〜8 割は Monocs が何もしていない（往復の時間が負荷で 4.5 → 11ms（最大 25ms）に延び、64KB の窓では 3.5〜4MB/s で頭打ち。リンクの速さは 65〜72Mbps）。直したもの: (1) SDIO を High Speed（CCCR 0x13 の EHS、50MHz。os/drivers/sdio.c。コントローラの HCTL_HS_EN は立てない: Linux の sdhci-iproc の bcm2835 は SDHCI_QUIRK_NO_HISPD_BIT）。ファームウェアの書き込みと読み直しに失敗したら 25MHz で初めからやり直す（wifi.c の wifi_start。dmesg に「SDIO high speed failed」）。SDIO にかかる時間がほぼ半分（受ける向き 1,060 → 720ms / 8MB）。(2) TCP の受信の窓を 128KB に（lwipopts.h の LWIP_WND_SCALE・TCP_RCV_SCALE 1、TCP_WND 88 × MSS、tcp_api.c の RX_SIZE 128KB。送信バッファは 64KB のまま: 128KB にしても送る向きは速くならなかった）。`wifi status` にリンクの速さ（WLC_GET_RATE）。結果（前のカーネルと交互に、16MB）: 受ける向き 3.5〜3.7 → 3.6〜5.1MB/s、送る向き 2.7〜3.1 → 2.3〜2.8MB/s（測る時間帯で 2 割以上揺れ、送る向きは Monocs が 8 割の時間何もしていないので、電波の揺れと見ている）。1 枠あたりのコマンドを減らす・glom・DMA は、Monocs の処理を減らすだけで速さには効かない見込みなので見送り。確かめ: verify.sh quick（一致）、実機（試しの起動、50MHz でファームウェアの読み直し・接続・DHCP）。
- slimcc の if 変換（2026-11-26。docs/slimcc-codegen.md の 8）: 値を選ぶだけの小さな分岐を csel に（cc/ir.inc の IR_SELECT・ir_if_convert）。実機で予測できない分岐のループ 396 → 186ms、カーネルの中のコンパイラは 0.6% 遅い。試験 select_gen.py（cc-test の 8e）、verify.sh（全部一致）。
- slimcc: 結果の変わらない段を省く（2026-11-27。docs/slimcc-codegen.md の 22。LICM の後の ir_dce、各回の ir_dominators）: 実機の codegen.c 1.1% 速い。出すコードは変わらない、verify.sh（全部一致）。
- slimcc のコンパイラの中を軽くする続き（2026-11-27。docs/slimcc-codegen.md の 21）: ir_mark_low32 を後ろから、ir_fixup は片付けていない置き換えがあるときだけ。実機の codegen.c 1.0% 速い。出すコードは変わらない、verify.sh（全部一致）。
- slimcc のコンパイラの中を軽く（2026-11-27。docs/slimcc-codegen.md の 20）: ir_cleanup の phi を探すループを phi でない命令で止める、ir_dce を後ろからたどる。実機の codegen.c 2.2% 速い。出すコードは変わらない（cmp_s.sh）、verify.sh（全部一致）。
- slimcc: 冗長な読み出しの除去で同じ回の置き換えを反映（2026-11-27。docs/slimcc-codegen.md の 19。ir_load_elim）: 実機 0.8% 速い。verify.sh（全部一致）。
- slimcc: 局所変数の配列の b[i] を [番地, i] に（2026-11-27。docs/slimcc-codegen.md の 18。ir_fold_addresses）: 実機の codegen.c 0.6% 速い。verify.sh（全部一致）。後置の番地の形は見送り。
- slimcc: 書き込みの前の拡張を省く（2026-11-27。docs/slimcc-codegen.md の 17。ir_simplify の IR_STORE）。速さは変わらず、少し小さく。17 に GCC 版との差の残りと次の候補（誘導変数の強度の削減・後置の番地の形）。
- slimcc: switch のビットの表（2026-11-27。docs/slimcc-codegen.md の 16。ir_switch_bits。行き先 3 つまで・幅 64 まで）: コンパイラ自身は変わらず、文字の分類の switch で実機 7.5% 速い。試験 switch_gen.py の shared、verify.sh（全部一致）。
- slimcc: switch の表引き（2026-11-27。docs/slimcc-codegen.md の 15。ir_switch_table・IR_SWTAB、内蔵アセンブラの .word A - B）: 実機で 0.4% と小さい。試験 switch_gen.py を広げた、verify.sh（全部一致）。
- slimcc の中間表現のコード生成を分けた（2026-11-27。ユーザーの「将来違う CPU を使う場合に備えたい」から）: ir.inc（ターゲットに依存しない）、ir_a64.inc（AArch64 のレジスタ・呼び出し規約・即値）、ir_a64_emit.inc（AArch64 の命令を出す）。出すコードは変わらない（cmp_s.sh・cmp_o.sh）。残る AArch64 の前提と、別の CPU を足すときの見込みは docs/slimcc-codegen.md の 0.1。
- slimcc: 定数の計算の直し（(x + c1) + c2 のまとめ、負の即値、定数の掛け算をずらす命令に、4 バイトの読み出し・戻り値を広げない。2026-11-27。docs/slimcc-codegen.md の 14。GCC 版と関数ごとの命令の数を比べて見つけた）。試験 gccmatch/arith_imm.c、verify.sh（全部一致）。
- slimcc: 退避の要るレジスタを stp・ldp で 2 つずつ、memcpy のある関数でも x0〜x5 を値に（2026-11-27。docs/slimcc-codegen.md の 12 の続き・13）: 実機のカーネルの中のコンパイラ約 1.3% 速い。verify.sh（全部一致）。
- slimcc: 呼び出しのある関数でも x0〜x7 を値に（2026-11-27。docs/slimcc-codegen.md の 12。IR_X0・ir_fixed_hints・ir_par_move）: 命令の数 約 10% 減、実機のカーネルの中のコンパイラ 3〜4% 速い。試験 gccmatch/parmove.c、verify.sh（全部一致）、QEMU の試験、実機の compat_dev.sh と Lua の公式テスト。
- slimcc: a && b を cmp・ccmp に、1・0 を選ぶ csel を cset に（2026-11-27。docs/slimcc-codegen.md の 11。ir_ccmp_chain）。試験 ccmp_gen.py（cc-test の 8f）、verify.sh（全部一致）。
- slimcc: 1 ビットを見る分岐を tbz・tbnz に（2026-11-27。docs/slimcc-codegen.md の 10）: 32KB を超える関数では飛び越す形に直す（ir_relax_tbz）。試験 gccmatch/tbz.c、verify.sh（全部一致）。
- slimcc: 引数の定数を x0〜x7 に直接作る（2026-11-27。docs/slimcc-codegen.md の 9。cc/ir.inc の ir_arg_const）: 命令の数 約 2% 減、実機のカーネルの中のコンパイラ約 1% 速い。verify.sh（全部一致）。
- C の Web のフォルダのページ（2026-11-26）: 目録 cweb.txt（sha256sum の形）で、いくつものファイルのページを取ってきて確かめる（os/slimcc/sandbox_fetch.c の sandbox_prepare・os/slimcc/sandbox_page.h、sandbox_cmd.c の sandbox_run_files）。#sha256= は目録のハッシュ。/usr/bin/sha256sum（GNU と同じ出力、-c。OS の関数 monocs_sha256 = os/kernel/sha256.c の sha256）。GCC 版で -Warray-bounds（名前が空のとき base[bl - 1]）が出たので直した。確かめ: QEMU・実機（Pi 5 から）、verify.sh quick（一致）。
- C の Web の最初のページ（2026-11-26。docs/cweb.md の 4）: samples/cweb/ の index.c（目次）・snake.c・life.c・about.c（curses。`cweb /usr/share/samples/cweb/index.c`）。Makefile の sdcard.img の依存に samples/*/* を足した。QEMU で全部のページを開いて戻れた。
- C の Web のブラウザ（2026-11-26。docs/cweb.md の 3）: シェルの `cweb URL|FILE.c[#sha256=…]`（os/slimcc/cweb_cmd.c）。ページは /usr/lib/sandbox/cweb.h の cweb_go・cweb_back（システムコール SYS_GO 7。sandbox.c は SANDBOX_NAVIGATE で戻り、sandbox_nav で移る先を渡す）。sandbox_cmd.c を sandbox_run_file（ファイルをコンパイルして動かす。結果の種類も返す）に分け、プログラムにも -I /usr/lib/sandbox（キャッシュのキーも変えた）。履歴 32、相対・/・http:// のリンク、.. は断る、リンクの #sha256= を確かめる（手元のファイルも）。Ctrl-C で 1 つ前へ戻り、Ctrl-\\ で終わる（2026-11-26 に足した。uart_interrupt_is_quit で見分ける）。確かめ: QEMU（手元のページの行き来・戻る・まちがったハッシュ・終わる）、実機（Pi 5 から a → b → c、ハッシュは最初だけ）、verify.sh quick（一致）。
- `sandbox http://…`（2026-11-26。docs/sandbox.md の 5.0・8 の 5、C の Web の 2）: os/slimcc/sandbox_fetch.c が HTTP/1.0 で取ってきて（カーネルの tcp_api・dns_api）、SHA-256（os/kernel/sha256.c）で確かめる。#sha256= が合わなければ動かさない、なければ前に許したハッシュと比べて変わっていたら聞く、初めてならハッシュを出して覚える。置き場所 /var/cache/sandbox/<FNV-1a>/。https はまだ。httpd に公開のフォルダ /root/share（Wi-Fi からもパスワードなしで GET・HEAD、.. を含むものは公開にしない）。確かめ: 実機（Pi 5 の python3 -m http.server から。ハッシュあり 7・まちがい 1・なし・変わったら聞いて 30 秒で断る・404、公開のフォルダ 200・.. 401・外 401）、verify.sh quick（一致）。つまずき: make の失敗（implicit declaration）を grep error で見落とし、古いカーネルを送った。make の終了の値で確かめる。実機の /root/share はこのときに作り、試しの hi.c を置いた。
- サンドボックスのキャッシュ（2026-11-26）: sandbox_cmd.c が slimcc_compile の代わりに cache_compile（キーの頭に sandbox・sandbox-lib）。実機の sandbox hi.c が 0.87 → 0.013〜0.032 秒（2 回目から）、ソースを直すとそのファイルだけ 0.069 秒。
- サンドボックスの curses（2026-11-26）: OS の curses（os/curses/curses.c）を書き換えずにサンドボックスの中でもコンパイルする。tools/tls/mklib.sh が curses.c・curses_os.h を /usr/lib/sandbox/curses/、curses.h を api/include/、wcwidth.c・表を libc/ に写し、頼り先の OS の関数は lib/sandbox/kernel_shim.c（と drivers/・kernel/ の代わりのヘッダー）がシステムコールで代わる。sandbox_cmd は頭のフォルダと curses/・libc/ の .c をコンパイルする。カーネルは SYS_WRITE の中の ESC [ ? 1049 h・l を見て、curses の画面のまま終わったら端末を戻す（最初は 7 文字の並びを 8 文字で探していて見つからなかった）。Monocs の curses に box がなかったので、ncurses と同じ形の wborder・box・border（と mvwhline・mvwvline のマクロ）を足した（os/curses/curses.c、curses.h、api.def。run とサンドボックスの両方で確かめた）。実機で 1 回 約 0.8 秒（毎回コンパイル）。
- サンドボックスの段階 4（2026-11-26。docs/sandbox.md の 8 の 4、curses のほか）: システムコール 2 読む・3 画面の大きさ・4 時刻・5 眠る・6 乱数（os/kernel/sandbox.c）。待つもの（読む・眠る・画面の大きさ・書く）は例外の処理の中で割り込みを入れて（msr daifclr）待ち、10ms ごとに Ctrl-C（uart_shell_interrupts）と時間の上限を調べる（check_stop。leave の前に割り込みを止める）。読んだバイトに Ctrl-C（3）があれば渡さずに止め、止めたら残りの入力を捨てる（捨てないとシェルが ^C を読み直した）。sandbox_cmd はコンパイルの途中の Ctrl-C でも動かさない。ライブラリ（lib/sandbox）に unistd.c（read の行の編集・termios・ioctl・poll）・time.c、stdio.c に読む関数、string.c・stdlib.c に strcspn・strtol など。確かめ: QEMU（画面の大きさ・眠る・乱数・poll・行の編集（Backspace）・1 文字ずつ・待っている途中と計算し続けている途中の Ctrl-C で 130・その後のシェル）、実機（sbtest、眠る・乱数・poll・時刻、ssh を切ると止まる）、verify.sh quick（一致）。
- サンドボックスの段階 3（2026-11-26。docs/sandbox.md の 8 の 3）: C のソースをサンドボックスで動かす。シェルの `sandbox FILE.c [引数]`（os/slimcc/sandbox_cmd.c）が FILE.c と /usr/lib/sandbox/*.c（lib/sandbox。SD カードへは tools/tls/mklib.sh・mkimg.sh）をコンパイルし、OS の関数にはつながずにサンドボックスの領域の中でつなぐ（memlink の確保を領域の頭に。足りない関数は undefined symbol）。sandbox.c は sandbox_region_alloc・sandbox_enter（引数を struct sandbox_info としてデータの頭に置き、入口の x0 に渡す。vectors.S の sandbox_eret の 3 つ目の引数）・sandbox_run（機械語の試し）に分けた。システムコールは svc #0 と x8 の番号: 0 終わる、1 書く（fd 1・2。ポインタを確かめ、例外の処理の中で fwrite）。MMU_SANDBOX_IMAGE（EL0 が読み書き・実行）を足した。picolibc の stdio.h で putc・putchar がマクロなので、ライブラリでは `int (putc)(…)` とかっこで囲む。確かめ: QEMU と実機（書式・引数・malloc・構造体のコピー・stderr・終了の値 7、fopen は undefined symbol、OS のメモリ・GPIO で 139）、sbtest、verify.sh quick（一致）。1 回 約 0.37 秒（キャッシュなし）。
- サンドボックスの段階 2（2026-11-26。docs/sandbox.md の 8 の 2）: EL0 で動かして戻るまで。os/kernel/sandbox.c（sandbox_run: コア 0 から、アプリでないときだけ。2MB 境界の 4MB をカーネル用のヒープから取り、コードの 2MB を EL0 の実行・読み出しだけ（AP 11、PXN）、データとスタックの 2MB を EL0 の読み書きだけ（AP 01、UXN・PXN）にする（mmu.c の mmu_sandbox_map。2MB のブロックの記述子を書き換えて tlbi vmalle1is）。vectors.S の sandbox_eret で、割り込みを止め、SP_EL0・ELR・SPSR（EL0t）・TPIDR_EL0 = 0、汎用・FP のレジスタを 0 にして eret）。下位 EL の入口（ventry_lower）は SP_EL0 をフレームの sp0（前の pad）に、EL0 の TPIDR_EL0 を sandbox_user_tp に置いてカーネルの TPIDR_EL0（sandbox_kernel_tp）に戻し、EL0 へ戻るとき（SPSR の M[3:0] が 0）に逆をする。exception.c の EXC_SYNC + 8 → sandbox_handle_sync（svc #0 = 終わる、ほかは止める）、EXC_IRQ + 8 → irq_dispatch の後 sandbox_after_irq（Ctrl-C・時間の上限）。出るときは例外の処理の中から longjmp。**RAM の記述子に UXN を足した**（ATTR_NORMAL・ATTR_NORMAL_RO。前は AP 00 で EL0 からの読み書きは止めていたが、EL0 から OS のコードを実行できた。EL1 には効かない）。確かめ: `sbtest`（終わる 42、スタック、OS のメモリを読む・OS のコードを実行・GPIO を読む・特権の命令・自分のコードに書く・知らないシステムコールで止まる、回り続けると 300ms で戻る。QEMU と実機で 9 項目 ok）、その後のアプリ、verify.sh quick（一致）、実機の primes 44ms（同じ）。まだ: システムコール（画面・キーなど）、中の C ライブラリ、C のソースをサンドボックスで動かす道。
- `cc --report`（2026-11-26。C の Web・サンドボックスの 1 歩目。docs/sandbox.md の 6 の 2・8 の 1）: os/slimcc/report.c。`cc --report [-l名前] FILE.c…` は作るのと同じ道でコンパイルしてつなぎ（できた /tmp/.cc-report は消す）、つなぐときに OS の表から引いた名前（run.c の resolve の report_note）を種類ごと（ファイルを開く・変える、ネットワーク、ほかのプログラム、機械の設定、ほかのコア）に並べ、-l のハードウェアのライブラリと、渡したソースのファイルの中のインラインアセンブリ・周辺機器の番地（注釈と文字列は飛ばす）を知らせる。確かめ: QEMU（hello・http_get・gpio_blink・わざと怪しく書いたもの・ないファイル）、verify.sh quick（一致）。run.c を変えたのでコンパイラの識別が変わり、入れ替えた後は全部のキャッシュを一度作り直す。
- Wi-Fi の省電力（2026-11-22。ユーザーの「Linux より省エネ？」から）: `wifi pm [0|1|2]`（os/shell/wifi_cmd.c。WLC_GET_PM 85・WLC_SET_PM 86。brcmfmac と同じ値: 0 切る、1 いちばん省電力、2 通信があれば起きている（Linux の既定））。測った（実機、同じ LAN）: 0 は ping（1 秒おき）平均 2.9ms・ssh の 1 往復 約 61ms・受ける 4.6MB/s・送る 2.8MB/s、2 は ping 平均 109ms（最大 240ms）・ssh 約 73ms・4.3・2.4MB/s、1 は ping 84ms・ssh 約 1.35 秒・0.7・1.2MB/s。減る電気の量は電流計がないので測っていない。ユーザーと合意して既定は 0 のまま（手で打つ使い方では、暇な後の最初の 0.1〜0.2 秒の遅れがもたつく）、コマンドだけ残す。再起動すると 0 に戻る（/root/.profile はサービス（wifid）より先に動くので、そこに書いても効かない）。電池で使うときに、起動時から 2 にする設定を足す。
- ライブラリのヘッダーが見つからないときの案内（2026-11-21。ユーザーが `run servo.c` で `#include <monocs_pwm.h>` が file not found になり、`-lpwm` の付け忘れとわからなかったことから。ユーザーと合意）: `#include` のファイルが見つからなかったら、コンパイラ（cc/libslimcc.c の ignore_missing_dep）が弱いシンボル `slimcc_include_hint`（cc/libslimcc.h）を呼び、カーネル（os/slimcc/run.c）が `/usr/lib/名前/` の直下か `inc/`・`include/` に同じ名前のファイルがあれば「note: monocs_pwm.h is in the library /usr/lib/pwm; add -lpwm (run -lpwm FILE, cc -lpwm, or LDLIBS = -lpwm in the Makefile)」を返して、エラーの文に添える（slimcc・firmware は除く）。勝手に -l を足すのは Linux の gcc と動きが変わるのでしない。エラーのときだけ調べるので、ふだんの速さは変わらない。確かめ: QEMU（monocs_pwm.h・bearssl.h（inc/）で案内、ないヘッダーは今までどおり、-lpwm で動く）、verify.sh quick（一致）、実機（Wi-Fi の試しの起動で入れ替え、/root/work で run servo.c に案内）。コンパイラの識別が変わるので、入れ替えた後は全部のキャッシュを一度作り直す。
- 起動の進み具合を出す（2026-11-21。ユーザーの「キャッシュをクリアして vim を起動すると数十秒待たされるが、フリーズかコンパイル中か判断できない」から）: run・名前で動かすソースのプログラムの読み込みが 0.5 秒を超えたら、標準エラーが端末のときだけ「vim: compiling 57/134 files」（キャッシュから読むだけなら loading、つなぐ間は linking）を同じ行に書き直して出し、終わったら消してから動かす（os/slimcc/run.c の progress_*）。書くのはファイルが 1 つ終わったときと、コンパイラが関数を 1 つ出し終えたとき（cc/libslimcc.h の弱いシンボル slimcc_progress。どのコアからも。sqlite3.c のような 1 つの大きなファイルの間も進む。0.2 秒に 1 回まで）。make・svc・cc -o では出さない。あわせて isatty(2) を Linux と同じに（`2>`・`2>&1` でファイル・パイプ・ssh のコマンドの間は 0。前はいつも 1 で、nos.py の出力に表示が混ざった。curl の進み具合も同じ判定）。確かめ: 実機の `ssh -tt` で `cache clear; vim --version > /dev/null`（compiling 0/134 → linking 134/134 → 消える）、nos.py cmd では何も出ない、isatty の 0・1・2 が端末・`2>/dev/null`・`2>&1 | cat`・ssh のコマンドで Linux と同じ。
- キャッシュの確かめに FAT の日時（2026-11-21。ユーザーと合意:「Claude さんのおすすめで」。再起動の後、キャッシュが使えるかの確かめでソース・ヘッダー約 18MB を読み直していた）: キャッシュの頭の読んだファイルの一覧に、FAT の生の日時（fdate << 16 | ftime。TZ で換算しない。os/fs/vfs.c の vfs_file_stamp。ファイルのキャッシュに載っていれば SD カードを読まない）を足した（CACHE_VERSION 4）。使うときは、大きさと日時が保存したときと同じなら中身を読まずに同じとみなし、そのハッシュをメモリの覚え（dep_remember）に入れる（ほかの .c も同じヘッダーを並べるので、2 回目からは stat もしない。最初の版は毎回 stat して、vim の 133 個で約 2 万回 VFS のロックを取り、3.0 → 3.6 秒に遅くなった）。日時が違えば今までどおり中身のハッシュで確かめる（touch だけならキャッシュを使う）。git の「racy」の扱いと同じく、保存したときに日時が今から 4 秒の内（か先）のファイルと、時計が合う前の決まった日時（2026-01-01 00:00）のファイルは日時を残さない（0。次も中身で確かめる）。見逃しうるのは、時計が戻った（電源を切った後の起動で fake-hwclock が最大 60 秒古い・手で日時を戻した）ときに同じ大きさ・同じ 2 秒の刻みで書き換えたものだけ。見逃したら `cache clear NAME`。確かめ: QEMU（同じ大きさの書き換え → 新しい中身、touch → キャッシュ、書き換えの直後 → 中身で確かめる）、verify.sh quick（一致）、shell.sh。実機: 再起動の直後の vim 3.0 → 2.38 秒（2 回とも。SD カードの読み出し 29.7 → 16MB）。残りの読み出しはキャッシュの部品（約 10MB）と、__DATE__ を使うので毎回コンパイルする vim の version.c の分。
- SD カードを High Speed（50MHz）に（2026-11-21。ユーザーと合意: 再起動の後の起動が SD カードの読み出しで決まっていたため）: `os/drivers/sdhost.c` が 4 ビットにした後、CMD6（SWITCH_FUNC、SD 仕様 4.3.10）の mode 0 で対応（状態のバイト 13 のビット 1、バイト 16 の下の 4 ビットが 1）を調べてから mode 1 で切り替え、ファームウェアに 50MHz を頼む（コアのクロック 400MHz の 1/8）。どこかで失敗したら 25MHz のまま（dmesg に「does not switch to high speed」）。`SD_HIGH_SPEED_AT_BOOT` を 0 にすると前と同じ。`mem` の行に「SDHOST 50 MHz」。
  - 試し方 `sdtest hs [N]`（`os/drivers/sd_test.c` の sd_selftest_hs。vfs_exclusive の中）: Wi-Fi だけでつないでいると EMMC で起動する sdtest は使えず（EMMC は Wi-Fi の SDIO）、区画をカードの終わりまで広げたので区画の外の空きもないため、SDHOST のまま試す。High Speed に切り替えたカードは 25MHz でも動くので、比べる相手は 25MHz で読んだもの。区画の頭の 8MB を 25MHz でまとめて読み、50MHz でいろいろな数・境界で読んで比べる。FAT32 の BPB から区画の最後のクラスタ（2MB）を求め、FAT の項目がすべて空きであることを確かめてから、50MHz で模様を書き・読み、25MHz でまとめて読み直して比べる。元の中身は 25MHz で読んでおき、最後に書き戻して確かめる。
  - 確認: QEMU（25MHz 起動で 2 回、50MHz 起動で 20 回、その後の fsck.vfat、verify.sh quick）、実機の sdtest hs 5 回・30 回（試験のほうの表の外を読む誤りで 17 回目にカーネルが止まった。ウォッチドッグで戻った。直して 30 回とも ok、FIFO に残った語・ブロックの終わりの時間切れ 0）、50MHz で起動してセルフホストのビルド（C ライブラリ 45 秒・カーネル 10 秒。どちらも Linux 上のビルドと一致。読み書きの誤り 0）。試す前に実機の /root・/etc を Pi 5 の build/sd-backup/files-20261121 に控えた。
  - 速さ（実機）: 2MB をまとめて読む 180 → 93ms（約 11 → 22MB/s）。書き込みはカードの中の状態で 140〜720ms とばらつき、変わらない。再起動の直後の vim の起動 3.9 → 3.0 秒（SD カードの読み出し 3.2 → 2.3 秒・29.7MB。小さなファイルが多くコマンドごとの待ちが残る）。セルフホストのビルドの時間は変わらない（計算と書き込みの待ちで決まる）。
- 再起動の後の起動を速くする（2026-11-21。ユーザーと合意、まず内訳を測った）: 再起動の直後（ファイルのキャッシュもつないだ結果の覚えも空）の vim の起動は 5.9 秒（キャッシュの確かめと読み込み 4.8 秒のうち SD カードの読み出し 4.39 秒・41.6MB、つなぐ 0.84 秒、解放 0.27 秒）で、コアは VFS のロック待ちと PIO の転送にいた。vim のコンパイルのキャッシュ 22.7MB（133 個）の中身を数えると、記号の表が 13.0MB（24.8 万個）で、そのうち 19.2 万個・10MB がコンパイラが関数の中に作る分岐先のラベル（.L.ir.N.M）。機械語・データは 3.6MB、リロケーション 2.7MB、頭（読んだファイルの一覧）3.4MB。保存（cc/assembler.c の asm_obj_save）で、リロケーションから名前で参照されていない .L のローカルな記号を落とす（GNU as も .o に入れない。.o の出力は変わらない）。キャッシュ全体 41.5 → 19.6MB、再起動の直後の vim 5.9 → 3.9 秒（読む量 29.4MB、確かめと読み込み 3.6 秒、つなぐ 0.19 秒、解放 0.03 秒。2 回とも同じ）。cc -o の実行ファイルも小さくなる（形は同じ）。残りの読み出しの多く（約 18MB）は、キャッシュが使えるかを確かめるためのソース・ヘッダーの読み直し（中身のハッシュ）。
- run を戻した（2026-11-19。ユーザーの「run aaa.c で実行できてキャッシュされてた時の方がプログラムしてて楽しかった」から、ユーザーと合意。実行ファイル（make・cc -o・./prog）はそのまま）: `run FILE.c|DIR|- [引数]` はその場でコンパイルして動かす（ファイルは作らない。tcc -run と同じ使い方）。キャッシュは run のときだけ（os/slimcc/cache.c を 2026-11-17 の直した版（ハッシュ表・コンパイラの識別）で戻し、置き場所は /var/cache/slimcc）。run.c の slimcc_run_source（compile_program の cached）、shell.c の cmd_run・cmd_cache。nos.py run も run に戻した。build/gen/compiler_id.h（Makefile の COMPILER_ID、mksrc.sh が /usr/src/monocs/kernel にも写す）も戻した。
- Wi-Fi で使えるまでを縮める（2026-11-17〜19。ユーザーと合意）: 起動の記録に段階ごとの時刻（`wifi: chip ready, scanning`・`scan done`・`connected`・`address`）。実機: サービス開始 1.0 秒、チップの準備 0.45 秒、スキャン 0.6 秒、つなぐ 2.8 秒、DHCP 約 2 秒。つなぐ命令（WLC_SET_SSID）に、スキャンでわかった相手の BSSID と chanspec（スキャンの結果の値のまま）を渡す（Linux の brcmfmac の brcmf_join_params と同じ。wifi_bss_of。ファームウェアがスキャンし直さない）: つなぐ 2.8 → 2.2 秒。DHCP の約 2 秒は、つないだ後 0.3 秒ずつ DISCOVER を送り直しても変わらなかった（6 回送り直しても 2.2 秒）ので、ルーターが渡す前にアドレスの使用を確かめている時間と思われる（Monocs の側では縮められない。試した送り直しは戻した）。
- シェルが止まったときの調べ（2026-11-17）: コア 0 は割り込みのたびに、いた番地と x30 を覚える（exception.c の core0_where、OS API の monocs_core0_where）。sshd が「waiting for another ssh command」と知らせるときに、その番地も出す（Pi 5 で kernel8.sym と比べる）。きっかけ: nos.py kernel の途中（新しいカーネルの起動の前。reboot tryboot のあたり）で、シェルが止まって電源を入れ直すしかなくなったことが 2 回あった（ほかの回は通った）。原因はわかっていない。
- 時刻を SD カードに覚える（2026-11-17。ユーザーと合意。Raspberry Pi OS の fake-hwclock と同じ）: /etc/fake-hwclock.data（「YYYY-MM-DD HH:MM:SS」UTC）。起動したらサービスより先に読み（clock_restore）、推定の時刻にする（clock_approx は真、clock_known は偽: make・ファイルの日時は推定を使い、HTTPS（tls.c の monocs_clock_known）はネットワークで合わせるまで待つ）。シェルが待つ間に 60 秒に 1 回まで・reboot・poweroff の前に書く（clock_save）。実機で再起動から 8 秒で推定の時刻が使えた。date は「（推定。…）」と出す。
- SD カードの配置（2026-11-13。ユーザーと合意: ルートを整理する。カーネルは /boot、本体は Linux 風に、試験の残りものは消す）:
  - ルートに置くのは、ファームウェアがルートから読むもの（bootcode.bin・start.elf・fixup.dat・config.txt）とフォルダだけ。`ls /` は apps bin boot config.txt etc include lib samples services src work（ほかの 3 つは隠し属性。`ls -a` で見える）。
  - /boot: kernel8.img・kernel8.sym・dtb・overlays/・LICENCE.broadcom。config.txt の `os_prefix=boot/`（Raspberry Pi の資料 config.txt: os_prefix はカーネル・dtb・overlays などに前置きされる。boot/ にカーネルと dtb がなければ無視してルートから読む。overlays は boot/overlays/README があるときだけ boot/overlays/ から）。`tools/mksd.sh` が作る。`nos.py kernel` は /boot/kernel8.img があればそこへ、なければ（古い SD カード）/kernel8.img へ送る。
  - /etc/services.txt（起動時のサービス。なければ /services.txt も読む）、/lib/firmware/brcm（Wi-Fi のファームウェア。Linux と同じ。なければ /firmware）、/lib/slimcc/include（コンパイラのヘッダー。`cc/platform/monocs.c`。古い場所には戻らない）、/boot/kernel8.sym（perf record。なければ /kernel8.sym）。
  - FAT の隠し属性: `monocs_dirent_hidden(DIR *)`（`os/libc/dirent.c`。readdir が最後に返したもの）。`ls` は「.」で始まる名前と同じく、-a がなければ出さない（Windows と同じ。Linux の vfat は見ない）。System Volume Information が消えた。
  - ルートのファイルのうち bootcode.bin・start.elf・fixup.dat に隠し属性を付けた（config.txt は人が直すことがあるので見せておく。ユーザーの提案、2026-11-13。最初は 4 つとも付けて試した: ユーザーと合意して実機で試した、2026-11-13）。GPU の ROM・ファームウェアが隠し属性のファイルを読むかは資料で確かめられなかったが、実機で付けたまま起動した（再起動 3 回。config.txt が読まれていることは、カーネルが /boot にしかないのに起動したこと・3Mbps の UART・PL011 が GPIO14/15 に出ていることでわかる）。これで `ls /` はフォルダだけ。`tools/mkimg.sh` も mattrib で付ける（`tools/mksd.sh` の build/sdcard/ は Linux のフォルダなので付かない。PC で SD カードに写したら、付けたいときは Monocs の fatattr で）。起動しなくなったら、PC で属性を外す（Windows: `attrib -h E:\bootcode.bin` など）。
  - `/bin/fatattr`（Linux の fatattr と同じ書き方: `fatattr +h FILE`、`-h`、属性を書かなければ表示。a・h・r・s）。中身は `monocs_fat_attr(path, set, clear)`（`os/fs/vfs.c`、FatFs の f_chmod）。
  - 実機の移し方（2026-11-13）: 新しい場所に置く → os_prefix の config.txt を置いて再起動 → 新しいカーネルで起動したこと（新しい API を使う ls が動く）を確かめる → 古いもの（/kernel8.*、dtb、/overlays、/firmware、/slimcc、/services.txt、/hello.txt、/rand1m.bin、/wifi.log、/bench、/work の中身）を消す → もう一度起動、Wi-Fi・HTTPS・perf record・hwcheck。消したカーネルなどは Pi 5 の build/sd-backup/root-20261113/。
- HTTPS（TLS。2026-11-13。ユーザーと合意: Monocs に TLS を入れる、置き場所は共有のライブラリ /lib、コマンドは curl。Pi 5 を中継役にする案はやめた）:
  - BearSSL（`third_party/bearssl`。サブモジュール、7bea48e = v0.6 の後の直し。MIT。無改造）。全 294 ファイルが slimcc で通り、BearSSL の test_crypto・test_x509 が slimcc でコンパイルしたもので合格（Pi 5）。slimcc は `__SIZEOF_INT128__` を定義しないので、64 ビットの速い実装（m62・m64・i62）は空になり、31 ビットのもの（i31・m31）が使われる。
  - カーネルには入れない。SD カードの `/lib/bearssl`（BearSSL のうち、TLS のクライアントが使う 86 ファイルとヘッダー。`lib/bearssl/files.txt`。一覧は `sh tools/tls/files.sh` が、GNU ld が書庫から取り出したものから作る）と `/lib/tls`（`lib/tls/tls.c`・`monocs_tls.h`。薄い層）。集めるのは `tools/tls/mklib.sh`（build/lib。`make sdimage` が入れる。実機には `sh tools/tls/mklib.sh && nos.py pushdir build/lib /lib`）。
  - `/lib/tls`: `monocs_tls_connect`・`send`・`recv`・`close`・`error`。BearSSL のエンジンの 4 つの口を自分で回す（時間切れのため br_sslio は使わない）。`br_ssl_client_init_full` と `br_x509_minimal`（鎖・名前・期限）。認証局は `/etc/ssl/certs/ca-certificates.crt`（Linux と同じ場所。イメージを作る Pi 5 のものを写す。リポジトリには入れない。148 個）。読み解くのが遅い（BearSSL の PEM・X.509 の読み解きは小さな仮想機械。実機で約 2 秒）ので、結果を `/.cache/tls/anchors.bin` に覚える（認証局のファイルの大きさと FNV-1a が同じなら使う）。日時は SNTP で合っていること（合っていなければ断る）。
  - 乱数: `getentropy`（`os/drivers/rng.c`。BCM2835 の RNG、0x3F104000。Linux の bcm2835-rng.c の手順。デバイスツリーの rng@7e104000）。カーネルに入れた（TLS の鍵に要り、Linux と同じ関数にするため。小さい）。実機で 16KB が 0.145 秒、1 のビットは 49.9%。
  - `curl`（`bin/curl/`。フォルダのアプリ。compile_flags.txt に `-ltls`）: http と https、-o・-O・-L・-I・-i・-s・-S・-f・-m・-X・-H・-d・-A・-v。Content-Length・chunked・閉じるまで。終了の値は curl と同じ番号。`tools/mkimg.sh` は bin/ のフォルダも入れる。
  - Linux で試す: `tools/tls/linux_shim.c`（monocs.h の TCP・名前・時計を Linux のソケットで）と一緒に slimcc でコンパイルする（files.sh が作る。`KEEP_CURL=パス` で残せる）。
  - 速さ（実機）: curl の最初の 1 回（88 ファイルのコンパイル）6〜9 秒、2 回目から https://example.com/ が約 1.2 秒（うち始まるまで約 0.7 秒、暗号約 0.3 秒）。
  - TCP の窓（`os/net/port/lwipopts.h`）: TCP_WND を 16 → 44 × MSS（64,240。窓の拡大なしの上限。接続ごとの受信バッファ 64KB に収まる）。遠いサーバーからの受信は「窓 ÷ 往復の時間」で頭打ちになる（765KB の取得 8.7 → 4.3 秒。Pi 5 は 2.1 秒）。TCP_SND_BUF も同じにした（窓だけ広げたら、カーネルの TCP エコーが送信バッファに入らない分を断り、5MB のエコーが時間切れになった。hwcheck で見つけた）。同じ LAN の Pi 5 からは約 750KB/s だった（チップからの受信を 10ms ごとのタイマー割り込みで読んでいたため。下の「受信を割り込みで読む」で約 1.2MB/s）。
  - 確認（実機、GCC 版・slimcc 版）: example.com・google・wikipedia・ecc256・rsa4096（badssl.com）につながる、期限切れ・名前の違い・自己署名・知らない認証局は断る（60）、-L の転送、-I、-f の 404（22）、765KB が Linux の curl と一致、パイプ（`curl -s ... | grep`）。QEMU でもコンパイル・リンクできる。verify.sh quick（一致）、hwcheck。
  - 残り: TLS 1.3・HTTP/2 はない（ユーザーと合意して見送り、2026-11-13。TLS 1.2・HTTP/1.1 で困っていない。TLS 1.3 は BearSSL が対応せず、別のライブラリへの載せ替えになる。つなぎたいサイトで TLS のエラーになったとき・HTTP/2 専用の API を呼びたくなったときに見直す。どちらも /lib と curl の中の話で、後から足せる）。
- SSH のサーバー（2026-11-13。ユーザーと合意: 段階 1〜3 = 暗号化された接続・ログイン・シェル。ファイルの転送は後で決める）:
  - サービス `services/sshd/`（フォルダのサービス。compile_flags.txt に `-lbearssl`。/services.txt に `/services/sshd`）。SSH の手順は自作（RFC 4253・4252・4254・5656・8731・8332・8308、OpenSSH の strict KEX）、暗号の部品は BearSSL（SHA-256・HMAC・AES-CTR・X25519・ECDSA・RSA の検証）。既存の SSH サーバー（OpenSSH・Dropbear）はプロセスと擬似端末が前提なので移植しない。
  - 方式は 1 つずつ: 鍵交換 curve25519-sha256、ホスト鍵 ecdsa-sha2-nistp256（最初の起動で作り `/etc/ssh/monocs_host_key` に秘密の数 32 バイト）、暗号 aes128-ctr、MAC hmac-sha2-256、圧縮なし。OpenSSH 10 の既定で、そのままつながる。chacha20-poly1305@openssh.com は BearSSL の Poly1305 が RFC 7539 の組み立て専用なので使っていない。
  - 認証: USB から届いた接続はどの方法でも通す（`ssh 192.168.7.2`。パスワードなし）。Wi-Fi からは、パスワード（/etc/htdigest。telnet・HTTP と同じ）か公開鍵（`/.ssh/authorized_keys`。HOME が / なので Linux の ~/.ssh/authorized_keys と同じ場所。ECDSA nistp256・RSA 2048 ビット以上。**Ed25519 はまだ**: BearSSL に Ed25519 の署名がない。自作すれば足せる）。公開鍵ではユーザー名を見ない。1 回の接続で 6 回、続けて 5 回まちがえたら 30 秒断る。
  - 接続: チャンネル 1 つ、`pty-req`（大きさ）・`shell`・`window-change`・`env`（受け取るだけ）。telnet と同じコンソールの写しにつなぐ（telnet と合わせて同時に 1 つ。使われていれば 1 行出して閉じる）。`exec`（ssh host コマンド）・`subsystem`（sftp）・ポートの転送はない。相手からの鍵の交換し直しに応える。
  - Linux で試す: `sh tools/ssh/test.sh`（`tools/ssh/linux_main.c`: TCP をソケット、コンソールを擬似端末の上の /bin/sh にして、GCC の検査付きで動かし、本物の ssh でつなぐ。パスワード、誤ったパスワード、ECDSA・RSA（rsa-sha2-256・512）の鍵、Ed25519・知らない鍵・ほかの暗号を断る、鍵の交換し直し、300KB の出力、USB 扱い、壊れた入力 300 通りで落ちない）。
  - /lib/bearssl に足したファイルは 3 つ（ec_keygen・ec_pubkey・ecdsa_i31_sign_raw。`tools/tls/files.sh` が sshd の分も拾う。89 ファイル）。
  - 実機（GCC 版）: 起動 1.1 秒（BearSSL のキャッシュがあるとき。カーネルを更新した直後は、起動時にコンパイルで数秒かかる。QEMU の起動も約 5 秒長くなった）。鍵交換から認証・チャンネルまで約 0.05 秒。USB からパスワードなし、Wi-Fi から誤ったパスワード・知らない鍵は断る、Wi-Fi から登録した ECDSA の鍵で通る。実機でのシェルの操作も確認した（ユーザーが確認。こちらでも USB 経由でコマンドの実行と exit）。
  - シェルの `exit`・`logout`（2026-11-13。ユーザーの「どうして exit で終わらないの」から。Linux に合わせる）: telnet・ssh でつないでいるとき、その接続を閉じる。シェルは `contap_request_hangup`（`os/kernel/contap.c`）で頼み、サービスは `monocs_console_hangup()` を見て、残りの出力を送り切ってから閉じる（ssh は exit-status 0 を知らせる。相手の ssh は 0 で終わる）。どこで打たれたかは「写しからの最後の入力が 2 秒以内か」で見分ける。シリアルのコンソール（USB・UART）で打ったときは、終わる先がないので 1 行出すだけ。行の頭の Ctrl-D も同じ（2026-11-14 から）。
  - 専門家の検査を受けた実装ではない。家の LAN の中で telnet の代わりに使うためのもの（インターネットに直接さらさない）。
- 圧縮の展開（2026-11-13。ユーザーと合意: gunzip・tar・curl --compressed。圧縮する側は作らない）:
  - `/lib/inflate`（`lib/inflate/inflate.c`・`monocs_inflate.h`。自作。RFC 1951・1952・1950）: `monocs_inflate(形式, 読む関数, 書く関数)`。gzip（続けて並んだ塊、後ろの詰め物も）・zlib・DEFLATE だけ・自動（HTTP 用）。32KB の窓、ハフマン符号は 9 ビットまで表を 1 回引く。CRC-32・Adler-32・大きさを確かめる。`monocs_crc32` も。
  - `/bin/gunzip`（-c・-k・-f。`.gz` → 元の名前、`.tgz` → `.tar`）、`/bin/zcat`（gunzip.c を読み込むだけ）、`/bin/tar`（x・t、v、f、-C、--strip-components=N、NAME の指定。gzip は中身で決める。標準入力も: `curl -sL URL | tar xz`。ustar・GNU の長い名前・pax の path。リンクは飛ばして警告。「..」を含む名前は受け付けず、先頭の / は取る。作る（c）はない）。どれもフォルダのアプリ（compile_flags.txt に `-linflate`）。
  - `curl --compressed`（Accept-Encoding: deflate, gzip を送る）。頼まなくても Content-Encoding が付いていれば展開する（`--raw` でしない）。本文の読み方を「読む関数」（`body_read`。Content-Length・chunked・閉じるまで）にまとめた。
  - 確かめる（Linux）: `sh tools/inflate/test.sh`（gzip -1・-6・-9、zlib、DEFLATE だけ、圧縮なしのブロックを、いろいろな中身と読む大きさで展開して元と比べる。GCC の検査付きと slimcc。壊した入力 4,500 通りで落ちない）、`sh tools/inflate/tartest.sh`（GNU tar・gzip と、展開したもの・一覧・--strip-components・日時を比べる。外に書かせない、途中で切れたものを断る）。
  - 実機: 765KB の .tar.gz（471 個）の展開 約 1.6 秒（Linux で展開したものと一致）。wikipedia の最初のページ 122,506 バイトが、圧縮では 29,183 バイトで届く。Pi 5（slimcc）での展開は約 58MB/s。
  - 気づいたこと: アプリから「Monocs かどうか」を知るマクロはない（`__has_include(<monocs.h>)` で見分けた。tar.c は Linux でもコンパイルして試すため）。picolibc の utime.h に utime の宣言はなく、monocs.h にある。
- /dev/null をファイルとして開ける（2026-11-13。Linux に合わせる。`os/fs/vfs.c`）: open・fopen・stat・fstat・read（すぐ終わり）・write（捨てる）・lseek・close。ファイルの表の枠だけを使い、SD カードには何もない（`ls /dev` には出ない）。シェルのリダイレクト（`> /dev/null`）は前からの別の仕組み。`curl -o /dev/null` が動く。
- 内蔵 Wi-Fi の受信を割り込みで読む（2026-11-13。`os/drivers/wifi.c` の wifi_handle_irq、`sdio.c` の sdio_card_irq_*、`irq.c`）:
  - チップは、読むものがあると SDIO のカードの割り込み（DAT1）を上げる（CCCR の Int Enable に全体・機能 1・2。チップの hostintmask は前から設定済み）。コントローラ（EMMC）の INTERRUPT・IRPT_MASK・IRPT_EN のビット 8（CARD）→ GPU 割り込み 62 番（デバイスツリーの interrupts = <2 30>）→ コア 0。10ms ごとのタイマーの読み出し（wifi_net_poll）も残してある。
  - バスを持っている間は、コントローラにカードの割り込みを見させない（`bus_taken`）。外すときに検出を入れ直し、チップが上げていれば読んでから、割り込みを上げさせる（`bus_release`）。lwIP の送信の中（wifi_send_eth）では読まない（届いたものを lwIP に渡すと、送信の途中の lwIP に入り直す）。割り込みは、バスが空いていれば読み、空いていなければ止めて戻る（持っているところが外すときに読む）。
  - **実機を止めた不具合**: 最初の版は、割り込みを止めるのに信号（IRPT_EN）だけを切り、状態の有効（IRPT_MASK の CARD）を切らなかった。自分の転送で立ったコントローラの印が残り、チップは上げていない（intstatus 0、CCCR の Int Pending 0）のに割り込みが上がり続け、コア 0 が割り込みから戻れずにシェルが止まった（ping には答える。コア 3 のサービスも、コア 0 が持ったままのロックを待って止まる）。SD Host Controller 仕様の Card Interrupt Status Enable の説明のとおり、切って入れ直すと消える（INTERRUPT に書いても消えない）。
    - 起動時に自動でつなぐので、電源を入れ直しても同じになった。復旧（SD カードは差し替えずに済んだ）: 電源を入れ直した直後に UART から、シェルに組み込みのコマンドだけを先に送る（`recv /services.txt …` で wifid のない一覧に置き換え、`reboot`。recv はデータを UART からそのまま読むので、コマンドとデータを続けて送れる。reboot は再起動の前に止まったが、書き出しは済んでいて、次の電源の入れ直しで戻った）。/bin のアプリ（mv）は起動が間に合わなかった。
    - 安全策: 読むものがない割り込みが 16 回続いたら、割り込みをやめて（割り込みコントローラでも止める）タイマーだけに戻す。`wifi events` に「rx by interrupt・timer」と割り込みの数、やめたときのチップの状態。`wifi irq on`・`off` で切り替えられる（比べる用）。
    - 教訓: 割り込みを新しく足すカーネルを送る前に、起動時にそれを使うサービスを外しておく（今回なら /services.txt から wifid）。上がり続ける割り込みは、UART も USB も効かなくする。
  - あわせて: poll_rx は、hostintmask のビットを読んだら全部消す（brcmfmac の dpc と同じ）。`wifi log`（wifi_read_console）もバスのロックの中で読む（前は、タイマーの読み出しと重なりえた）。
  - 効果（実機）: ping の往復 平均 10 → 5ms。同じ LAN の Pi 5 からの 3MB の取得 約 4.2 → 3.2 秒（転送は約 0.75 → 1.2MB/s。残りは、枠ごとの SDIO のコマンドの数と 25MHz。未着手）。遠いサーバーからは変わらない（窓 ÷ 往復の時間で決まる）。
  - つないだ直後に ping の応答が返らないことがある件（2026-11-13 に調べた。原因は突き止めていない）:
    - 起きていること: Monocs が相手の MAC アドレスを調べる最初の ARP の要求に、応答が来ない（約 7〜8 割）。lwIP の ARP のタイマー（1 秒ごと）からの再送には必ず 1〜5ms で応答が来る（20 回中 20 回）。その間、lwIP は待たせるパケットを 1 つしか持たなかったので、ping の応答が捨てられていた。相手がルーター（192.168.3.1）でも同じ。
    - 確かめたこと: 最初の要求と再送は 1 バイトも違わない。どちらもすぐチップに渡している（待ち行列で止まっていない）。割り込みで読むか・タイマーだけかに関係ない。チップの省電力は切れている（WLC_GET_PM = 0）、クロックは HT のまま（CHIPCLKCSR 0xd2）、ARP の肩代わり（arpoe・arp_ol）は 0。roam_off を 1 にしても同じ。送った直後にバスを 1 回読んでも同じ。ARP が済んだ後の ping の応答（4 秒おきでも）は取りこぼさない。つないで 30 秒以上たち、直前に通信があったときは、新しい相手への最初の要求にもすぐ応答が来た（4 回中 4 回）。
    - わからないこと: 「アプリ・受信の処理から単発で送った最初の要求」だけが失われる理由（チップの中か、電波か、ルーターか）。Pi 5 から 2.4GHz の電波は見えない（Pi 5 は 5GHz 側。Pi 5 のチップは ARP を自分で答えるので、Pi 5 の OS にも要求は見えない）。試していないもの: ampdu を切る（つないだ後は設定できない。WLC_UP の前に）、別のルーター。
    - 別のアクセスポイントで比べた（2026-11-13、ユーザーの iPhone のテザリング、2.4GHz、チャンネル 6）: ゲートウェイあての最初の ARP の要求に、8 回中 8 回、4〜21ms で応答が来た（家のルーターでは 6 回中 0〜1 回）。同じカーネル・同じ測り方なので、Monocs のドライバだけの問題ではなく、家のルーター（SSID は伏せる。2.4GHz と 5GHz をまたいで ARP を代わりに答えるなどの動きをする）との組み合わせで起きる。ルーターの中で何が起きているかは見えない。日本語の SSID にもつながった。
    - 対策: lwIP の ARP_QUEUEING を 1 に（ARP を待つ間のパケットを 3 つまで待たせ、応答が来てから送る）。0.1 秒おきの ping ではまだ取りこぼす（3 つを超える分）が、ふつうの通信（TCP は再送する）は 1 秒以内の遅れで済む。
    - 調べる道具: `wifi iovar NAME [VALUE]`（ファームウェアの設定の読み書き）は残してある。ARP・ICMP の送受信の記録（`os/net/net.c` の wtrace）と `wifi pm`・`wifi clk` は、2026-11-14 の整理で消した（要るときは git の a20f796〜d7139a2 にある）。
  - 確認: verify.sh quick（一致）、hwcheck（GCC 版・slimcc 版）、起動時の自動の接続、curl の https、待機中のコア 0 の idle 93%。
  - フォルダを移したら（2026-11-13、`~/dev/tmp/new_os` → `~/dev/c/unux`）、`rm -rf build/picolibc build/picolibc-install` で picolibc を作り直す（meson の設定に絶対パスが残り、`make sdimage-self` の mklibc.py が止まって、verify.sh が古いイメージで `kernel DIFF` になる）。
- シェルの `run`（`run -` はキーボードから打ち込んだもの。2026-11-07 までの `cc`）、`/autorun.c` は slimcc-a64 でコンパイルして実行する（`os/slimcc/run.c`）。`run -std=c17 FILE.c` のように規格を選べる（既定は gnu23）。
  - `-nostdlib` 相当で、外部のシンボルはすべて OS API 表（`os/api/api.def`）から探す。アプリから呼べる関数を増やすときは api.def に足す。
  - QEMU での確認（2026-11-04）: slimcc の test/ 104 本（TLS の 2 本を除く）を、`test/host/common.c` と 1 つのファイルに連結して Monocs 上で実行し、100 本が通った（GCC でビルドしたカーネルと slimcc でビルドしたカーネルの両方）。通らない 4 本はテストの環境の都合: `atomic.c`（pthread がない）、`macro.c`（`-I` を指定できない）、`function.c`・`function2.c`（別の翻訳単位が前提）。
  - Monocs 用のプラットフォームは `__GNUC__` を 4.2.1 と定義する（picolibc の `sys/cdefs.h` が `__GNUC__` なしでは `__ASMNAME` を定義せず、`__func__` を `NULL` にするため。版は clang と同じ。ユーザーと合意、2026-11-04）。`__extension__` は空のマクロ。slimcc の `limits.h` は Monocs では `_GCC_LIMITS_H_` を立てて、picolibc の `limits.h` が先を探さないようにする。
  - カーネルのスタックは 8MB（`os/boot/linker.ld`）。1MB では `countof.c`（約 1.4MB の VLA）が溢れてカーネルのコードを壊した。
- システムを守る（2026-11-12。ユーザーと合意。`os/kernel/mmu.c`・`vectors.S`・`exception.c`）: 単一アドレス空間・EL1 のまま、MMU で次の 2 つを行う。アプリも EL1 なので、わざと MMU を書き換えれば外せる（守るのはバグから）。
  - カーネルの .text・.rodata（0x80000〜.tbss の先頭のページの手前）を読み出し専用にする。書くと例外になり、`(write to read-only memory ...)` と出てシェルに戻る。
  - 各コアのスタックのいちばん下の 64KB をガードページ（無効）にする（`mmu_stack_guard`）。スタックは SP_EL1 で、例外も同じスタックに退避するので、ベクタの入口（現在の EL・SP_ELx の 4 つ）で SP がガードページの先頭（TPIDR_EL1）から 72KB の中かを調べ、そうならコアごとの非常用のスタック（16KB）に切り替えて `*** stack overflow` を表示し、アプリ（コア 1〜3 は仕事）を止める。調べる間の作業用には SP_EL0・TPIDRRO_EL0 を使う。64KB を飛び越える大きな局所変数は捕まえられない（slimcc はスタックを 1 ページずつ触る確かめを入れない）。
  - 2MB のブロックを 4KB ページに分ける表は 12 個まで（`L3_POOL`）。カーネルが大きくなって足りなくなると、残りは守られない（例外にはならない）。
  - 確認: QEMU と実機（GCC 版・slimcc 版）で、カーネルのコード（printf）への書き込み、再帰のあふれ（コア 0 とコア 1）、NULL への書き込みのあと、シェルとアプリが動き続けること。verify.sh quick、hwcheck.sh も通った。
  - アプリ用のヒープ（2026-11-12。ユーザーと合意。`os/libc/malloc.c`・`heap.h`、`os/kernel/memory.c`）: ヒープの領域の下からカーネル用（sbrk）、上からアプリ用（`memory_app_grow`）を広げる。どちらも TLSF。アプリの実行中（`run.c` の `malloc_app_begin`〜`malloc_app_stop`）の確保はアプリ用から取り、アプリが終わったら `malloc_app_discard` で丸ごと捨てる（free しなかったメモリも戻る。0 で塗りつぶすので、カーネルが捨てたものの中のポインタを使うと NULL を参照して例外になり、気づける。0 なのは memset が DC ZVA を使えて速いため。実機で、アプリが使った 1MB あたり 0xA5 では 0.9ms、0 では 0.65ms。300MB で 270 → 196ms。ふつうのアプリ（数 MB）では数 ms）。`mem` に最後のアプリのヒープの大きさと、free されなかった量が出る。
    - どちらのヒープのものかは番地で決める。解放・realloc は元のヒープで。どちらでもない番地の free（捨てたアプリのものなど）は `free: invalid pointer` と出して無視する。
    - アプリの実行中でも、アプリが終わった後まで残すものは `malloc_kernel_begin`〜`end`（コアごと）でカーネル用から取る: VFS のファイルのキャッシュ（`fc_load`）、TZ の setenv（`clock_set_tz`。アプリの `monocs_set_timezone`）、アプリから呼んだコンパイラ・リンカ（`slimcc_cmd_app`・`slimld_cmd_app`。コンパイラは状態を次のコンパイルまで持つ）。サービス（コア 3 の runner）はいつもカーネル用。ほかのコアの仕事の始めに深さを 0 に戻す。curses はアプリの終わりに自分で解放する（`curses_app_exit`）ので、捨てる前に呼ぶ。
    - アプリを始めるとき、カーネル用の終わりの空きを sbrk で返す（`trim_kernel`）。途中の空き（ファイルのキャッシュの間など）は返せないので、アプリ用が足りなくなったらカーネル用の空きからも取り（`alloc_any`）、印（F_APP）を付けて、アプリの終わりに解放する。実機でビルドの後に 390MB の確保を試して、毎回 369MB まで取れた（ヒープは 400MB）。
    - 確かめる: `tools/picolibc/test_malloc.c`（Linux。カーネル用・アプリ用・両方の乱数の確保、捨てた後のカーネル用のものの無事、終わりの空きを返すこと、カーネル用の空きから取ったものを解放すること）。
  - 実行時のヘッダーは SD カードの `/lib/slimcc/include`（`cc/slimcc_headers/include`。2026-11-13 までは /slimcc/include）と `/include`（picolibc と `monocs.h`）。
- セルフホスト（2026-11-05、QEMU）: Monocs 上でカーネル全体（OS、FatFs、組み込みの slimcc と slimld）をビルドできる。
  - 手順: `make sdimage-self` のイメージで起動し、`cd /src`、`run build.c`。ソースを 1 つずつ `slimcc` でコンパイルして `build/kslim/*.o` を書き、`slimld` で `build/kslim/kernel8.img` を作る。QEMU で約 45 秒。
  - シェルのコマンド `cc -c -o OUT.o [-I] [-D] [-std=] FILE`（.c・.S・.s）と `ld -o kernel8.img FILE.o... LIB.a...`（2026-11-07 に slimcc・slimld から改名。ユーザーと合意。中身はコンパイラ slimcc とリンカ slimld。ld は今は Monocs のカーネルを作る専用）。アプリからは OS API の `monocs_cc`・`monocs_ld`（`monocs.h`）。
  - 実機（Zero 2 W、2026-11-05）: `make sdimage-self-auto` で起動時に自動でビルドし、LED で成功を確認。できた kernel8.img の SHA-256 が Linux 上のビルドと一致した（e4963e98…）。
  - 確認: Monocs 上で作った .o 40 個と kernel8.img が、Linux 上の `make kernel-slimcc` とバイト単位で一致する。さらに、その kernel8.img で起動して同じビルドをした 2 世代目も一致する（不動点）。
  - 一致させるため、ドライバ（main.c）と libslimcc はオプションの後処理・予定義マクロ（`define_cli_macros`、`cc/options.c`）と、.S の前処理の出力（`print_tokens`、`cc/preprocess.c`）を共有する。/src のパスはリポジトリと同じ相対パス（`tools/selfhost/mksrc.sh`）。
  - picolibc は slimcc でビルドしたもの（下記）を使い、libgcc は使わない（cc/rt。下記）。
  - C ライブラリも Monocs 上でビルドできる（2026-11-06）: `cd /src`、`run buildlibc.c` で picolibc の 1,012 ファイルをコンパイルし、`build/picolibc-slimcc/libc.a` を作る。その後の `run build.c` はこの libc.a を使う（なければ Linux 上でビルドした `lib/libc.a`）。これで OS・コンパイラ・リンカ・C ライブラリのすべてを Monocs だけで作り直せる。
    - 材料: `tools/selfhost/mklibc.py`（`make sdimage-self` から）が、`tools/picolibc/build.py` の `plan()`（Linux 上のビルドと同じコマンド）から `libc_files.h`（オプション 6 通り、ファイル、libc.a のメンバーの順）を作り、ソースと、`slimcc -M` で調べたヘッダーを同じ相対パスで /src に写す（1,242 ファイル）。slimcc が自動で読む `bitint_builtins` は -M に出ないので別に写す。コンパイルは build/picolibc で行い、slimcc のヘッダーも `../../cc/slimcc_headers/include` の相対パスで渡す（Linux 上でも同じ。結果は変わらない）。
    - 組み込みの slimcc（`SlimccOptions`、シェルの `cc`）に `-U`・`-isystem`・`-nostdinc`・`-ffunction-sections` を足した。
    - ar: `tools/selfhost/ar.h`。GNU ar の決定的モードと同じバイト列（シンボル表と長い名前の表は、大きさを偶数にそろえた分も大きさに含める）。Linux で既存の .a を書き直して確かめる（`tools/selfhost/ar_test.c`。picolibc の GCC 版・slimcc 版、libgcc.a、glibc の libc.a・libm.a で一致）。
    - 確認: Monocs 上で作った libc.a と、それでリンクした kernel8.img が、Linux 上のビルドとバイト単位で一致する（QEMU 481 秒 + 60 秒、実機 136 秒 + 12 秒。実機は CRC32 で比べた）。
    - 実機での一周（2026-11-06）: Monocs が実機で作った kernel8.img を /kernel8.img にして起動し（元のカーネルは /kernel8.old に残す）、その上で `run buildlibc.c`・`run build.c` をやり直した。できた libc.a と kernel8.img は、起動しているカーネル・Linux 上のビルドと CRC32 が一致した（不動点）。
    - 実機の /src の更新は `nos.py pushdir build/selfsrc /src`（1 つの束にして送り、`tools/uart/unpack.c` が展開する。1,333 ファイル、約 33 秒）。
  - アプリから呼んだコンパイル・リンク（`monocs_cc`・`monocs_ld`）の間は、Ctrl-C とほかのコアの中断を、その 1 回の終わりまで後回しにする（`abort_defer_begin`・`end`、`os/kernel/lock.c`。ロックと同じ深さ）。途中で止めると、コンパイラの状態が壊れて次のコンパイルで例外になった（2026-11-12 に見つけた。前からあった。セルフホストのビルドを Ctrl-C で止めた後、次のビルドでほかのコアが例外・カーネルが停止）。どこで止めても戻るように後始末をそろえるのは確かめきれないので、止めない形にした。Ctrl-C から止まるまで、実機で 0.5〜1.7 秒（QEMU で約 8 秒）。
    - あわせて、コンパイルの入口で前のコンパイルが途中で抜けていたら（コンパイラの中の例外）状態を捨てる（`compile_busy`、`cc/libslimcc.c`）、`codegen_reset` で中間表現の確保も捨てる（`ir_free_all`）。
- picolibc の slimcc でのビルド（2026-11-05、`make picolibc-slimcc`）: meson が GCC で設定した `build/picolibc` の `compile_commands.json` を読み、コンパイラを `cc/slimcc-monocs` に置き換えてコンパイルし、GCC 版と同じメンバー・同じ順の `libc.a` を作る（`tools/picolibc/build.py`。設定 `picolibc.h` などは GCC のときのもの）。
  - 複素数の関数（libm/complex の 72 本）は除く（slimcc が `_Complex` に対応していない）。
  - `memcpy`・`memset`・`strlen` などは picolibc のアセンブリ版（NEON を使う .S）を使う。そのため picolibc のビルドにだけ `-D__ARM_NEON=1 -D__ARM_FEATURE_UNALIGNED=1` を渡す（slimcc は `arm_neon.h` の組み込み関数に対応していないので、アプリには定義しない）。QEMU で memcpy は C 版の約 2.2 倍、memset は約 3 倍速く、GCC 版と同じ速さ。
  - 内蔵アセンブラの SIMD: add・sub・addp・cmeq（0 との比較も）・cmhs・cmhi・cmgt・cmge・umaxp・uminp・umax・umin・bsl・bit・bif・shrn・rshrn・dup・ld1・st1（ほかに既存の and・orr・eor・bic・mov・fmov・ldr/str/ldp/stp の q レジスタ）。ccmp・ccmn、マクロのキーワード引数（`name=value`）も足した。GNU as と一致（`cc/test-a64/asm/neon.asm`）。
  - 確認: `tools/picolibc/libctest.c`（printf・strtod・数学関数・文字列・qsort・malloc など 2133 行の出力）が、GCC 版と slimcc 版の picolibc で一致する。
  - そのために足したもの: Monocs 用のプラットフォームに GCC の浮動小数点の予定義マクロ（`__FLT_*`、`__DBL_*`、`__LDBL_*`）と、picolibc が使う組み込み関数のマクロ（`__builtin_isnan`、`__builtin_huge_val`、`__builtin_signbit`、`__builtin_isless`、`__builtin_clz` など。`cc/platform/monocs.c` の gcc_builtin_macros）。条件が定数の `?:` は選ばれる側だけを出す（`cc/codegen.c`）。内蔵アセンブラに `. = 式`、`.variant_pcs`、整数の接尾辞（`1LL`）。slimcc の `limits.h` は Monocs で `INT_MAX` などを自分で定義する（picolibc の `limits.h` が `__GNUC__` のときコンパイラに任せるため。`__GNUC__` を定義してからアプリで `INT_MAX` が使えなくなっていたのも、これで直った）。
  - OS API 表に、`isnan`・`isinf`・`fpclassify` などのマクロが呼ぶ関数（`__isnand` など）、`ctype.h` のマクロが参照する `_ctype_b`、数学関数（`asinh`、`sqrtl` など）を足した。
- libgcc の置き換え（2026-11-05）: slimcc 版のカーネルとセルフホストは libgcc を使わない。long double（binary128）の演算・比較・変換は `cc/rt/softfp128.c`、`__clear_cache` は `cc/rt/clear_cache.c`（自作。GCC 版のカーネルは今も libgcc）。
  - 結果・例外フラグ・丸めモードは libgcc と同じ（`cc/rt/test_softfp.c` で 1 億件以上をビット単位で比較。`make cc-test` の 13）。
  - 速さ（2026-11-06）: 割り算は Knuth の Algorithm D（32 ビットずつ、`udiv` で商を見積もる）、`clz` は命令。両方が正規化数のときの足し算・引き算・掛け算は、128 ビットの値を 2 つの `uint64_t` の局所変数で持つ速い道（`add_fast`・`mul_fast`）。slimcc は構造体をいつもメモリに置くが、アドレスを取らないスカラーの局所変数はレジスタ（x21〜x28）に置けるため。128 ビットの小さな操作と `round_mode`・`raise_flags` はマクロ（slimcc はインライン展開をしない）。
    - Pi 5 で slimcc でコンパイルしたもの: add 449 → 75ns、mul 589 → 168ns、div 629 → 497ns（同じソースを GCC -O2 で 17・34・176ns、libgcc の add は 11ns）。QEMU の slimcc 版のカーネルで div 112μs → 約 3μs。
    - 残りの差は slimcc のコード生成（レジスタに置ける変数が 8 個まで、インライン展開がない）による。足し算 1 回は約 470 命令（GCC は約 60）。
- 浮動小数点と 10 進の変換（2026-11-06）: picolibc を `-Dio-long-double=true` でビルドし（printf・scanf の `%Lg` など）、picolibc の不正確な変換を `os/libc/ldtoa.c` で置き換えた（picolibc の同名の関数より先にリンクされる。third_party は無改造）。
  - 置き換えたもの: `__ldtoa_engine`（printf の long double）、`__dtoa_engine`（printf の double。picolibc の ryu は、要求された桁数が最短の表記より多いと後ろを 0 にしていた。例: `%.17g` の 0.1 が `0.1`）、`__atold_engine`（scanf の long double）、`strtold`・`strtod`・`strtof`（picolibc は 10 進を 32・16・8 桁で打ち切ってから丸めるので、長い入力で丸めを誤っていた）。
  - 方法: 値を 10 進の桁の並び（最大 17,200 桁）で正確に持ち、2 のべき乗の掛け算・割り算を桁の上で行って、最近接・偶数への丸めで正しく丸める（Go の strconv と同じ手順）。`ERANGE` は glibc と同じ（無限大、または丸める前に最小の正規化数より小さく、かつ丸めが起きたとき）。
  - 確認: `tools/picolibc/test_ldtoa.c` で glibc と値・`endptr`・`errno` を比べる（`make cc-test` の 14）。QEMU の Monocs 上でも、long double の値・printf の出力・`strtod` の境界の入力が Linux と一致する。
  - 残る違い（picolibc の作りで、規格が認める範囲）: printf の double は 17 桁、long double は 34 桁より先を 0 にする（long double の 35・36 桁目は規格の DECIMAL_DIG に届かない）。scanf の `%f`・`%Lf` は 10 進を打ち切ってから変換する（`strtod` などは打ち切らない）。直すには picolibc のパッチが必要（ユーザーと相談のうえ見送り、2026-11-06）。
- マルチコア（段階 1、2026-11-06）: `os/kernel/smp.c`。
  - 起こし方: ファームウェアはコア 1〜3 をスピンテーブルで待たせている。コア n の飛び先を 0xD8 + 8n 番地に書き（デバイスツリーの `cpu-release-addr` で 0xE0・0xE8・0xF0 を確認）、キャッシュから書き戻して `sev`。0 番地のページは NULL の検出のため無効にしてあるので、書く間だけ有効にする（`mmu_map_page0`）。QEMU の raspi3b も同じ番地。
  - コア 1〜3 は `boot.S` の `secondary_entry` から EL1 に降り（EL2 → EL1 の手順はコア 0 と共通のマクロ）、コアごとの 8MB のスタック（`linker.ld` の `.stack_smp`。slimld の配置の表も同じ）で `secondary_main` に入り、MMU を有効にして仕事を待つ。
  - API: `smp_run(コア, 関数, 引数)`、`smp_wait(コア)`、`smp_cores()`。アプリには `monocs_core_start`・`monocs_core_wait`・`monocs_cores`（`monocs.h`）。例: `samples/primes_mc.c`（QEMU で 4 コアで約 3.3 倍）。
  - アプリが終わると（return・exit・例外・Ctrl-C）、`app_call` が `smp_abort_all` でほかのコアの仕事を止める（ARM ローカル周辺のメールボックス 0 の割り込み。コア 1〜3 は仕事中だけ IRQ を受ける）。コア 1〜3 で例外が起きたら、内容を表示してそのコアだけ待機に戻り、`smp_wait` が false を返す。
  - 段階 2（2026-11-06）: ほかのコアから C ライブラリ（printf、malloc、ファイル操作）を呼べる。`os/kernel/lock.c`。
    - picolibc は `-Dsingle-thread=false -Dstdio-locking=true` でビルドし、`__retarget_lock_*` を lock.c で用意する（再帰できるスピンロック。ldaxr/stxr）。picolibc は malloc・atexit などを 1 つのロック（`__lock___libc_recursive_mutex`）、fopen した FILE をファイルごとのロックで守る。FILE ごとのロックは 64 個の固定の表から配る（使い切ったら全体のロックを共用）。
    - `stdio-locking` がないと、fprintf などが 1 文字ごとにファイルのロックを取り、セルフホストのビルドが約 43 秒 → 59 秒に遅くなった（slimcc は open_memstream に fprintf で書く）。あると呼び出しごとに 1 回で、45 秒。コンソールの FILE はロックなし（`__LOCK_NONE`。getchar がキー入力を待つ間ロックを持たないように。`os/libc/syscalls.c`）。
    - VFS（ファイルの表と FatFs）は 1 つのロック（`vfs_lock`）で守る。FatFs の `FF_FS_REENTRANT` は使わない（FatFs を呼ぶのは VFS だけ）。コンソールの読み書きはロックの外。
    - OS・C ライブラリのロックを持っている間は、そのコアの中断を後回しにする（コア 0 の Ctrl-C は次のタイマー割り込み、コア 1〜3 は最後のロックを外したとき `smp_check_abort`）。同期例外で止まったときは `lock_release_all` でそのコアのロックを外す。
      - 待っている間は後回しにしない（取れた瞬間に数える。取る瞬間だけ割り込みを止める）。待つ間も後回しにしたら、ほかのコアがロックを取り直し続けるとき Ctrl-C が効かなくなった。
    - アプリ用のロックは `monocs_mutex_lock`・`monocs_mutex_unlock`（`monocs.h`。中断は後回しにしない）。例: `samples/libc_mc.c`。
    - 確認: ロックを外したカーネルでは `libc_mc.c` がすぐ例外になる。ロックありでは、全コアで malloc・ファイル操作中の Ctrl-C、malloc の中の例外の後も正しく動く（GCC 版と slimcc 版）。
  - 制限: コンソールへの出力はコアどうしで文字の単位で混ざりうる。
  - 段階 3（2026-11-06）: TLS（_Thread_local）とコアごとの slimcc。
    - TLS: `linker.ld` に `.tdata`・`.tbss`・`.tls_space`（コアごとに 1MB）。`os/kernel/tls.c` が各コアの TLS を初期値で用意し、TPIDR_EL0 を設定する（コア 0 は `mmu_init` の直後、コア 1〜3 は `secondary_main`）。TP からのオフセットは「.tdata からの位置 + align_up(16, TLS の境界)」（AAELF64、GNU ld と同じ）。
    - slimld: TLS の配置（GNU ld と同じく、.tdata の境界を .tbss の境界まで上げ、.tbss はアドレスを使わない）と、local-exec のリロケーション、initial-exec の local-exec への書き換え（`adrp`・`ldr` → `movz`・`movk`）。`make ld-test` で GNU ld と一致。内蔵アセンブラは、TLS のリロケーションで参照したシンボルを STT_TLS にする（GNU as と同じ）。
    - picolibc は `-Dthread-local-storage=true`（errno・strtok・rand などがコアごと）。アプリの errno は、memlink が resolve に "tls:errno" で TP からのオフセットを問い合わせ、`movz`・`movk` に書き換える（OS API 表の `API_TLS`）。アプリが定義する _Thread_local の変数は未対応。
    - slimcc（`SLIMCC_LIBRARY` のとき）: 書き換えるファイルスコープ・関数内の静的な変数に `TLS`（`cc/slimcc.h`、`cc/ld.c`。Linux 用のビルドでは空）。組み込みの型（`ty_int` など）もコアごとに持ち、`type_reset` で初期値に戻す。`os/libc/ldtoa.c` の作業領域もコアごと。
    - 確かめ方: GCC 版の `build/slimcc/*.o` で、書き込める .data・.bss が空であること（readelf）。漏れがあると、別のコアのコンパイルと壊し合う。
    - `tools/selfhost/build.c` は使えるコアすべてでコンパイルを分担する（大きいファイルから。リンクはコア 0）。結果は 1 コアのときと同じ（Linux 上のビルドと一致）。
    - 速さ（QEMU）: 1 コア 39 秒 → 4 コア 28 秒。1 コアのうち約 23 秒は VFS（SD カード）の中で、ここはコアを増やしても速くならない（VFS のロックで 1 つずつ）。
    - 並列にして見つかったこと: picolibc の malloc は空き領域を 1 本のリストでたどるので、内蔵アセンブラが 1 文字列ずつ malloc していたのを、64KB の塊から切り出す方式にした（2 コアで極端に遅くなり、止まったように見えた）。picolibc の FILE のバッファは `fstat-bufsiz`（VFS の `st_blksize` は 16KB）と `fast-bufio` で大きくした（37 秒 → 28 秒）。
    - picolibc の設定を変えたら、カーネルの .o も作り直される（Makefile。-MMD はシステムヘッダーを依存に含めないため）。
    - ファイルのキャッシュ（2026-11-06、`os/fs/vfs.c`）: 読み出し専用で開いたファイル（4MB まで）を、最初に全体を読んでメモリに置く。鍵は絶対パス（小文字にそろえ、`.`・`..` を除く）。書き込みで開いたとき・削除・名前の変更で捨てる（2026-11-20 から、ほかの fd で書いたものは、読み出し専用の fd の次の read・lseek・fstat で読み直す。下の「Lua の公式テスト」）。合計 64MB を超えたら、使われていない古いものから捨てる。`stat` もキャッシュから答える。`mem` コマンドで量とヒットの回数を見られる。
      - セルフホストのビルド（4 コア）: QEMU 28 秒 → 17 秒（2 回目は 15 秒）、実機（Zero 2 W）43 秒 → 28 秒（2 回目は 23 秒）。ヘッダーなど 189 ファイル、約 7MB。その後 SD カードの読み書きをまとめて実機 5 秒（下の「SD カードの読み書きをまとめる」）。
      - SD カードをほかの機械で書き換えたときのことは考えていない（差し替えたら再起動する）。
  - コア間のキャッシュの一貫性は、ページ表の Inner Shareable と、ファームウェアが立てる `CPUECTLR_EL1.SMPEN` に頼る。
  - 実機（Zero 2 W、2026-11-06）: `make sdimage-smp-auto`（`tools/smp/autorun.c`）で、4 コアの起動、分担した計算の一致、4 コアからのアトミックな加算（合計 400 万）を LED で確認した。
- tinycc（以前の C コンパイラ）は、slimcc-a64 に置き換えたので削除した（2026-11-04、ユーザーと合意済み）。git の履歴（3e70a6d まで）に残っている。
- 起動時に SD カードの `/autorun.c` があれば、シェルがコンパイルして実行する（`tools/mkimg.sh FILE.c` で入れられる）。
- アプリの終了処理: `main` から戻る、`exit()`、同期例外、Ctrl-C のどれでも `app_call`（`os/kernel/app.c`）に longjmp で戻る。終了後に、開いたままのファイルは閉じる。
  - アプリが free しなかったメモリは、アプリ用のヒープごと捨てる（上の「システムを守る」）。
  - atexit（2026-11-14 に直した。`os/kernel/app.c` の `app_atexit`・`app_exit_with_handlers`。OS API 表の atexit・exit）: アプリごとに 32 個まで覚え、main から戻ったとき・exit のときに、登録の逆の順に呼ぶ。例外・Ctrl-C・_Exit・abort では呼ばない。それまでは picolibc の atexit（カーネルと共有の一覧）で、呼ばれなかった。
  - Ctrl-C はタイマー割り込みで検出するので、割り込みを止めたまま無限ループすると止められない。
  - Ctrl-C で止めると、止めた場所（ELR）と、フレームポインタをたどった呼び出し元のアドレスを表示する（デバッグ用。アドレスは GCC 版は `nm build/kernel.elf`、slimcc 版は `build/kslim/kernel.map` で関数に直す）。
- 実機での確認（2026-11-06、UART）: Pi 5 の `/boot/firmware/config.txt` に `dtparam=uart0=on`（`/dev/ttyAMA0`）。Claude が `/dev/ttyAMA0` からシェルを操作できる（termios で 3Mbps、raw。ファームウェアのログは 115200bps）。
  - 起動（EL2 → EL1）、ヒープ 410MB（ARM 用 448MB）、CNTFRQ 19.2MHz、4 コア、SD カード、キー入力・Backspace、`hello.c`、`primes.c`（55ms）、`primes_mc.c`（1 コア 1265ms → 4 コア 415ms）、`libc_mc.c`、`crash.c`、Ctrl-C、4 コアでのセルフホストのビルド（43 秒）が動いた。
  - UART の送信（2026-11-06、`os/drivers/uart.c`）: 64KB の送信バッファに入れてすぐ戻り、PL011 の送信割り込み（GPU 割り込み 57 番、コア 0 に配送）で FIFO に移す。送るものがなくなったら TXIM を外し、新しい文字が来たら FIFO に直接書いてから戻す。バッファがいっぱいのとき・止まる前（`uart_flush`）はその場で FIFO に書く。実機で 32KB の表示が約 2.8 秒 → 4ms（表示は裏で続く）。バッファを超える分は線の速さ（115200bps、毎秒約 11KB）で待つ（200KB で 11.4 秒）。
    - ボーレート: シェルの `baud [BPS]`（新しい速さで 10 秒以内に `y` が届かなければ元に戻す）。上限は基準クロック 48MHz ÷ 16 = 3Mbps。実機（ジャンパー線、Pi 5 の ttyAMA0）で 3M・2M・1.5M・1M・921600bps のどれでも 200KB（3200 行）が化けずに届いた。200KB の表示は 3Mbps で 464ms（115200bps では 11.4 秒）。起動時の既定は 3Mbps（ユーザーと合意、2026-11-06。`UART_BAUD`。実機で起動からセルフホストのビルドまで確認）。手で使うときは `picocom -b 3000000 /dev/ttyAMA0`。
    - 注意: MMU（キャッシュ）を有効にする前は、排他アクセス（ldaxr・stxr。C11 のアトミック）を使わない。実機ではロックを取れずに止まった（QEMU では起きない）。uart.c は送信バッファを使い始めるまで（MMU の後）ロックを取らない。
  - 実機でだけ出た不具合: Ctrl-C で止めると `^C at` が延々と出た。割り込みの中の表示に UART で 10ms 以上かかり、戻る前に次のタイマー割り込みで同じ中断を繰り返していた。中断を決めた時点で要求を消す（`app_abort_from_exception`）。割り込みの中で UART に長く書くときは、この時間に注意する。
- UART での更新（2026-11-06）: SD カードを差し替えずに、Pi 5 からファイルやカーネルを送れる（`tools/uart/nos.py`。cmd・push・run・kernel・reboot・wait）。
  - シェルの `recv PATH SIZE CRC32`: 4096 バイトずつ受け取り、そのたびに「+」を返す（送る側はそれを待つので、受信バッファがあふれない）。全体をメモリに受け取り、CRC32 が合えば PATH.tmp に書いてから置き換える。`reboot` はウォッチドッグ（PM_RSTC・PM_WDOG。出典は Linux の bcm2835_wdt.c）。QEMU も対応している。
  - UART の受信は割り込み（RXIM・RTIM。FIFO が半分か途切れたとき）で 64KB の受信バッファに移す（3Mbps では 16 文字の FIFO が 53μs であふれるため）。
  - 実機（2026-11-06）: 1MB のファイルを約 120KB/s で送れた（CRC32 も一致）。カーネル（582KB）を 4.7 秒で送り、ウォッチドッグで再起動して起動した。
  - nos.py の注意: 3Mbps では入力の折り返しと出力が 1 回の読み込みでまとめて届く（QEMU の擬似端末では別々に届くので気づかない）。
  - 動かないカーネルを送ると、SD カードの差し替えでしか戻せない。
  - QEMU で試すときは `-serial pty` で起動し、`nos.py --port /dev/pts/N`（QEMU の擬似端末は約 30KB/s）。
- USB のコンソール（2026-11-06、`os/drivers/usb.c`）: Zero 2 W の「USB」端子（内側）を Pi 5 につなぐと、USB シリアル（CDC-ACM、0525:a4a7。Linux の g_serial と同じ ID）として `/dev/serial/by-id/usb-Monocs_Monocs_console-if00`（`/dev/ttyACM0`。2026-11-07 より前のカーネルは `usb-Monocs_Monocs_console-if00`。nos.py はどちらも探す）に見える。電源も Pi 5 から入る（PWR IN は外す）。
  - コントローラは DWC2（Synopsys、2.80a、`0x3F980000`、GPU 割り込み 9 番）。ペリフェラル資料に説明がないので、レジスタと手順は Linux の `drivers/usb/dwc2/`（hw.h・gadget.c・core.c、v6.12）による。電源はメールボックスの Set power state（デバイス 3）。ファームウェアはホスト側に固定しているので、GUSBCFG で機器側に固定する。
  - DMA を使わないスレーブモード。専用の TX FIFO（RX 1024 語、EP0 64 語、バルク IN 1024 語、割り込み IN 16 語）。High Speed で列挙する（バルクの最大パケット長 512）。
  - UART と同時に使える: USB で受け取った文字は UART と同じ受信バッファへ（`uart_rx_push`。Ctrl-C も同じ）、UART に出す文字は USB にも送る（`uart_add_tx_mirror`。telnet も同じ仕組み）。ホストが端末を開いている（DTR）間だけ USB に送る。USB で受け取られている間は、UART の送信バッファがいっぱいなら UART の分を捨てる（UART の速さで USB の表示が遅くならないように）。ホストが 0.5 秒受け取らなければ、USB の分を捨てて UART を待たせる元の動作に戻る。
  - 使い分け（ユーザーと合意、2026-11-06）: 操作と転送は USB を使い、UART は起動の表示を見るときだけ使う。`nos.py` は既定で USB を使い（なければ 3 秒待って UART）、kernel・reboot・boot では起動の表示を UART から読んで表示してから、USB で開き直す。`--uart` で UART を使う。
  - `recv PATH SIZE CRC32 [BLOCK]`: USB では 32KB ずつ送る（UART は 4KB）。1MB の転送は USB で 0.06 秒（約 16MB/s）。`reboot` は先に USB を切り離す（`usb_disconnect`）。起動の表示は、ホストが開く前に流れるので USB では見えない。
  - 実機: 200KB の表示が約 45ms（UART 3Mbps では 464ms）、ファイルの送信 約 1.4MB/s（下の SD カードの書き込みをまとめた後。UART は約 120KB/s）。シェル、Ctrl-C、`run`、カーネルの更新と再起動、4 コアでのセルフホストのビルド（26 秒）を USB で確認（GCC 版・slimcc 版）。
  - QEMU の raspi3b は DWC2 の機器側を持たないので、`usb: not available` で飛ばす。USB の確認は実機だけ。
  - UART の線を外しても使えるように、GPIO15（RXD0）はプルアップにし、フレーミングなどの誤りのある文字は捨てる。止まる前（`uart_flush`）は USB にも送り切る（`usb_flush`）。UART なしでは起動の表示（USB がつながる約 1 秒前まで）が見えず、USB を壊したカーネルを送ると SD カードの差し替えでしか戻せない。当面は UART もつないでおく（ユーザーと合意、2026-11-06）。
  - ホストなし（「USB」端子に何もつながず、PWR IN から給電）でも普通に起動し、UART で使える（実機で確認。`usb` は not configured）。PWR IN から給電したまま「USB」端子をホストにつながない（両方の 5V がつながるはず。未確認）。
- 端末の設定（termios。2026-11-12、`os/libc/termios.c`。ユーザーと合意、Linux に合わせる）: アプリの stdin（端末のとき）は Linux の端末の既定と同じ（ICANON・ECHO・ICRNL・ISIG・IEXTEN）。打った文字を表示し、Backspace・Ctrl-U・Ctrl-W で直せ、Enter で 1 行が '\n' 付きで届く（fgets・scanf がそのまま使える）。行の頭の Ctrl-D は EOF。`tcsetattr` で ICANON・ECHO を外すと 1 文字ずつ（VMIN・VTIME も POSIX と同じ）、ISIG を外すと Ctrl-C はキー（止めるのは Ctrl-\。`uart_set_raw`）、OPOST・ONLCR も効く（syscalls.c の screen_putc）。`tcgetattr`・`tcsetattr`・`tcflush` など、`tcgetwinsize`・`ioctl(TIOCGWINSZ)`（Linux のプログラムが画面の大きさを取る書き方）も OS API 表に。宣言は picolibc の termios.h・sys/ioctl.h（`cfmakeraw` はない）。アプリが終わると既定に戻し（`termios_reset`）、前のアプリが stdin に押し戻した文字・EOF の印も、次のアプリを始めるときに消す（run.c）。curses は UART から直接読むので関係ない。シェルも自分の行の編集で読む。
  - それまで（〜2026-11-12）は、アプリの getchar がキーをそのまま返し（Enter は '\r'、エコーなし）、fgets・scanf が Enter で終わらなかった。samples/tui.c は termios（ICANON・ECHO を外す）と ioctl(TIOCGWINSZ) を使う形に直した。
- 端末の画面の大きさ（2026-11-06、`os/kernel/term.c`、アプリには `monocs_term_size`）: USB シリアルや UART には画面の大きさを伝える仕組み（telnet の NAWS、ssh の window-change）がないので、Linux の resize と同じく、カーソルを右下の端に動かして位置を問い合わせる（ESC [ 6 n → ESC [ 行 ; 列 R）。答えがなければ 0.3 秒で 24 行 80 桁。UART と USB の両方の端末が答えることがあるので、2 つ目の答えも 50ms の間は読み捨てる。例: `samples/tui.c`（枠を描き、矢印キーで動かす）。アプリの getchar は、2026-11-12 から termios の既定（Linux と同じく 1 行ずつ・エコーあり。下の「端末の設定」）。それまではキーごとにすぐ返り、エコーしなかった。
  - 経緯（ユーザーと合意、2026-11-06）: USB イーサネット（CDC-ECM）を足して ping → lwIP（ユーザーと合意して third_party に追加）→ telnet サーバー（NAWS で画面の大きさ）。すべて完了。
- USB イーサネット（2026-11-06、`os/drivers/usb.c`、`os/net/net.c`）: USB の機器を、USB シリアル（ACM、インターフェース 0・1）と USB イーサネット（CDC-ECM、2・3）の複合機器にした（IAD で組にする。ID は Linux の g_multi と同じ 1d6b:0104。by-id の名前は変わらない）。Pi 5 からは `usb0`（cdc_ether）に見える。
  - アドレス: Monocs 192.168.7.2（MAC 02:6E:65:77:6F:02）、Pi 5 の usb0 192.168.7.1/24（MAC は ECM の iMACAddress で 02:6E:65:77:6F:01）。Pi 5 は NetworkManager の接続 `monocs-usb`（固定アドレス。ユーザーと合意して追加、2026-11-06。作り直すのは `sh tools/pi5/setup-usbnet.sh`、消すのは `sh tools/pi5/setup-usbnet.sh remove`）。
  - エンドポイント: ECM のデータは EP4（IN・OUT、バルク）、通知は EP5（割り込み）。データインターフェースの代替設定 1 で使い始め、NETWORK_CONNECTION を通知する（cdc_ether はこれでリンクを上げる）。フレームは短いパケットで終わり、最大パケット長の倍数なら長さ 0 のパケットを足す。TX FIFO は ECM の送信 512 語・通知 16 語を足した。
  - 構成の記述子が 154 バイトになったので、EP0 の IN は 64 バイトずつ送る（`ep0_reply`。wLength より短く 64 の倍数なら長さ 0 のパケットで終える）。
  - 受け取ったフレームは割り込みの中で待ち行列に入れ、ロックを外してから `net_input` に渡す。送信の待ち行列は 32 フレーム（8 では TCP の送信であふれた）。
- lwIP（2026-11-06、`third_party/lwip`。サブモジュール、STABLE-2_2_1_RELEASE。無改造）: `os/net/net.c` がつなぐ。設定は `os/net/port/lwipopts.h`・`arch/cc.h`。
  - NO_SYS=1・raw API（コールバック）。lwIP を呼ぶのは `net_lock`（割り込みを止めて取る、コア間・同じコアなら重ねて取れるスピンロック）の中だけ: USB の割り込みの受信（`net_input`）、タイマー割り込み（`net_timer`、10ms ごとに `sys_check_timeouts`）、送信。
  - 使うもの: IPv4、ARP、ICMP、UDP、TCP、IP の断片の組み立て・分割。DHCP・DNS・IPv6 は使わない。メモリは静的（MEM_SIZE 512KB、PBUF_POOL 128）。TCP の窓・送信バッファは 16 × MSS。
  - 確認用に TCP のエコー（ポート 7）。実機: ping（4000 バイトの断片も）、flood ping、20MB のエコーが一致して片方向 約 15.5MB/s（GCC 版・slimcc 版）。slimcc でコンパイルした lwIP も動く。
  - セルフホストのビルドにも入れた（`KIND_LWIP`。22 ファイル）。
- telnet サーバー（2026-11-06、ポート 23。最初はカーネルの `os/net/telnet.c`、同じ日にサービス `services/telnetd.c` に移した）: Pi 5 から `telnet 192.168.7.2` でシェルを使える。USB シリアルと同じく「もう 1 つのコンソール」（出力は UART・USB・telnet のすべてに出し、どこから打った文字もシェルに届く）。同時につなげるのは 1 つ（2 つ目には断りの 1 行を返して閉じる）。
  - つないだときに WILL ECHO・WILL SGA・DO SGA・DO NAWS を送り、1 文字ずつ送る形にして画面の大きさを知らせてもらう（RFC 854・857・858・1073）。NAWS の大きさは `monocs_term_size` が返す（ウィンドウの大きさを変えるたびに届く）。IAC の 2 重化、Enter の CR LF・CR NUL は CR だけにする、IP・BRK は Ctrl-C にする。
  - コンソールとは「コンソールの写し」（`os/kernel/contap.c`。OS API の `monocs_console_open`・`close`・`read`・`input`・`set_size`）でつながる。コンソールの出力を 64KB のバッファに写し、サービスが service_poll で読み出して送る（カーネルからサービスの関数は呼ばない）。いっぱいなら書く側が待つ（USB の割り込みの処理を自分で呼びながら。0.5 秒進まなければ捨てる。サービスのコア自身は待たない）。サービスが例外で止まったら写しも閉じる。
  - 実機: コマンド、NAWS（120×40）、Ctrl-C、200KB の表示が 0.14 秒（GCC 版。slimcc 版 0.21 秒）、つなぎ直し、2 つ目の拒否、Linux の telnet コマンドで確認。一致を確認（GCC 版・slimcc 版・2 世代目）。QEMU には USB の機器側がないので、ネットワークの確認は実機だけ。
- HTTP サーバー（2026-11-06、ポート 80。最初はカーネルの `os/net/httpd.c`、同じ日にサービス `services/httpd.c` に移した）: `curl http://192.168.7.2/path`（GET。ディレクトリは一覧の HTML でブラウザで見られる）、`curl -T FILE http://192.168.7.2/path`（PUT。親のディレクトリも作り、PATH.tmp に書いてから置き換える）、`curl -X DELETE`。HEAD も。USB からは認証なし、Wi-Fi からはダイジェスト認証（2026-11-13。内蔵 Wi-Fi の段階 4）。
  - サービスなので、アプリの実行中も動く（実機で、無限ループのアプリの実行中に 1MB の GET が 0.18 秒）。同時に扱う接続は 1 つ。10 秒進まなければ閉じる。curl の `Expect: 100-continue` に答える。
  - 実機: GET 5MB 0.56 秒、PUT 5MB 1.2 秒。内容の一致を確認（GCC 版・slimcc 版）。
  - `nos.py` の push・pushdir・kernel・run は、HTTP サーバーに届けば PUT で送り、HEAD で大きさを確かめる（届かなければ、または `--serial` ならシェルの recv）。HTTP のサービスを止めているときや、古いカーネルでは、自動でシリアルになる。
- アプリから使える TCP の API（2026-11-06、`os/net/tcp_api.c`、`monocs.h` の `monocs_tcp_listen`・`accept`・`connect`・`send`・`recv`・`close`）: 待つ形（timeout_ms: 0 = 待たない、負 = いつまでも）。例: `samples/tcp_echo.c`（サーバー）、`samples/http_get.c`（クライアント）。HTTP サーバーも同じ API を使う。
  - 接続は 16 個まで、接続ごとに 64KB の受信バッファ（静的）。lwIP のコールバックは受信バッファに書くだけで、TCP の窓はアプリが読んだ分だけ開けるので、バッファはあふれない。コールバックの中では malloc しない。
  - アプリが終わると（Ctrl-C でも）、アプリが開いた接続は閉じる（`tcp_api_close_app`、`os/slimcc/run.c`）。待ち受けは SOF_REUSEADDR（`SO_REUSE`）にした（TIME_WAIT の接続が残っていても同じポートで待ち受けられる。これがないと、Ctrl-C の後に同じポートで待ち受けられなかった）。
  - 実機: ping の往復 約 0.1ms。1400 バイト、フレームがちょうど 512・1024 バイト（長さ 0 のパケット）、5000 回の flood ping を 2 回で取りこぼしなし。その間も USB シリアルのシェルは動く（GCC 版・slimcc 版）。
- SD カードの読み書きをまとめる（2026-11-06、`os/fs/diskio.c`）: イメージ（`tools/mkimg.sh`、64MB の FAT32）はクラスタが 1 セクタなので、FatFs は 1 セクタずつ読み書きする。実機の SD カードは 1 セクタの書き込みが 1 回約 2.2ms かかる。
  - 書き込み（ライトバック。ユーザーと合意、2026-11-06）: 書き込みは「まだ書いていないセクタ」の表（最大 4MB、ハッシュで引く）にためる。同じセクタ（FAT・ディレクトリ）の書き直しは表の中でまとまる。書き出すのは、最初の書き込みから 1 秒たったとき（ファイル操作のついでと、シェルが入力を待つ間の `vfs_sync(false)`）、表がいっぱいのとき、`sync`・`reboot`・アプリの終了（`vfs_sync(true)`）のとき。番号順に並べ、続くものは 1 回の複数ブロックの書き込み（最大 1MB）にする。CTRL_SYNC（f_close など）ではすぐには書かない。**電源が切れると、書き出す前の分（最大約 1 秒）は失われる。** FatFs を呼ぶのは VFS のロックの中だけなので、ロックは取らない。
    - 読み出しでは、まだ書いていないセクタを表から返す（読んだものの上に重ねる）。先読みのバッファにも、読み込んだ時点で重ねておく。重ねるのを読み出しのたびだけにしたら、書き出した後に先読みに残った古い中身を返し、FatFs が古い FAT を読んで使用中のクラスタを別のファイルに割り当てた（実機の SD カードで kernel8.img と .o がクラスタを共有。下の修復の手順で直した）。
    - 効果（実機）: 小さなファイル 300 個 5348ms → 25ms、4MB のファイル 1573 → 760ms、300 個の削除 3216 → 340ms、`nos.py pushdir` の /src（1,588 ファイル）49 → 10 秒、1MB の送信（HTTP）0.7 → 0.1 秒、カーネルのビルド 12 → 9 秒。
    - 効かなかったもの（見送り）: 複数ブロックの書き込みの前の ACMD23（SET_WR_BLK_ERASE_COUNT）、まとめる量を 128KB → 1MB（どちらも、ばらつき（同じ設定で 1550〜2150ms）より小さい）。
  - ないとわかっているパスのキャッシュ（`os/fs/vfs.c` の neg_*。2026-11-06）: コンパイラはヘッダーを -I のディレクトリの順に探すので、ないファイルを何度も開こうとし、FatFs はそのたびにディレクトリを端から読んでいた（picolibc のビルドで SD カードから 760MB）。開けなかった・stat できなかったパスを覚え（8192 個まで）、そのパスにファイルやディレクトリを作ったら（O_CREAT・mkdir・rename の行き先）忘れる。picolibc のビルド（実機）193 → 49 秒（起動直後）。`mem` に数が出る。
    - 同じ起動のまま繰り返すと遅くなっていた（63 → 104 秒）。原因は 2 つで、どちらも直した（下の「malloc の置き換え」と、VFS のファイルのキャッシュのハッシュ表）。今は 42 → 36 → 36 秒。
  - 診断と修復（HTTP サーバー `services/httpd.c`）: `curl -o fat.img http://192.168.7.2/.sdcard/fat.img` で最初の区画をそのまま取り出し、Pi 5 で `fsck.vfat -n fat.img` をかけられる。直したイメージは `curl -T fat.img http://192.168.7.2/.sdcard/fat.img` で書き戻せる（区画と同じ大きさのときだけ。メモリ上のキャッシュを捨て、何も書かずに再起動する。元に戻せない操作なので、取り出したイメージを残しておくこと）。`mem` に SD カードの読み書きの失敗の数も出る。
  - 1 セクタの読み出しは、先読みから返したものもセクタのキャッシュに入れる（2026-11-21。read_media）。クラスタが 4KB のカード（2026-11-20 に区画を広げてから）では、ディレクトリの 8 セクタが続いて並ぶので先読みの道を通り、キャッシュに入らなかった。C ライブラリのビルドが、956 個の .o があるフォルダ（libc.a.p）にファイルを作るたびにそれを読み直して、SD カードから 147MB 読み、61〜84 秒かかった。直して読み出し 4〜17MB、38〜41 秒（できた libc.a・kernel8.img は Linux 上のビルドと一致）。
  - 先読み: 前の読み出しの直後のセクタを読むときだけ、8 セクタから始めて倍々に（最大 64KB）まとめて読む。飛び飛びの読み出し（FAT など）は先読みしない。最初から 64KB を先読みすると、小さなファイルやディレクトリで読みすぎて遅くなった（22MB → 152MB）。書き込みが重なったら捨てる。
  - セクタのキャッシュ: 1 セクタずつの飛び飛びの読み出し（FAT・ディレクトリ）を覚えておく（直接マップ、4096 セクタ = 2MB）。書き込みでは中身を新しくする。順でない読み出し（キャッシュから返すものも）で先読みの量を 8 セクタに戻す（戻さないと、ファイルごとに 64KB 先読みして 79MB 読んだ）。
  - 内訳（実機、`mem`。キャッシュの前）: 読み出し 10.7 秒のうち、コマンドの後にデータの準備を待つ時間が 10.2 秒（コマンド 1 回あたり約 0.3ms のカードの応答の待ち × 24,000 回）、PIO は 0.6 秒、コマンドを出す時間は 0.2 秒。線の速さ（High Speed の 50MHz）や DMA より、コマンドの数を減らすほうが効く。High Speed は線の上の時間しか縮まないので 1 割未満の見込み（見送り。2026-11-21 に入れた: 大きなファイルを読むときは線の上の時間が大部分だった。下の「SD カードを High Speed（50MHz）に」）。
  - 実機: 1MB の送信 4.7 秒 → 0.6 秒、カーネルの更新 3.2 秒 → 0.5 秒、4 コアでのセルフホストのビルド 26 秒 → 13 秒（書き込み・先読み）→ 5 秒（キャッシュ。slimcc 版は 6 秒。読み出しのコマンド 24,472 → 2,636 回、2.2 秒）。残りは書き込みの完了待ち（約 2.3 秒）。`mem` で SD カードの読み書きの回数・量・時間と内訳を見られる。
  - 確認: 書いて読み直す（いろいろな大きさの fwrite）、再起動の後の CRC32、セルフホストの一致（GCC 版・slimcc 版・2 世代目）と、その後の fsck.vfat。
- malloc の置き換え（2026-11-06、`os/libc/malloc.c`）: picolibc の malloc（空き領域を 1 本のリストで先頭からたどる）を TLSF（Two-Level Segregated Fit。Masmano ら、ECRTS 2004）に置き換えた。空き領域を大きさで 2 段（2 の累乗 × 32 区分）に分けたリストに置き、ビットマップで O(1) で見つける。境界タグで隣の空きとつなげる。`ldtoa.c` と同じく、カーネルで定義して picolibc より先にリンクする（malloc・free・calloc・realloc・memalign・aligned_alloc・posix_memalign・valloc・pvalloc・malloc_usable_size・mallinfo・malloc_stats・mallopt。picolibc の malloc のメンバーを引き込まないように全部）。ロックは picolibc の malloc と同じ `__lock___libc_recursive_mutex`。
  - 実機: malloc・free 20 万回 4548ms → 177ms。picolibc のビルドの後も 178ms（前は 17,700ms）。
  - Linux で確かめる: `tools/picolibc/test_malloc.c`（偽の sbrk で 300 万回の確保・解放・realloc・memalign・calloc。中身・境界・重なりと、全部解放したら空きが 1 つにつながること）。
  - 不具合（直した）: 領域を広げるとき、求められた大きさちょうどしか広げず、区分を切り上げて探す find_free が見つけられずに NULL を返した（1MB を少し超える確保。slimcc のゾーンの塊で起き、実機が止まった）。広げる量を切り上げた大きさにし、見つからなければ同じ区分のリストも順に探す。テストに 1〜8MB の確保を足した。
- VFS のファイルのキャッシュは、鍵からハッシュ表で引く（2026-11-06）。前はリストを先頭から比べていて、ないパスの stat（負のキャッシュより先にファイルのキャッシュを見る）のたびに 2,559 ファイルをたどっていた。古いものから捨てる順のリストは双方向にした。
- サービス（2026-11-06、`os/kernel/svc.c`。ユーザーと合意）: 裏で動き続けるプログラム。カーネルの外に置き（「仕組みはカーネル、サービスはプログラム」）、コア 3 で動かす。
  - 書き方: `main` の代わりに `int service_start(int argc, char **argv)`（0 で成功。省略可）、`int service_poll(void)`（待たずに戻る。仕事をしたら 0 以外）、`void service_stop(void)`（省略可）。アプリと同じ OS API を使える。例: `services/httpd.c`。
  - シェル: `svc`（一覧）、`svc start FILE.c [ARGS]`、`svc stop 名前`（ファイル名から .c を除いたもの）。起動時に `/etc/services.txt`（2026-11-13 までは /services.txt。1 行に「FILE.c 引数」。# で始まる行は飛ばす）のものを始める（`svc_autostart`、/autorun.c より前）。
  - コア 3 で、すべてのサービスの service_poll を順に呼び続ける（協調型）。どれも仕事をしなかったら wfe で眠り、コア 0 の割り込みのたびの sev で起きる（`os/drivers/irq.c`）。サービスが 1 つでもある間はコア 3 を取っておき（`smp_reserve`、`smp_run_reserved`）、アプリには 3 コアに見える（`monocs_cores`）。アプリの終わり（Ctrl-C を含む）でも止めない。サービスがなくなれば 4 コアに戻る。
  - 持ち主: ファイル・ディレクトリ・TCP の接続に持ち主（`svc_owner`。0 = アプリ・カーネル、1〜 = サービス）を付け、アプリの終わりでは 0 のものだけ閉じる（`vfs_close_owner`、`tcp_api_close_owner`）。サービスを止めたら、そのサービスのものを閉じる。
  - サービスの中で例外が起きたら、コア 3 は待機に戻る。シェルが入力を待つ間（`svc_check`）に気づき、すべてのサービスを止めて後始末する（QEMU で確認）。
  - OS API に足したもの（HTTP サーバーの区画の取り出し・書き戻しのため）: `monocs_disk_read`・`monocs_disk_write`（SD カードを直接。VFS のロックの中で）、`monocs_disk_sync`、`monocs_disk_discard`、`monocs_reboot`。
  - 実機: サービスがあると、セルフホストのビルドは 3 コア（カーネル 6〜7 → 9 秒、C ライブラリ 36 → 38 秒）。止めたいときは `svc stop httpd`。
  - 今のサービスは `httpd`（HTTP）と `telnetd`（telnet）。カーネルに残るネットワークは、USB イーサネット、lwIP、TCP の API、確認用の TCP エコー（ポート 7）。
  - 注意（2026-11-06）: Makefile が FatFs・lwIP・組み込みの slimcc の .o の .d を読んでいなかったので、`lwipopts.h` を変えても GCC 版の lwIP が作り直されなかった（SO_REUSE が lwIP の本体に効かず、telnet のセッションをつないだまま telnetd を止めると、ポート 23 で待ち受け直せなかった）。.d を読むようにした。設定のヘッダーを変えたら、生成物が新しくなったか確かめる。
- 端末のアプリ（curses）を動かす（2026-11-07。ユーザーと合意。きっかけは、ユーザーの端末のエディタ mi（`~/dev/tools/mini-editor`、C11 + ncursesw、37 ファイル）をソースのまま動かすこと。mi のソースは書き換えない）:
  - `run DIR`: ディレクトリの中の .c をすべてコンパイルし（名前の順。使えるコアで分担）、1 つのプログラムにつなぐ。`DIR/compile_flags.txt`（clangd と同じ形式。1 行に 1 つ）の `-std=`・`-D`・`-U`・`-I` を使う。`run` にも `-D`・`-U`・`-I` を足した。`svc start DIR` も同じ。
  - サブフォルダと共有のライブラリ（2026-11-12。ユーザーと合意。`os/slimcc/run.c` の add_directory・add_libraries）: `run DIR` はサブフォルダの中の .c も探す（「.」で始まるものは飛ばす。フォルダごとに名前の順、深さ 8 まで）。compile_flags.txt か run の `-l名前` で、`/lib/名前/`（サブフォルダも）の .c を一緒にコンパイルしてつなぎ、`-I/lib/名前` も足す（ライブラリはアプリのオプションでコンパイルする。別のファイルとしてつなぐので、static の名前はぶつからない）。`run -l名前 FILE.c` も同じ。ないライブラリはエラー。
    - 除くもの（2026-11-12）: アプリ・ライブラリのフォルダの `.runignore`（.gitignore と同じ考え方。1 行に 1 つ、`*` などは fnmatch。/ を含まない名前はどのフォルダでも、/ を含むものはフォルダからの位置で比べる。終わりの / はフォルダだけ。先頭の / はそのフォルダから）。テスト用の main を持つファイル（mi の test_buffer.c など）を入れないため。compile_flags.txt に書かないのは、clangd がその行をすべてオプションとして読むため。
    - ライブラリは、そのフォルダの compile_flags.txt（なければ既定の gnu23）でコンパイルする（アプリのオプションは使わない。自分のフォルダも -I に入る）。そこに `-l別の名前` があれば、それもつなぎ、そのヘッダーも探せる（同じライブラリは 1 回だけ。輪になった指定でも止まる。16 個まで）。同じライブラリを使うアプリどうしで、コンパイルのキャッシュも共有できる（ファイルごとのオプションでコンパイルする。CompileJob の flags）。コンパイルのキャッシュはファイルごとなので、ライブラリを直すとそのファイルだけコンパイルし直す。SD カードのイメージに空の /lib を作る（tools/mkimg.sh）。置くのは `nos.py pushdir DIR /lib/名前`。

  - 複数のオブジェクトのリンク（`slimcc_link_many`、`cc/memlink.c`）: GNU ld と同じく、STB_LOCAL はそのオブジェクトの中だけ、グローバルは全体で 1 つ（強い定義が 2 つならエラー、弱い定義は強い定義に負ける。-fno-common なので `int x;` を 2 つのファイルに書くとエラー。GCC 10 以降と同じ）。`slimcc-jit a.c b.c -- 引数` で試せる（`make cc-test` の 12 に、GCC でリンクしたものとの比較）。
  - curses（`os/curses/curses.c`、`os/api/include/curses.h`。`ncurses.h`・`ncursesw/curses.h` は読み替え）: ncurses 互換の小さなもの（自作。third_party は使わない）。定数（キー・属性・マウス）の値は ncurses 6 と同じ（Pi 5 の /usr/include/curses.h で確認）。送るシーケンスと読むキーは Pi 5 の terminfo（`infocmp -1 xterm-256color`）と同じで、terminfo は使わない。stdscr だけ（newwin などはない）。画面を 2 つ（アプリが描いたもの・端末に出ているもの）持ち、wrefresh で違うところだけを送る。UTF-8・全角（右半分の上書きで左半分も描き直す）・色（256 色、既定の色 -1）・罫線（ACS を Unicode の罫線素片で）・マウス（SGR 形式 ?1006h と X10 形式）・define_key・ungetch・ungetmouse・KEY_RESIZE（telnet の NAWS で大きさが変わったとき）。
    - ncurses と違うところ: クリック・ダブルクリックは作らない（いつも mouseinterval(0) と同じ）。スクロール（scrollok）はしない。画面の大きさの変更は telnet のときだけわかる（USB シリアル・UART には知らせる仕組みがない）。
    - raw() の間は Ctrl-C をキーとしてアプリに渡し、Ctrl-\ でアプリを止める（`uart_set_raw`。Linux の SIGQUIT と同じキー）。アプリが終わったら戻す。
    - endwin を呼ばずに終わったとき（return・exit・例外・Ctrl-\）は、代わりの画面から戻し、マウス・括弧付き貼り付け・modifyOtherKeys も切る（`curses_app_exit`、例外の表示の前に `curses_emergency_restore`）。
  - POSIX（`os/libc/posix.c`）: `access`・`realpath`（正規化して、なければ NULL。前の syscalls.c の realpath は正規化せず、ないパスでも返していた）・`clock_gettime`（MONOTONIC・REALTIME）・`nanosleep`（眠る）。プロセスはないので、`getpid` はアプリを動かすたびに変わる番号、`kill` は今のアプリへの信号 0 だけ成功、`popen` はいつも NULL（2026-11-14 から、kill は今のアプリに送れ、popen は一時ファイルで動く。下の「シグナルと popen」）。`getline`・`getdelim`・`setlocale` も OS API 表に。環境変数 `HOME=/`（アプリが `~/.local` などに設定を置ける）。
    - アプリには `_POSIX_MONOTONIC_CLOCK` を定義して渡す（`run.c` の `app_defines`。picolibc の time.h はこれがないと `CLOCK_MONOTONIC` を隠す。picolibc・カーネルのビルドには渡さない）。
  - `wcwidth`（`os/libc/wcwidth.c`。picolibc より先にリンク）: picolibc はマルチバイト対応なしでビルドしているので、全角も 1 を返していた。Unicode の表（`os/libc/wcwidth_table.h`、`tools/gen_wcwidth.py` で Python の unicodedata から作る。Unicode 15.1）で、glibc と同じ分け方（結合文字・書式文字 0、East Asian Width の W・F は 2）。glibc 2.41（Unicode 16.0）との違いは、16.0 で全角になった記号などと、ごく一部の書式文字だけ。
  - まとめてコンパイルするときの使い回し（2026-11-07。ユーザーと合意: 結果のキャッシュ（SD カードに保存）ではなく、コンパイラを速くする）: `slimcc_cache_files(on)`（`cc/libslimcc.h`）の間、読んだファイルの中身・字句解析の結果（トークンの種類・位置・長さ・行番号の記録。`cc/tokenize.c` の TokRec）・ファイルがあるか（インクルードの検索）を覚え、どのコアからも使う（ロックで守る。最後のコアが off にしたら捨てる）。トークンは前処理が書き換え・使い回すので、記録から毎回新しく作る（文字列・文字の定数は読み直す）。`run DIR` が使う。SD カードには何も残さない。
    - 内訳（実機、mi、1 コアあたり、温まった状態）: 前処理 約 285 → 240ms（ファイルの読み込み・字句解析がなくなった分）、構文解析・中間表現 約 310ms、アセンブル 約 60ms、リンク 28ms。起動直後（VFS のキャッシュが冷えている）は、SD カードからの読み込みが 1 コアあたり約 200ms 加わる。
    - 確認: `slimcc-jit -cache -odir DIR`（`-odir` は .o を書く）で、使い回しあり・なしの .o が一致（mi 37、カーネル 33、slimcc 18、lwIP 29 ファイル）。
    - 行番号を付ける処理（`add_line_numbers`）も軽くした（トークンの間の改行を数えるだけ）。読み込みは、大きさのわかるファイルは一度に読む。
  - コンパイルのキャッシュ（2026-11-07。ユーザーと合意。`os/slimcc/cache.c`）: `run`・`svc`（ファイルとディレクトリ）でコンパイルした結果を SD カードの `/.cache/slimcc/<カーネルの識別>/<キー>` に保存し、次からはソースとヘッダーの中身が変わっていなければ読み込む（電源を切っても残る）。`cc`（打ち込んだソース）とセルフホストのビルド（slimcc コマンド）は使わない。
    - 置き場所の識別は、中のコンパイラの識別（2026-11-16 から。`build/gen/compiler_id.h` の SLIMCC_COMPILER_ID。tools/compiler_id.sh が cc/ の .c・.h・.inc、cc/platform/monocs.c、os/slimcc/cache.c・cache.h・run.c の名前と中身の SHA-256 から作る。セルフホストの /src にも mksrc.sh が写す）。コンパイラが変われば別の場所になり、古いものは最初に使うときに消す。それまではカーネル全体（.text・.rodata）の FNV-1a で、シェルやドライバを直しただけでも全部コンパイルし直していた。コンパイルの結果にはカーネルの番地が入らない（名前のまま持ち、つなぐのは動かすたび）ので、コンパイラが同じなら使える。実機で、標準のカーネルから GCC 版に入れ替えても、コンパイル 0・キャッシュから 140。**コンパイラの出力に効くファイルを cc/ の外に足したら、tools/compiler_id.sh に渡す一覧（Makefile の COMPILER_ID_SRCS）にも足す**（漏れると、古い出力を使い続ける）。/boot/kernel8.sym の識別（cache_kernel_id）は今までどおりカーネル全体。キーはソースの絶対パスとオプション（`compile_flags.txt` と `run` の -D などを含む）。
    - 使えるかは、コンパイルで読んだファイル（`slimcc_deps`。ソース・ヘッダー・#embed）の絶対パス・大きさ・中身のハッシュがすべて一致し、コンパイルで調べてなかったパス（`slimcc_absent`。ヘッダーの検索で先に探した場所、__has_include）が今もなく、ファイル全体のハッシュ（書きかけ・壊れたものを除く）が合うとき。時刻は使わない（RTC がなく、起動のたびに時刻が戻る）。ハッシュは 1 回の run の間、ファイルごとに覚える。（2026-11-21 から日時も使う: 下の「キャッシュの確かめに FAT の日時」）
    - `__DATE__`・`__TIME__`・`__TIMESTAMP__` を使ったもの（`slimcc_cacheable` が false）は保存しない（毎回コンパイルする）。
    - 書くときは PATH.tmp に書いてから置き換える。そのため VFS の `rename` を POSIX と同じにした（行き先がファイルなら置き換える。FatFs の f_rename は行き先があると失敗していた。mi の保存の置き換えも同じ）。
    - コンパイルした結果（AsmObj）の保存と読み込みは `slimcc_obj_save`・`slimcc_obj_load`（`cc/assembler.c` の asm_obj_save・asm_obj_load。独自の形式、壊れた入力でも止まらない）。`make cc-test` の 12 で、test/ を書いて読み戻したものでも動かす（`slimcc-jit -roundtrip`）。
    - シェルの `cache`（数・大きさ・起動してから使えた回数とコンパイルした回数）、`cache clear`（消す）。`cache clear NAME|PATH...`（2026-11-21。ユーザーの「vim のキャッシュだけ削除したい」から）: そのプログラムのソース（ファイル、フォルダならその下）のものだけ消す。場所の名前はハッシュなので、中身の頭のソースの絶対パスで決める（cache.c の cache_clear_program）。/usr/lib の共有のライブラリの分は残す。名前は PATH で探す（シェルと同じ）、/ を含むか .c で終わるものはパス。どちらの clear も、メモリに覚えたつないだ結果（run.c の img_cache）も捨てる（前は全部の clear でも残り、小さなプログラムはコンパイルせずに動いた）。
    - 実機（mi）: 1 回目 1.2 秒、2 回目から 0.2〜0.4 秒、電源を入れ直した直後 0.5 秒（ヘッダーを SD カードから読んで確かめる）。ソースを 1 つ直すと、そのファイルだけコンパイルし直して 0.3 秒。mi の分で約 770KB。
  - 日時（2026-11-07、`os/kernel/clock.c`）: Zero 2 W には RTC がないので、日時は設定されるまで「起動を 2026-01-01 00:00:00 UTC」として数える（前と同じ）。設定はシェルの `date -s @秒`・`date -s 年-月-日 時:分[:秒]`（地方時）・`date -z +0900`（タイムゾーン。環境変数 TZ にして picolibc の localtime が使う。+0900 は JST）。nos.py もつなぐたびに Pi 5 の日時とタイムゾーンを送る（`set_time`。前に空白を付けるので履歴に残らない）。ふだんは下の SNTP のサービスが起動の直後に合わせる。
    - FatFs の日時は `FF_FS_NORTC 0` と `get_fattime`（`os/fs/vfs.c`）。日時が設定されるまでは 2026-01-01 00:00 の固定（前の FF_FS_NORTC 1 と同じ。QEMU の検証の結果は変わらない）。`stat` の `st_mtime` も FAT の日時を返す（地方時を TZ で UNIX 時間にする。ファイルのキャッシュにも持つ。開いた fd の fstat は、キャッシュにないものは 0）。
    - SNTP（2026-11-07。ユーザーと合意して Pi 5 に chrony を入れた）: サービス `services/sntp.c`（RFC 4330）が、起動してネットワークがつながったら Pi 5 の chrony（192.168.7.1:123）に問い合わせて合わせ、その後は 1 時間ごとに合わせ直す。合うまでは 5 秒おき。設定は `/services.txt` の 1 行（`/services/sntp.c 192.168.7.1 -i 3600 -z +0900`。サーバー・間隔・タイムゾーン）。止めるのは `svc stop sntp`。合わせたことは表示しない（シェルの行に割り込むため。`date` で見る）。
    - Pi 5 の準備は `tools/pi5/setup-chrony.sh`（chrony を入れ、`/etc/chrony/conf.d/monocs.conf`（2026-11-21 より前に作った Pi 5 のものは seedos.conf） に `allow 192.168.7.0/24` と `local stratum 10`。元の chrony.conf は書き換えない。`remove` で外す）。chrony は systemd-timesyncd の代わりになり、Pi 5 自身の時刻合わせも続ける。
    - アプリ・サービスの UDP の API（`os/net/udp_api.c`、`monocs_udp_open`・`sendto`・`recvfrom`・`close`。TCP の API と同じ作り。ソケット 8 個、それぞれ 8 個のデータグラムの枠。持ち主の終わりで閉じる）と、POSIX の `clock_settime`・`settimeofday`、`monocs_set_timezone`・`monocs_clock_known` を足した。
  - `ls`（2026-11-07、`os/shell/shell.c`）: GNU の ls に近づけた。名前の順（大文字・小文字を区別しない）に、端末の幅に合わせて上から下へ列に並べる（全角の名前も幅で数える。幅は `term_cols`: telnet は NAWS、ほかは最後に問い合わせた値）。ディレクトリは青（`ESC [1;34m`。GNU の --color と同じ）。`-a`（「.」で始まるもの、. と ..）、`-l`（種類・大きさ・日時（半年以内は「月 日 時:分」、ほかは「月 日 年」。GNU と同じ）・total）、`-h`、`-1`。複数の PATH（ファイルが先、ディレクトリは見出し付き）、エラーの文も GNU と同じ形。
  - シェルの基本コマンド・リダイレクト・パイプ（2026-11-07、`os/shell/shell.c`。ユーザーと合意）:
    - コマンド: `cp [-r]`、`rm [-rf]`（GNU と同じく、ディレクトリは -r のときだけ）、`rmdir`、`mkdir [-p]`、`touch`（FatFs の f_utime。`FF_USE_CHMOD 1`。POSIX の `utime` も OS API に）、`echo [-n]`、`df [-h]`（`statvfs`）、`clear`、`history [-c]`、`which`。`cat` はファイルがなければ stdin。エラーは stderr（GNU と同じ形）。
    - シェルに組み込むのは、シェル自身の状態を変えるもの（cd、pwd、export、env、history、which、help、write）、プログラムを動かす仕組み（run、cc、svc、slimcc、slimld）、カーネルの中を見る・変えるもの（mem、clock、usb、cache、sync、reboot、baud、recv、date、uptime）だけ（Linux の bash と同じ分け方。ユーザーと合意、2026-11-07）。ls・cat・cp・mv・rm・rmdir・mkdir・touch・echo・df・clear は `/bin` のアプリ（リポジトリの bin/）。実機で ls は 1 回目 0.15 秒（コンパイル）、2 回目から 0.03 秒（組み込みのときは 0.02 秒）。`write` は行の編集で入力を読むので組み込みのまま（2026-11-07 の時点では、アプリの getchar は打った文字を表示しなかったため。今は termios で読めるが、組み込みのまま）。アプリには `monocs_term_cols`（覚えている桁数）、`localtime_r`・`gmtime_r` を足した。
    - 行の書き方: 引用符（'...' はそのまま、"..." は \" と \\ だけ解く、外の \ は次の 1 文字）、`>`・`>>`・`<`、`2>`・`2>>`・`2>&1`、`/dev/null`（書いたものを捨て、読むとすぐ終わり）、`|`、`&&`・`||`・`;`（終了状態で決める。書き方の誤りがあれば何も動かさない。bash と同じ）。
    - リダイレクト（`os/libc/syscalls.c`）: stdin・stdout・stderr を別々の FILE にした（前は 1 つを共有）。シェルが動かす間だけ stdout・stdin をファイルに向ける（`console_redirect_*`。stderr はいつも画面、サービスのコア 3 の出力は向けない）。fd 0・1 の read・write（VFS）も同じ向け先。`isatty` もリダイレクトを返す。向け先のファイルは `vfs_keep_open` でアプリの終わりに閉じない（閉じられて、まとめて書いていた出力が消えていた）。前のプログラムが stdin を終わりまで読んだ印（EOF）は `clearerr` で消す。
    - パイプは一時ファイル（`/.pipe0`、`/.pipe1`…。MS-DOS と同じ方式。Monocs は同時に 1 つのプログラムしか動かさない）。終わったら消す。
    - `ls` は stdout がファイルのとき、色を付けず 1 行に 1 つ（GNU と同じ）。
  - Monocs に付いてくるアプリ（2026-11-07、リポジトリの `bin/`。`make sdimage` が SD カードの /bin に入れる）: `less`（curses。折り返し、/・? の検索と反転、パイプから読める）、`grep`（-ivnclrFE。小さな正規表現: . [] [^] ^ $ * \+ \?、-E では + ?。グループと | はない。画面に出すときは合ったところを色付け）、`head`・`tail`（-n N）、`wc`（-lwc。数の幅も GNU と同じ）。ファイルがなければ stdin。grep と wc は Linux の grep（ugrep）・wc と出力を比べた。
  - アプリを名前だけで動かす（2026-11-07、`os/shell/shell.c` の find_app）: 組み込みのコマンドにない名前は、環境変数 PATH（既定 `/apps:/bin`）のディレクトリから順に `ディレクトリ/名前`（ディレクトリのアプリ）か `ディレクトリ/名前.c` を探して run する（引数もそのまま。組み込みのコマンドがいちばん先）。`/bin` は Monocs に付いてくるアプリ（リポジトリの bin/。更新で入れ替わる）、`/apps` は自分で作った・入れたアプリ（mi など。Monocs の更新では触らない）。`/apps` が先なので、同じ名前なら自分のものが使われる（Linux の /usr/local/bin と同じ。ユーザーと合意）。Tab の補完と `help` にも出す。例: `mi ファイル名`。
    - `export NAME=VALUE`・`env`（環境変数。picolibc の setenv）。シェルは起動時に `/.profile` があれば 1 行ずつ動かす（`export PATH=...` を書けば残る。# は飛ばす）。TZ も setenv で設定する（os/kernel/clock.c）。
  - mi の動かし方: Makefile の SRCS の .c とヘッダーを `/apps/mi` に置き（`test_buffer.c` は main を持つので入れない）、`compile_flags.txt` に `-std=c11`・`-D_DEFAULT_SOURCE`・`-D_XOPEN_SOURCE=700`。`mi ファイル`（`run /apps/mi ファイル` と同じ）。起動は実機で、キャッシュが使えれば 0.2〜0.4 秒、37 ファイルをコンパイルするときは 0.9〜1.2 秒（3 コア。1 コアでは 2.9 秒）。設定・履歴は `/.local/share/mi`。
  - 確認: QEMU と実機（GCC 版・slimcc 版。USB シリアルと telnet）で、表示・日本語の入力・保存・マウスのクリック・終了・Ctrl-\・telnet でのウィンドウの大きさの変更。テストは端末を再現するスクリプト（ESC [ 6 n に答え、出力から画面を作る）で行った。人の目での確認はまだ。
- シェルの行の編集（2026-11-07。ユーザーと合意: シェルは curses にせず、readline・linenoise と同じく 1 行だけを書き直す。`os/shell/lineedit.c`）: ← →・Home・End・Ctrl-A/E/B/F・Alt-B/F（単語）、↑ ↓・Ctrl-P/N（履歴）、Backspace・Delete・Ctrl-D・Ctrl-W・Alt-Backspace・Alt-D・Ctrl-U・Ctrl-K、Ctrl-L、Tab（行の最初の語はコマンド名、ほかはファイル名。大文字・小文字を区別しない。候補が複数なら共通の部分まで、進まなければ一覧）。
  - 履歴は `/.shell_history`（200 行。電源を切っても残る）。空白で始まる行は残さない（bash の ignorespace と同じ）。nos.py はコマンドの前に空白を付けて送る。
  - 行の終わりに文字を足すだけなら、今までと同じく打った文字をそのまま返す（nos.py などの自動の操作の表示は変わらない）。書き直すときだけ端末の桁数を使う（telnet は NAWS、ほかは初めて書き直すときに `term_size` で問い合わせる）。端末の幅を越えて折り返した行は、linenoise の複数行の方式と同じ計算で書き直す（右端までちょうど打った直後は、端末の折り返しが保留されていることに注意）。日本語（全角）も表示幅で数える。
  - `term_size` は、答えを待つ間に届いたキーを受信バッファの先頭に戻す（`uart_rx_unread`。前は読み捨てていて、速く打つと文字が消えた）。
  - 確認: QEMU（端末を再現するスクリプト。ESC [ 6 n に答える）と実機（USB シリアル、telnet、nos.py、mi）。
- 省電力（2026-11-07、`cpu_idle`・`uart_wait_input`。`os/drivers/timer.c`・`uart.c`）: コア 0 が待つ間（シェルの入力、getchar、端末の大きさの問い合わせ、recv、`timer_delay_us` の 1 ティックより長い分、TCP の API の待ち）は wfi で次の割り込みまで眠る。割り込み（タイマー 100Hz、UART・USB の受信）はすべてコア 0 に届くので、遅れはせいぜい 1 ティック（telnet の入力はコア 3 から入るので最大 10ms）。入力を待つときは割り込みを止めて調べてから眠る（調べた後に届いた文字の割り込みでも起きる。wfi は PSTATE.I で止めていても割り込みで起きる）。コア 1〜3 では何もしない（割り込みが来ないので）。`uptime` にコア 0 が眠っていた割合を出す。
  - 実機: 待っている間のコア 0 の idle は 99%。温度 44.0℃ → 41.3℃（10 分後）。ネットワーク（エコー 5MB 18MB/s、アプリの TCP、HTTP、telnet）、Ctrl-C、セルフホストのビルドは変わらず動く。
  - クロック（見送り）: メールボックスの SET_CLOCK_RATE（ARM 600MHz。skip turbo の 0・1 とも）と SET_TURBO を試したが、ファームウェアは設定した値を返すだけで、実測（GET_CLOCK_RATE_MEASURED、サイクルカウンタ、primes の時間）は 1000MHz、コアの電圧（GET_VOLTAGE）は 1.256V のまま変わらなかった。理由は資料で確かめられない。wfi で眠っている間はクロックが止まるので、クロックを下げる効果はもともと小さい。
- CPU のクロック（2026-11-06、シェルの `clock`）: 実機（Zero 2 W）の最大は ARM 1000MHz、CORE（VPU）400MHz。メールボックスの実測値（GET_CLOCK_RATE_MEASURED）とサイクルカウンタ（PMCCNTR_EL0 を 100ms 数えた値）が一致する。
  - **訂正（2026-11-13）**: 前は「何も設定しなくても 1000MHz のまま」と書いていたが誤り。ファームウェアは起動から 60 秒だけ最大で動かし（config.txt の initial_turbo。2024 年 11 月のファームウェアから既定が 60）、その後は OS の cpufreq が頼まなければ最小（ARM 600MHz、CORE 250MHz。GET_CLOCK_RATE の「current」はこの値）に下げる。11-06 の確認は 60 秒の内だったと思われる。600MHz では primes 46 → 60ms、primes_mc 497 → 828ms、セルフホストのビルドも遅い。カーネルを送った直後（再起動から 60 秒以内）に測ると速く出るので、速さの比べものが揺れていた（Wi-Fi・SD カードのドライバのせいに見えた。ユーザーの「Wi-Fi を入れてからコンパイルが遅くなっていないか」から調べてわかった）。
  - 直し: config.txt に `force_turbo=1`（資料: 「Arm のコアが忙しくなくても最大にする」。保証の印は over_voltage_* と組み合わせたときだけ。電圧は変えない）。実機で、起動から 2 分たっても 1000MHz、温度はビルドの後で約 50℃、throttled 0。待つ間は wfi で眠る。
  - 速さを測るときは、`clock` の measured を見る（1000MHz であること）。
  - 同じときに確かめたこと（2026-11-13）: Wi-Fi のある・なし、SD カードのドライバ SDHOST・EMMC で、ビルドの速さは変わらない（同じ条件で交互に測った。カーネル 5.5〜5.9 秒、揺れは SD カードの書き込みの時間 1.9〜3.6 秒による）。Wi-Fi の受信の読み出しは 1 回 13〜16 マイクロ秒（10ms ごと。測るコードは整理で消した）。
  - 速さ（force_turbo の後、起動から 2 分以上、3 コア）: カーネル 5.5〜5.7 秒、C ライブラリ 30.6〜34.0 秒。
- 性能カウンタ（2026-11-12。ユーザーと合意。`os/kernel/pmu.c`、シェルの `perf`）: Cortex-A53 の PMU（イベントカウンタ 6 個とサイクルカウンタ）で、キャッシュのミスなどを数える。`perf [-e EV,...] [-c] コマンド...`（既定は inst・l1d・l1d-miss・l1i-miss・l2・l2-miss。`-l` で一覧と、この CPU が数えるか（PMCEID0/1_EL0）、`-c` でコアごと）。表示は stderr。
  - アプリを動かすコマンドなら、アプリの実行中だけを数える（コンパイルの分は `pmu_app_begin` で捨てる）。ほかは コマンド全体。アプリが動かしたコア 1〜3 の仕事も足す（`pmu_job_begin`・`end`。止められたときも `abort_return` で足す）。コア 3 のサービスは数えない。OS とアプリは同じ EL1 なので、割り込みなどカーネルの分も入る。wfi で眠っている間はサイクルも数えない。
  - イベントカウンタは 32 ビットなので、あふれたら PMU の割り込みで上位を数える（ARM ローカル周辺の PMU の配送 0x40000010・要因 9。出典は Linux の drivers/irqchip/irq-bcm2836.c）。サイクルカウンタは 64 ビット（PMCR_EL0.LC）。実機で 1 コア 60 億命令のループが、計算どおりの数になった。
  - 内蔵アセンブラに PMU のシステムレジスタ（PMCEID0/1、PMSELR、PMXEVTYPER・PMXEVCNTR、PMEVCNTR<n>・PMEVTYPER<n>、PMOVSCLR、PMINTENSET/CLR、PMCCFILTR、MDCR_EL2 など）を足した（GNU as と一致。`cc/test-a64/asm/sysreg.asm`）。
  - QEMU の PMU はサイクルしか数えない（イベントは「not counted by this CPU」）。確かめるのは実機。
  - 実機の例: primes.c（1MB の配列）は L2 のアクセスの 36% がミス。セルフホストのビルド（3 コア）は 0.59 命令/サイクル、1000 命令あたり L1D のミス 3.8・L1I のミス 2.0・L2 のミス 4.5。TLB のミス（itlb-miss・dtlb-miss）は数百回でほとんどない（カーネルのコードを 4KB ページにした影響は見えない。ただし A53 でこのイベントが何を数えるかは資料で確かめていない）。
  - `perf record [-e EV] [-p N] [-n TOP] [-g] [-o FILE] コマンド...`（2026-11-12、`os/shell/perf.c`）: EV（既定 l2-miss、`-e cycles` で時間）が N 回（既定 1 万）起きるごとにカウンタ 0 をあふれさせ、割り込みで止まっていた番地（ELR）を記録して（最大 65,536 個）、関数ごとに多い順に並べる。`-g` は x30 とフレームポインタ（このコアのスタックの中のときだけ読む）をたどった呼び出し元の並びを、関数ごとに多い 3 つ。
    - アプリの関数名は、アプリが終わる直後（コードと記号表を捨てる前）に `slimcc_symbol_at`（`cc/memlink.c`。番地の順の表を作って二分探索。`.L` のラベルは除く）で付ける。カーネルの関数名は `/kernel8.sym`（SD カードの /boot/kernel8.sym。`tools/perf/mksym.py` が make のときに作る。GCC 版は nm、slimcc 版は slimld の -Map。1 行目にカーネルの識別（`cache_kernel_id`）があり、違えば番地のまま）。`nos.py kernel` が隣の .sym も送る。`make sdcard` も入れる。slimld の -Map に static の関数も書くようにした（イメージは変わらない）。
    - VFS のロックを分けた（2026-11-12。ユーザーと合意。`os/fs/vfs.c` の `vfs_fc_lock`）: ファイルのキャッシュ・ないパスの覚え・ファイルの表の空き・カレントディレクトリの写しは `vfs_fc_lock` で守り、キャッシュに当たる open・stat、キャッシュから開いたファイルの read・lseek・fstat・close、getcwd は `vfs_lock`（FatFs と SD カード）を取らない。ロックの順は vfs_lock → vfs_fc_lock だけ。vfs_lock を待つ間にほかのコアがキャッシュに入れたかもしれないので、取った後でもう一度キャッシュを調べる。ファイルの表の空きは、FatFs で開くときも先に取っておく（`slot_alloc_locked`）。
      - 効果: ヘッダーの open・stat・getcwd の待ちはほぼなくなったが、セルフホストのビルドの時間は変わらなかった（実機 3 回ずつ、前後とも約 5.0 秒、ばらつき ±0.3 秒）。C ライブラリのビルドは 34〜37 → 32 秒。残るロックの待ちの 9 割は、.o を書き込みで開く open が、ほかのコアの SD カードへの書き出し（読み出しの途中の `wb_flush`）を待つもの。
    - 実機の全部のビルド（2026-11-12、Unux に名前を変えた後のソース、サービスありの 3 コア。`perf -e inst run ...`）: C ライブラリ 34.6 秒（電源を入れ直した直後）・35.2 秒（2 回目）、カーネル 6.2・7.6 秒。0.57〜0.67 命令/サイクル。どちらも Linux 上のビルドと一致。11-06 の記録（38 秒・9 秒）より少し速いが、同じ条件でも 1 秒以上ばらつく。
    - わかったこと（実機、セルフホストのビルド、3 コア。VFS のロックを分ける前）: 時間のいちばんは VFS のロックの待ち（`take` 約 2 割。open・stat・fstat が、ほかのコアが VFS のロックを持ったまま SD カードを読み書きする間待つ）、次が SD カードの転送（`transfer`・`timer_now_us`・`wait_interrupt`）。L2 のミスは字句解析（`tokenize`・`new_token`）が 4 割弱（初めて読むソースと、新しく作るトークン。トークンはゾーンから順に取るので、並べ方ではあまり減らない）。
  - アプリを止めるとき、ほかのコアが中断を後回しにしている間（ロック・アプリから呼んだコンパイル）は 60 秒まで待つ（`smp_abort_all`。前は 5 秒で、待たずに進むとアプリ用のヒープを捨てた後もそのコアが動き続けうる）。
- 小さな負担の計測（2026-11-06、`perf` は Pi 5 に入れた）:
  - タイマー割り込みでの q レジスタ 32 本の退避と復元は 1 回約 121ns（実機）で、100Hz なら全体の約 0.001%。割り込み全体の負担も実機で 0.004%（割り込みを止めた場合との比較）。直さない。
  - MMU: RAM は 2MB ブロックの Normal・ライトバック・Inner Shareable、ページ表の参照もキャッシュ可、周辺機器は Device-nGnRnE。Device-nGnRE（Linux と同じ）にすると MMIO の書き込みが早く終わるが、SD は読み出し中心で書き込みの間に待ちを入れているので効果は小さい。GPU 用のメモリ（上端の 64MB）も Normal でマップしているが、読み書きはしていない。
- 実機での確認状況（2026-11-04、UART なし、ACT LED の点滅で確認）: 起動、EL2→EL1、MMU とキャッシュ、メールボックス（バスアドレスの別名 `0xC0000000`）、タイマー割り込み、SD カードのマウントと書き込み（`test.txt` を PC で確認）、tinycc によるコンパイルと実行（`make sdimage-blink` の LED パターンで確認）は動作した。slimcc-a64 に切り替えた後も、`make sdimage-blink` で同じ LED パターンになった（2026-11-04）。`CPUECTLR_EL1.SMPEN` は自前で立てていないが、問題は出ていない。UART の入出力は未確認。
- slimcc-a64（`cc/`）の状況:
  - 元にした slimcc は `cc/UPSTREAM.md` に記録（2388c39）。`third_party/slimcc` は無改造で残し、`cc/` を直接書き換える。
  - プラットフォームは `cc/platform/linux-aarch64.c`（`cc/platform.c` はそのシンボリックリンク）。
  - フロントエンドは AArch64 向け（予定義マクロ、`char` は符号なし、`wchar_t` は `unsigned int`、`va_list` は AAPCS64 の構造体、long double は binary128 の値、`_BitInt` は幅 65 以上で 16 バイト単位・境界 16、名前のないビットフィールドも構造体の境界に効く）。値は Pi 5 の GCC 14 と照合している（`cc/test-a64/`）。
  - `__int128` は当面 `_BitInt(128)` として扱う（aarch64 の glibc・カーネルヘッダーが `__uint128_t` を使うため）。`__SIZEOF_INT128__` は定義していない。
  - `-fsyntax-only` を追加した（構文解析と型検査まで）。
  - 中間表現（SSA）を使うコード生成（2026-11-06、`cc/ir.inc`。`codegen.c` の末尾で取り込む）: 既定で使う（`-fno-slimcc-ir` で今までのコード生成だけ）。ユーザーと合意して、今までのコード生成の改良ではなく作り直し（B 案）を選んだ。
    - 関数ごとに AST から基本ブロックと SSA の命令を作る（Braun らの方法）→ 定数の畳み込みと簡約、不要な拡張の除去、GVN（支配木、Cooper らの方法）、DCE → アドレッシングモードへの畳み込み → SSA を抜ける（重要な辺を分け、phi を並列コピーとして並べる）→ 穴のある生存区間の線形走査（Wimmer and Franz。コピーの両側をヒントでつなぐ）→ 出力。
    - 値の表し方: 整数とポインタは 64 ビットのレジスタに、型に合わせて拡張した形で持つ。符号つきの int の加減乗除はあふれない（未定義動作。GCC と同じ前提）として拡張し直さない。符号なしの 32 ビットの演算は w レジスタの命令。浮動小数点は別の組（v16〜v29、呼び出しをまたぐものは v8〜v15）。
    - レジスタ: x9〜x15・x19〜x28（呼び出しをまたぐ値は x19〜x28）。x0〜x7 は引数・戻り値・インラインアセンブリのオペランド・TLS のアドレス計算に、x16・x17・x8・v30・v31 は作業用に使い、値には割り当てない。
    - 扱えない関数（long double、可変長引数の関数、スタックで渡す引数、計算型の goto、asm goto、x0〜x7 以外を壊すインラインアセンブリなど）は、今までのコード生成（`gen_fn_code`）が出力する。理由は環境変数 `SLIMCC_IR_WHY` で表示できる。カーネル全体（slimcc・picolibc の一部を含む）で 52（2026-11-06。ほとんどが long double の引数・戻り値と setjmp）。
      - 中間表現で扱うようにしたもの（2026-11-06）: goto のラベル（関数本体・`__label__`）、`nullptr_t`、アトミック操作（`IR_CAS`・`IR_XCHG`。ldaxr・stlxr のループで x0〜x5 を使う。整数・ポインタだけで、float・double は今までのコード生成）、`_Atomic` の整数・ポインタの読み出し（ldar。アドレッシングモードに畳み込まず、冗長な読み出しの除去はここで知っている中身を捨てる。`_Atomic` の変数は SSA にしない）。カーネルのロックの関数などが入り、73 → 52。
      - long double を中間表現で扱うこと（残りの 52 のうち 35）は見送り（ユーザーと合意、2026-11-07。q レジスタの割り当てと演算の呼び出しが要る大きな作業で、呼ばれる回数が少なく速さはほとんど変わらないため）。
    - インラインアセンブリを含む関数は、今までと同じく x19・x20 を `[x29 - 16]` に退避する（フレームの形を前提にしたアセンブリがある。`test/asm.c`）。
    - `sqrt`・`fabs` は命令にする（`sqrt` は負の数のときだけ errno のために関数を呼ぶ。Monocs の picolibc は `want-math-errno=false` なので errno は変わらない）。浮動小数点の演算は共通部分式の除去をしない（`fesetround` で丸めモードを変えるコードがあるため）。
    - 効果: 自己コンパイル 569 → 488ms、.text 604 → 428KB、nbody 1328 → 560ms（GCC -O2 は 306ms）、slimcc 版のカーネル 1,063,800 → 875,656 バイト。実機の primes_mc（1 コア）は 1265 → 1338ms（64 ビットの sdiv・mul・msub が A53 では遅い）→ 下の 32 ビットの扱いで 1153ms。
    - 確認: `make cc-test`（GCC との呼び出し規約の互換性、2・3 世代目の一致を含む）、`make ld-test`、カーネルとセルフホストの一致（GCC 版・slimcc 版、2 世代目）、実機。
    - インライン展開（2026-11-06）: 中間表現を作るときに、小さな関数（最適化した後の命令が 12 個まで、`always_inline` は 150 個まで。深さ 4 まで）の本体を呼び出し元に展開する。展開できるのは、先に定義され中間表現にできた関数で、インラインアセンブリ・構造体の引数や戻り値のないもの。`return` は結果の変数への代入と出口への分岐になる。
      - slimcc は宣言ごとに AST の領域（`ast_arena`）を捨てるので、展開できる関数の AST は残す（`ast_arena_keep`、`cc/alloc.c`）。
      - 展開した関数は、static の局所変数（文字列リテラルなど）を持つときだけ出力する対象に加える（持たなければ、ほかから呼ばれない限り出力しない）。
      - しきい値 30 では .text が約 2 割増え、ベンチマークの速さは変わらなかったので 12 にした（.text 429 → 452KB）。
      - 同時にコンパイルの時間を削った: 呼び出しをまたぐかの判定を二分探索に、生存区間を作るときは立っているビットだけをたどる、中間表現の確保を塊からの切り出しに（自己コンパイル 722 → 543ms）。
      - 確認: `make cc-test`、`make ld-test`、カーネルとセルフホストの一致（GCC 版・slimcc 版、2 世代目）、実機（hello、primes_mc、libc_mc、セルフホストのビルド 26 秒）。slimcc 版のカーネルは 889,656 バイト。
    - 32 ビットの値（2026-11-06、`ir_mark_low32`）: 下位 32 ビットだけが使われる値（使う側が 32 ビット以下のストア、w レジスタの演算・比較、sxtw、cbz、phi）に印を付け、符号つきの int の演算を w レジスタの命令にして符号拡張を省く。int の割り算・余りはいつも w レジスタの命令（A53 では 64 ビットの割り算が遅い）。比較は、両方が符号拡張した 32 ビットなら w レジスタで、ゼロ拡張なら等しいかと符号なしの比較のときだけ。
      - 実機: primes_mc（1 コア）1338 → 1153ms、primes 55 → 47ms。Pi 5 ではほとんど変わらない。
      - 直した不具合: `ir_fits` が phi に付けた「調べ中」の印を残し、同じ問い合わせの中で 2 回目に聞かれると「収まる」と答えていた（gen2 の内蔵アセンブラが誤った）。印は調べ終わったら消し、仮定は同じ幅・符号の問い合わせにだけ使う。
    - 冗長な読み出しの除去（2026-11-06、`ir_load_elim`）: 同じ場所をもう一度読むとき・書いた直後に読むときは、その値を使う。基本ブロックごとに「わかっている中身」の表を持ち、合流では全前任で一致するものだけ残す。重なりの判断は、同じ土台・同じ添字で範囲が重ならない、または同じ構造体の重ならない別のメンバー（読み書きに構造体の型とメンバーの位置の目印 `mem_struct`・`mem_off` を付ける。共用体には付けない。C の型の規則による）。呼び出し・memcpy・インラインアセンブリで全部消す。
    - sqrt の確認を省く（`ir_mark_sqrt`）: 引数が負にならない（x*x、負にならないものの和・積、fabs、sqrt、0 以上の定数）なら、errno のための確認と呼び出しをしない（NaN でも errno は変わらない）。呼び出しとしても扱わない。
    - 積和演算（2026-11-06。ユーザーと合意）: 1 つの式の中の a*b+c、c+a*b、c-a*b、a*b-c（複合代入も）を fmadd・fmsub・fnmsub にする。GCC と同じく、既定は GNU の規格（gnu17・gnu23。`run` の既定）のときだけで、ISO の規格（-std=c17 など。picolibc・カーネルは ISO）ではしない。`-ffp-contract=on`・`off` で変えられる。丸めが 1 回なので、最後の桁が変わることがある。3 つ目の引数を読むため、v29 も作業用にした（割り当ては 21 個）。
    - 効果: nbody（Pi 5）559 → 530（sqrt）→ 443ms（積和演算。GCC -O2 は 301ms。GCC のベクトル化・積和演算なしは 444ms）。実機（Cortex-A53）では積和演算の効果はほとんどない（積和演算の遅延が掛け算と足し算の合計と同じくらいのため。300k ステップで 555 → 564ms、ばらつきの範囲）。冗長な読み出しの除去は nbody では効かない（L1 に当たるので）。残りの差は、番地の計算の強度の削減、ldp・stp、命令の並べ方、ベクトル化。
    - 確認: picolibc は変わる（読み出しの除去などで）が、`tools/picolibc/libctest.c` の出力（2133 行）は GCC 版の picolibc と一致。
    - 教訓: 出力の命令は「入力をすべて読んでから結果を書く」こと（区間を半開にして、最後に使う値と結果に同じレジスタを割り当てるため。`sqrt` の展開で破って誤った）。Cortex-A53 の速さは Pi 5 や QEMU と違うので、実機でも測る。
  - コード生成（`cc/codegen.c`）は AArch64 用に書き直した（x86-64 版の構成を引き継いだスタックマシン方式。最適化はほとんどない）。レジスタの使い方とフレームの形は、ファイル冒頭のコメントにある。
    - 改善（2026-11-05）: 局所変数を `[x29, #ofs]` で直接読み書きする（代入・複合代入・後置の増減も）。二項演算で右辺が定数・局所変数・局所変数のアドレスなら一時スタックを使わず、12 ビットの即値は命令に埋め込む。交換できる演算は左右を入れ替える。2 のべき乗の掛け算はシフトにする。浮動小数点の右辺が局所変数なら直接読む。
    - レジスタ割り当て（2026-11-05）: アドレスの要らないスカラーの局所変数（引数を含む）を、使われる回数（ループの中ほど重く数える）の多い順に x21〜x28、d8〜d15 に置く（関数の入口と出口で退避・復元）。関数ごとに出力を捨てる「下見」のコード生成をして、アドレスやメモリが必要な変数（`gen_addr` など。`pin_var`）と使われる回数（`note_var_use`）を調べてから、本番を生成する。`setjmp` などを呼ぶ関数では割り当てない。レジスタの変数は演算・比較・`sxtw` でそのまま使い、結果を使わない `x++`・`x += 定数` は 1 命令にする。
      - 効果: primes 1824ms → 1191ms（gcc -O2 は 1069ms）、nbody 1856ms → 1516ms、strhash 524ms → 488ms。自己コンパイルは 593ms → 584ms（コード生成が 2 回になる分と相殺）。slimcc 版のカーネルの上でのセルフホストのビルドは 72 秒 → 79 秒（QEMU。下見の分だけ遅くなった）。
      - 下見では出力の文字列を作らない（`emit_out`。引数に副作用のある呼び出しがあるので、引数は評価する）。下見の負担はコンパイル時間の約 7%。
    - アドレッシングモード（2026-11-05）: 構造体のメンバーは `[base, #ofs]`（入れ子のオフセットをまとめる）、配列の要素は `[base, index, lsl #k]`（レジスタの int の添字は `[base, wN, sxtw #k]`）で読む。要素の大きさが 2 のべき乗でなければ `madd` で 1 命令にする。局所変数の配列やレジスタのポインタを土台にする書き込み・複合代入も直接行う（`addr_nocode`）。
      - 効果（ここまでの合計）: .text 801KB → 523KB、自己コンパイル 735ms → 535ms、primes 1898ms → 1160ms（gcc -O2 は 1065ms）、nbody 2025ms → 1329ms（301ms）、strhash 553ms → 465ms（408ms）。slimcc 版のカーネル 1.48MB → 993KB、その上でのセルフホストのビルド 約 96 秒 → 70 秒（QEMU）。
      - 注意: `make cc-test` で見つからず、カーネルのビルド（`make kernel-slimcc`）で見つかった不具合があった。コード生成を変えたら、カーネルのビルドとセルフホストの一致も確かめる。
      - 注意: 文字列をポインタで比べない（GCC は同じ文字列リテラルをまとめるが slimcc はまとめないので、2 世代目と 3 世代目が食い違った）。
    - 効果（`cc/test-a64/bench.sh`。2 世代目の .text、自己コンパイルの時間、ベンチマーク）: .text 801KB → 563KB、自己コンパイル 735ms → 593ms、nbody 2025ms → 1856ms（gcc -O2 は 301ms）、primes 1898ms → 1824ms（1087ms）、strhash 553ms → 524ms（412ms）。slimcc 版のカーネルは 1.48MB → 1.06MB、その上でのセルフホストのビルドは約 96 秒 → 72 秒（QEMU）。
  - 呼び出し規約は AAPCS64（Linux）。HFA、16 バイトを超える構造体の参照渡し、x8 による戻り値の領域、可変長引数（va_list は AAPCS64 の構造体）に対応。GCC との互換性は `test-a64/abi_gen.py` で両方向に確かめている。
  - long double（binary128）は q レジスタで扱い、演算と変換は libgcc の関数（`__addtf3` など）を呼ぶ。Monocs で使うときは、これらの関数をカーネル側に用意する（OS API 表には登録済み）。
  - アトミック操作は LSE 命令を使わず、`ldaxr`/`stlxr` のループ、`ldar`、`dmb ish` で行う。
  - インラインアセンブリのオペランドは、制約 r、w、m（Q）、即値、Z、g、X、数字、修飾子 w、x、s、d、q、h、b、c、a、l に対応。`asm goto` の出力は、ラベルへ飛ぶ経路でも書き戻す。未対応: ビットフィールドへの出力、条件フラグの出力（`=@cc…`）。
  - PIE では、その翻訳単位で定義していない変数を GOT 経由で参照する（AArch64 の GNU ld は、PIE での adrp によるコピー再配置を解決できないため）。
  - `test/` は 106 本すべてが、Pi 5 と `qemu-aarch64 -cpu cortex-a53` で通る。x86-64 を前提にしていた 12 本は、`#if` で AArch64 の期待値を加えた（x86-64 の期待値も残してある）。`asm.c`、`inline_asm.c` には AArch64 版のテストを書いた。
  - 内蔵アセンブラ（`cc/assembler.c`）: codegen.c が出力する GNU as 形式のテキストとインラインアセンブリを、機械語・シンボル・リロケーションの表（`AsmObj`）にする。方式はユーザーと合意済み（2026-11-04）: 命令ごとの API に書き換えず、テキストを読む内蔵アセンブラにした（インラインアセンブリにもアセンブラが必要なため）。
    - GNU as と同じ結果にしている（同じセクション内のローカルなシンボルへの PC 相対分岐はアセンブル時に解決、ローカルなシンボルへのリロケーションはセクション＋オフセット、TLS セクションのシンボルは STT_TLS など）。
    - 確認: `slimcc -dump-internal-as file.s` と `test-a64/objdump.py`（GNU as の .o を同じ形式にする）の出力を `test-a64/asm_compare.sh` で比べる。網羅テストは `test-a64/asm_gen.py`（ビットマスク即値の全パターンなど）。
    - 未対応: `-g` の行番号情報（.loc は読み飛ばす）、`.cfi_*`（読み飛ばす）、マッピングシンボル（$x、$d）。
  - ELF の .o の書き出し（`asm_write_elf`）。内蔵アセンブラが既定で、`-fno-integrated-as` か `-g` のときだけ外部の as を使う。リンクは今も外部の ld。
  - セルフホスト: 外部の as を使わずに、2 世代目と 3 世代目の実行ファイルが一致する（PATH に失敗するだけの as を置いて確認、2026-11-04）。
  - libslimcc（メモリ上でのコンパイルとリンク。API は `cc/libslimcc.h`）: `slimcc_compile`（ソース → `AsmObj`）、`slimcc_link`（メモリ上に配置し、リロケーションを解決。範囲外の分岐はスタブ、定義していない変数は GOT 経由）、`slimcc_symbol`。
    - `-DSLIMCC_LIBRARY` でビルドする（`cc/Makefile` の `*.lib.o`、カーネルは `build/slimcc/`）。エラーは `cleanup_exit` から longjmp で戻り、NULL を返す。
    - slimcc は解放しないメモリが多いので、コンパイル中の確保をゾーン（`cc/zone.c`）から行い、終わったらまとめて捨てる。各ファイルの静的な状態は `*_reset` で初期化し直す。ゾーンの外（open_memstream など libc 内部の確保）に置くものは個別に解放する。
    - Monocs 用のプラットフォームは `cc/platform/monocs.c`（picolibc 用に GCC の型マクロを定義）。
    - アプリが定義する TLS の変数は扱わない（`test/tls.c`、`tls2.c` は対象外）。カーネルの TLS の変数（errno）への参照は扱う（マルチコアの段階 3）。
    - 確認: `make cc-test` の 12（Linux の `slimcc-jit` で test/ をメモリ上で実行、100 回コンパイルしてもメモリが増えない）。
  - セルフホストの段階 1（2026-11-04）: カーネル全体（OS、FatFs、組み込みの libslimcc、`.S`）を slimcc-a64 でコンパイルできる（`make kernel-slimcc`）。QEMU で起動し、組み込みの slimcc でアプリをコンパイル・実行できる。リンクは GNU ld。最適化がないので kernel8.img は約 1MB（GCC 版は約 470KB）。
    - 内蔵アセンブラに `.macro`/`.endm`、`ldr Rt, =式`（リテラルプール。`.ltorg`）、SYS 命令の別名（`tlbi`、`ic`、`dc`、`at`）を足した。GNU as と同じ結果（`cc/test-a64/asm/*.asm`、`make cc-test` の 10）。
    - 段階 2（2026-11-04）: 自作のリンカ slimld（`cc/ld.c`、API は `cc/ld.h`、Linux 用のコマンドは `cc/ld_main.c`）。ELF の .o と .a を読み、`os/boot/linker.ld` と同じ配置で kernel8.img を直接書く（配置は ld.c の表。linker.ld を変えたら ld.c も合わせる）。
      - GNU ld と同じ結果にしている（.a のメンバーの選び方と順、入力セクションの並びと境界、孤立セクション .got・.got.plt・.fini_array_onexit の位置、未定義の弱いシンボルの扱い）。違うのは 2 点: GOT の項目の並び（slimld は最初に参照された順）、SHF_MERGE のセクションの統合をしない（イメージが数百バイト大きくなる）。
      - 確認: `make ld-test`。GNU ld には SHF_MERGE を消した入力（`cc/test-a64/nomerge.py`）を渡し、GOT の項目の位置以外がバイト単位で一致することを `cc/test-a64/ld_compare.py` で確かめる。slimld でリンクしたカーネルは QEMU で起動し、test/ が通る。
      - 未対応: COMMON シンボル、SHF_MERGE の統合、ELF の出力（gdb 用の kernel.elf は作らない）。TLS は段階 3（マルチコア）で対応。
    - 段階 3・4 は完了（上の「セルフホスト」）。
    - 計画（ユーザーと合意、2026-11-04）: 2. 自前のリンカ（ELF の .o と .a から kernel8.img を直接作る）、3. Monocs 上でのビルド（.o を SD カードに書き、リンクする。ビルドの手順は C のスクリプトを `run` で動かす）、4. Monocs 上で作った kernel8.img が Linux 上のクロスビルドと一致することの確認。picolibc と libgcc は当面 GCC でビルドした .a を使う。
  - TODO: 名前のないビットフィールドの扱いは GCC の動作に合わせた。AAPCS64 の該当条項を確認して `cc/parse.c` の struct_decl に出典を書く。
- 不明な点や資料で確認できない仕様は、推測で実装せず、その旨を報告する。
