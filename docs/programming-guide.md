# Monocs でプログラムを書くための手引き

Monocs（Raspberry Pi Zero 2 W の自作 OS）の上で動くプログラムを書く人・AI のための手引きです。
ふつうの Linux と違うところ（クセ・注意点）と、その理由・対処を、これだけ読めばわかるようにまとめています。
内容は 2026-11-12 の時点から書き足してきたもの（2026-11-21 に、全部ソースのままの形と今のフォルダの構成に合わせて見直した）で、実機と QEMU で動きを確かめたものです。OS の仕様を変えたら、この手引きも直します。仕組みの詳しい経緯は、リポジトリの `CLAUDE.md` にあります。

---

## 0. まず押さえる 15 のこと

1. **ソースのまま動かす**。C のソースを置いて `run hello.c`、または名前だけ（PATH の `/root/apps`・`/usr/bin` にある `名前.c` か `名前/` のフォルダ）で、その場でコンパイルして動かす。2 回目からはコンパイルのキャッシュ（`/var/cache/slimcc`）で速い。付いてくるコマンドもサービスもソースのまま。Linux と同じく `make`・`cc -o` で実行ファイルを作って `./hello` で動かすこともできる（2026-11-17〜）。コンパイラは Monocs の上で動く（Pi 5 は要らない）。実行ファイルは Monocs 独自の形で、動かすたびに OS の関数とつなぐ（カーネルを入れ替えても作り直さなくてよい）。
2. **同時に動くプログラムは 1 つだけ**（シングルタスク）。プロセス、fork、exec、パイプの同時実行、シグナルはない。ただし `system("コマンド")` で別のプログラムを動かして、終わるのを待つことはできる（2026-11-14。→ 6.4）。
3. **OS もアプリも同じ特権（EL1）・同じアドレス空間**で動く。アプリはハードウェアも OS のメモリも触れる。バグで OS を壊しうるが、よくある事故（NULL、カーネルのコードへの書き込み、スタックのあふれ）は例外で止まってシェルに戻る。
4. **使える C ライブラリの関数は「OS API の表」（`os/api/api.def`）にあるものだけ**。ヘッダー（picolibc）に宣言があっても、表になければ実行のときに `link error: undefined symbol 'fork'` のようになる。`fork`・`exec`、`aligned_alloc` などはない（→ 5 章）。
5. **宣言していない関数を呼ぶとエラー**（警告ではない。既定の規格が C23 のため）。必要なヘッダーを必ず `#include` する（例: `isatty` は `<unistd.h>`）。
6. **キー入力は Linux の端末と同じ**（2026-11-12 から）。既定では打った文字が表示され、Backspace で直せ、Enter で 1 行が `'\n'` 付きで届く（`fgets`・`scanf` がそのまま使える）。キーを 1 つずつ受け取りたいときは、Linux と同じく `tcsetattr` で `ICANON`・`ECHO` を外す（→ 8.2）。画面の大きさは `ioctl(1, TIOCGWINSZ, &ws)`。
7. **Ctrl-C でアプリは止まり、シェルに戻る**（curses の `raw()` の間と、termios で `ISIG` を外した間は、Ctrl-C はキーとして届き、Ctrl-\ で止まる。`signal(SIGINT, 関数)` を登録すると、Ctrl-C で止まらずにその関数が呼ばれる（→ 6.5）。Ctrl-\ はいつも止める）。Ctrl-C で止まったときは、`atexit` で登録した関数は呼ばれない。
8. **アプリが終わると、OS が後始末をする**: 開いたファイル・TCP/UDP の接続は閉じ、malloc したメモリ（free しなかった分も）はまとめて捨てる。static の変数は、次に動かすときは初期値から始まる。
9. **static・グローバルの変数は `-fno-common`**。`int x;` を 2 つのファイルに書くと、リンクでエラーになる（GCC 10 以降と同じ）。
10. **アプリは `_Thread_local` の変数を定義できない**（リンクでエラー）。errno などの C ライブラリの状態は、OS がコアごとに持っている。
11. **ファイルシステムは FAT32**（SD カード）。大文字と小文字を区別しない、パーミッションも所有者もない、同時に開けるファイルは、ソケット・パイプと合わせて 253 個（fd 3〜255）。書き込みは最大約 1 秒遅れて SD カードに書かれる（その間に電源が切れると失われる）。
12. **時計（RTC）がない**。日時は Pi 5 から合わせてもらう（SNTP のサービス、または nos.py がつなぐたびに送る）。合うまでは「起動を 2026-01-01 00:00 UTC」として数える。
13. **ネットワークは USB でつないだ Pi 5（192.168.7.1）と、内蔵 Wi-Fi の先（インターネット）**と話せる（Monocs は USB では 192.168.7.2、Wi-Fi は DHCP）。IPv4 だけ。Wi-Fi がつながっていれば名前（DNS）も、HTTPS（`curl`、ライブラリ `-ltls`。11 章）も使える。Linux と同じソケット（`socket`・`connect`・`select`・`getaddrinfo` など。2026-11-14 から）と、Monocs 独自の待つ形の API（`monocs_tcp_*`・`monocs_udp_*`）のどちらも使える。
14. **マルチコアは、pthread（スレッド 1 つに空いたコアを 1 つ）か、コアに関数を任せる形**（`monocs_core_start`）。スケジューラはないので、同時に動かせるスレッドは空いたコアの数（サービスがあれば 2）まで（→ 10 章）。サービスがあるとコア 3 はサービス用で、アプリには 3 コアに見える。
15. **コンパイラは自作の slimcc-a64**（GCC ではない）。最適化はそれなりにするが、ベクトル化はしない。速さを調べるときは、シェルの `perf`・`perf record` で実機で測る。

---

## 1. Monocs の全体像

| 項目 | Monocs | ふつうの Linux |
|---|---|---|
| プログラムの形 | C のソースのまま（動かすときにコンパイル。キャッシュあり）。`cc -o` で Monocs 独自の形の実行ファイルも作れる | ELF の実行ファイル |
| 動かし方 | `run FILE.c`、名前だけ（PATH）、`./a.out` | `./a.out` |
| 同時に動くもの | アプリ 1 つ ＋ 裏のサービス（コア 3） | 多数のプロセス |
| 保護 | なし（EL1、単一アドレス空間）。よくある事故だけ例外で捕まえる | プロセスごとに分離 |
| C ライブラリ | picolibc（OS に組み込み）。使える関数は表にあるものだけ | glibc など |
| コンパイラ | slimcc-a64（OS に組み込み） | gcc・clang |
| 端末 | USB シリアル・UART・telnet（xterm 互換の端末ソフト） | tty |
| ファイル | SD カードの FAT32 | ext4 など |

ハードウェアは Cortex-A53 ×4（1GHz、ARMv8.0-A。LSE アトミック命令なし）、RAM 512MB（アプリが使えるヒープは約 400MB）。L1 データキャッシュはコアごと 32KB、L2 は共有の 512KB。

---

## 2. 最初のプログラム

```c
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("Hello, Monocs!\n");
    for (int i = 0; i < argc; i++)
        printf("argv[%d] = %s\n", i, argv[i]);
    return 0;
}
```

- Monocs のシェルで: `cc -o hello /usr/share/samples/hello.c && ./hello a b`（`argv[0]` は打ったパス、`argv[1]` から引数）。`make hello`（今のフォルダの hello.c から）でもよい。
- Pi 5 から送って動かす: `python3 tools/uart/nos.py run hello.c a b`（`/tmp` に送り、作って動かす）。
- 送るだけ: `nos.py push LOCAL /tmp/hello.c`、フォルダごと: `nos.py pushdir DIR /root/apps/名前`（/root/apps に置けば名前で動く）。
- Monocs の上で直接書く: エディタ `mi`（`mi /root/apps/hello.c`）。
- `main` の戻り値（または `exit` の引数）が終了状態になり、シェルの `&&`・`||` で使える。

---

## 3. プログラムの置き場所と動かし方

### 3.1 フォルダの役割

| フォルダ | 中身 |
|---|---|
| `/root/apps` | 自分で作った・入れたアプリ。名前だけで動く（`/usr/bin` より先に探す） |
| `/usr/bin` | Monocs に付いてくるアプリ（ls、cat、cp、less、grep、curl、tar、top、gpio など。C のソースのまま）。更新で入れ替わる |
| `/usr/lib` | 共有のライブラリ（C のソースのまま。3.6） |
| `/usr/sbin` | 裏で動くプログラム（13 章。C のソースのまま）。起動時に始めるものは `/etc/services.txt` |
| `/usr/include`、`/usr/lib/slimcc/include` | アプリが使うヘッダー（picolibc、`monocs.h`、`curses.h`、slimcc の標準ヘッダー） |
| `/etc` | 設定（`services.txt`、`wpa_supplicant.conf`、`htdigest`、`ssh/`、`ssl/certs/`） |
| `/boot` | カーネルと起動の材料（触らない。ルートの `bootcode.bin`・`start.elf`・`fixup.dat`・`config.txt` も） |
| `/usr/share/samples` | サンプル |
| `/usr/src/kernel` | Monocs 自身のソース一式（Monocs の上でカーネルを作り直すための材料） |
| `/tmp` | 作業用（nos.py の run もここに置く）。起動のたびに空になる |

### 3.2 名前だけで動かす（PATH）

組み込みのコマンドにない名前は、環境変数 PATH（既定 `/root/apps:/usr/bin`）のディレクトリから順に、`ディレクトリ/名前`（実行ファイルか、ソースのフォルダ）か `ディレクトリ/名前.c` を探して動かす（ソースならその場でコンパイル。2 回目からはキャッシュ）。例: `/root/apps/mi/`（フォルダ）があれば `mi file.txt`。組み込みのコマンド（cd、cc、make、svc、perf など。`help` で一覧）がいちばん先で、同じ名前のアプリは隠れる。`which 名前` でどれが動くかわかる。

`/` を含む名前（`./fd`、`/root/apps/mi`）は PATH を探さず、そこにある実行ファイル・フォルダのアプリ・.c を動かす。そこに何もなくても、親のフォルダの Makefile が作るプログラムの名前と同じなら、そのフォルダを動かす（Linux で make した後に `./mi` と打つのと同じ形）。

シェルの書き方（2026-11-15 から。bash・POSIX の sh と同じところ）:
- 構文: `;`・改行・`&&`・`||`・`|`・`!`、`if 条件; then …; elif …; then …; else …; fi`、`for NAME in 語…; do …; done`、`while 条件; do …; done`、`until`、`case 語 in パターン|パターン) …;; esac`、`break [N]`・`continue [N]`、`{ …; }`、`( … )`（中の変数・cd などは外に残らない）、関数 `NAME() { …; }`・`function NAME { …; }`（`local`・`return`・`$1`…・`$#`・`$@`）、ヒアドキュメント `<<WORD`・`<<-WORD`・`<<'WORD'`、`# 注釈`。スクリプトのファイルは `sh FILE [引数]`（別のシェルとして）・`source FILE`・`. FILE`（今のシェルで）。if・for などはリダイレクト・パイプの段にもなる（`for f in *.c; do …; done > list.txt`）。対話では、閉じていない行（if・for の途中、引用符、行の終わりの `\`・`|`・`&&`）の後に `> ` を出して続きを読む。`/root/.profile` と system() の文字列も複数の行でよい。
- 展開: `$NAME`・`${NAME}`・`$?`・`$1`〜`$9`・`${10}`・`$#`・`$@`・`$*`・`$0`（sh）・`$$`（いつも 1）、`${NAME:-語}`・`${NAME-語}`・`:=`・`=`・`:+`・`+`・`:?`・`?`、`${#NAME}`（文字の数）、`${NAME#パターン}`・`##`・`%`・`%%`、`${NAME/パターン/置き換え}`・`//`・`/#`・`/%`、`${NAME:位置:長さ}`、`{a,b}`・`{1..5}`・`{a..e}`・`{01..10..2}`、`~`・`~/…`（$HOME）、`$(...)`・`` `...` ``（中の行の標準出力。終わりの改行は除く。中の cd・export・代入は外に残らない）、`$((式))`（整数。+ - * / % ( )、比べる、&& ||、ビットの演算、? :。名前は変数の値）。引用符の外と `"..."` の中で展開し、`'...'` と `\$` はそのまま。そのコマンドを動かす直前に展開する（`false; echo $?` は 1）。
- 引用符の外で展開した値は、空白（スペース・タブ・改行）で語に分け直す（`X="a b"; prog $X` は 2 つの引数、`"$X"` は 1 つ）。そのあと、引用符の外の `*` `?` `[...]` をファイル名に展開する（名前の順。`.` で始まる名前はパターンも `.` で始まるときだけ。合うものがなければそのまま）。代入の形の語の値とリダイレクトの行き先は、分けず、ファイル名にもしない。
- 代入: `NAME=値 コマンド` は、そのコマンドの間だけ環境変数にする。`NAME=値` だけなら、シェルの変数（`export NAME` するまで、アプリ・system() の子には見えない）。
- 組み込みのコマンド: `:`、`set -- 引数…`（位置の引数。`set` だけなら変数の一覧。`-e` などのオプションはない）、`shift [N]`、`local`、`return [N]`、`unset -f NAME`、`test`・`[`（-e -f -d -s -n -z -r -w -x、= != < >、-eq -ne -lt -le -gt -ge、-nt -ot、! -a -o ( )）、`unset NAME`、`true`・`false`、`timeout [-s SIG] [-k 秒] [--preserve-status] 秒 コマンド`（期限を過ぎたら SIG（既定 TERM）を届ける。関数を登録していればその関数（コア 0 が待つところで呼ぶ）、既定の動きなら止める。`-s KILL` と `-k` の時間を過ぎたら必ず止める。期限を過ぎたら 124。組み込みのコマンドは止めない）、`dmesg`（カーネルの記録。HTTP の `GET /.log` でも読める）。
- Ctrl-S・Ctrl-Q: 流れている出力を止める・再開する（Linux の端末の IXON と同じ。2026-11-19）。止めている間もアプリは動いていて、出力しようとしたところで待つ。Ctrl-C でも再開してアプリを止める。curses のアプリ（raw）と、termios で IXON を外したアプリには、ふつうのキーとして渡る（mi の Ctrl-S は保存のまま）。
- Ctrl-C: アプリの実行中ならアプリを止め、行の残り（`;` の後、繰り返しの続き）も動かさない。組み込みのコマンドだけの繰り返し（`while true; do true; done`）も止まる（`$?` は 130）。
- ないもの: `&`（裏で動かす。Monocs は同時に 1 つのプログラムだけなので、書くと誤り）、`set -e` などのオプション、`$((i++))`・`$((x=1))` などの代入、`<<<`、`[[ … ]]`、配列、`trap`、`~名前`、`exec`・`eval`・`read`・`printf`（組み込みのコマンドとしては。`read` は stdin が端末でないときに使うことが多い）。1 行（続きの行をつないだもの）は 8192 文字、1 つのコマンドの引数は 255 個まで。

### 3.2.1 すぐ試す（run）

- `run FILE.c [引数]`（`.c` は省ける: `run hello`）・`run DIR [引数]`・`run -`（打ち込んだ C。`.` だけの行か Ctrl-D で終わり）: その場でコンパイルして動かす。ファイルは作らない（Linux の `tcc -run` と同じ使い方）。オプションはファイルの前に（`run -std=c17 -DDEBUG -lgpio prog.c`）。
- コンパイルの結果は `/var/cache/slimcc` に覚え、ソースと読んだヘッダーの中身・オプションが同じなら、次からは読み込むだけ（中身で比べる。2026-11-21 から、大きさと FAT の日時が前と同じなら中身を読まない。時計を戻して同じ大きさで書き換えたときなどに見逃したら `cache clear NAME`）。`cache` で量、`cache clear` で全部消す。`cache clear vim`（名前はシェルが動かすものと同じ探し方）・`cache clear /root/apps/vim`・`cache clear hello.c` なら、そのプログラムのソースのものだけ（/usr/lib の共有のライブラリの分は残す）。消すとメモリに覚えたつないだ結果も捨てるので、次はコンパイルし直す。
- ちょっと書いて試すのは run、作って置く・名前で使うのは make・cc -o（下）。

- 2 回目からは、前につないだ結果をそのまま使う（2026-11-20 から。vim の起動 約 2 秒 → 0.01〜0.4 秒）。大域変数・static の変数は毎回最初の値に戻すので、Linux で毎回新しいプロセスを始めるのと同じ。ソース・ヘッダーを書き換えた、フォルダにファイルを足した・消した、オプションが違う、ときは作り直す。

### 3.3 1 つのファイル、複数のファイル（cc）

- `cc -o PROG FILE.c... [-std=…] [-D…] [-U…] [-I…] [-l名前] [-fopenmp …]`: いくつかの .c をコンパイルして、1 つの実行ファイルにする（gcc と同じ書き方。`-o` がなければ `a.out`。使えるコアで分担）。`-O2`・`-Wall`・`-g` などは読み捨てる。
- `cc -c FILE.c [-o FILE.o]`: .o を書く（Monocs のカーネルを Monocs の上で作るとき用。.o からは実行ファイルを作れない）。
- つなぐ規則は GNU ld と同じ: `static` はそのファイルの中だけ、グローバルは全体で 1 つ、強い定義が 2 つならエラー、弱い定義（`__attribute__((weak))`）は強い定義に負ける。足りない関数は、作るときに `link error: undefined symbol 'X'` と出る。

### 3.4 Linux の Makefile をそのまま使う

フォルダに `Makefile`（`GNUmakefile`・`makefile` も）があれば、Linux と同じ Makefile からコンパイルするものを決める（2026-11-14）。Linux 用のプロジェクトを、ソースも Makefile も変えずに置いて動かせる。

- 既定のゴール（`all` など）から規則をたどり、前提に .c か .o を持つ最初の目標をプログラムとする。その前提の .c（.o なら同じ名前の .c。`build/x.o` は `x.c` も探す）だけをコンパイルする（テスト用の main を持つファイルは、Makefile に書かれていなければ入らない）。
- オプションは `$(CPPFLAGS) $(CFLAGS)` の `-std=`・`-D`・`-U`・`-I`。ほか（`-Wall`、`-O2` など）は黙って使わない。`$(LDFLAGS) $(LDLIBS) $(LIBS)` の `-l名前` は、`/usr/lib/名前` があるものだけつなぐ（`-lncursesw`・`-lm` などは Monocs が持っている）。
- 読むのは GNU make の書き方（代入 `=`・`:=`・`+=`・`?=`、`ifeq`・`ifneq`・`ifdef`・`else`・`endif`、`define`、関数 `addprefix`・`patsubst`・`wildcard`・`filter`・`foreach`・`call` など、置き換え `$(SRCS:.c=.o)`）。規則のコマンドは動かさない。`$(shell ...)` は空になる（`ifneq ($(findstring mingw,$(shell $(CC) -dumpmachine)),)` などは Linux と同じ枝になる）。`include` は読む（2026-11-20 から。ないファイルは飛ばす）。.o の .c はその .o の規則の前提（`objects/x.o: src/x.c`）、なければパターンの規則（`%.o: %.c`）・同じ名前の .c。オプションは .o を作るコマンドの行から取る（コマンドの行がなければ `$(CPPFLAGS) $(CFLAGS)`）。`-include FILE` も使える。
- Makefile がなければ、`make NAME` は NAME.c から作る（GNU make の暗黙の規則）。
- Linux の GNU make と同じ展開になることは `sh tools/make/test.sh` で確かめている。

### 3.4.0 make（作り方を選ぶ。2026-11-16）

- **自分のアプリ**: ソースのフォルダ（か `名前.c`）を /root/apps に置けば、名前だけで動く（PATH は /root/apps:/usr/bin。その場でコンパイル、キャッシュあり）。Pi 5 のフォルダからは `sh tools/apps/push.sh DIR`。
- **/usr/bin とサービス**: /usr/bin のコマンドと /usr/sbin のサービスも C のソースのまま（2026-11-19 から。名前で動かすときにコンパイル、キャッシュあり）。Pi 5 から入れ替えるのは `sh tools/update_bin.sh`。/etc/services.txt にはソースの名前（`/usr/sbin/httpd.c`、フォルダなら `/usr/sbin/sshd`）を書く。
- **make（2026-11-19 から）**: 実行ファイルは作らない。`make`（Makefile のプログラム）・`make posix` など（目標・`変数=値` を選ぶ）・`make hello`（Makefile がなければ hello.c。GNU make の暗黙の規則）は、作り方をフォルダの `.monocs-make` に覚え、コンパイルして誤りがないかを確かめ、キャッシュに入れるだけ。動かすのは今までどおり名前・`./prog`・`run`（ソースから。キャッシュで速い）。`make clean` は選んだ作り方を消す。実行ファイルがほしいときは `cc -o PROG FILE.c...`（Monocs 独自の形。`./PROG` で動く。ソースを直しても作り直すまで古いまま）。

- Monocs は Makefile のコマンドを動かさない（読んで、プログラムとその .c・オプションを決めるだけ）。Linux で `make posix` のように目標や `変数=値` を選ぶときは、そのフォルダでシェルの `make` を使う: `make posix`、`make MYCFLAGS=-DX=1`、`make -C /root/apps/lua posix`。選んだものはフォルダの `.monocs-make` に覚え、コンパイルまで済ませる（誤りはここで出る。うまくいかなければ前の選び方のまま）。その後に名前・`./lua` で動かすと、その選び方でコンパイルしたもの（キャッシュ）が動く。`make clean` で選び方を消す（`make` だけなら既定に戻す）。
- 目標の前提からプログラムが見つからなければ、その目標（なければ前提の目標）のコマンドの中の `$(MAKE) 目標 変数=値`・`cd DIR && $(MAKE) …`・`make -C DIR …` を読んで、読み直す（4 段まで）。`make` だけ（目標を選んでいない）ときは、見つからなければ `all` を見る。
- `$(shell uname)`・`` `uname` `` は `Monocs`（Linux ではない。Linux と名乗ると dlopen などの要る作りになる）、`uname -m` は `aarch64`。ほかのシェルのコマンドは空。
- 例: Lua 5.4（lua.org の src をそのまま /root/apps/lua に置く）は、`make` だけなら汎用の C、`make posix` なら io.popen なども使える。`make linux` は dlopen（`dlfcn.h`）がないので通らない。

### 3.4.1 オプション（compile_flags.txt）

- /usr/lib のライブラリのフォルダは `compile_flags.txt`（clangd と同じ形式。1 行に 1 つ）を読む（3.6）。アプリのオプションは `cc` のコマンドか Makefile の `CFLAGS` に書く。使えるのは `-std=`・`-D名前[=値]`・`-U名前`・`-Iディレクトリ`（相対パスはフォルダから）・`-l名前`・`-fopenmp`・`-fno-schedule-insns2`・`-fno-unroll-loops`・`-fno-strict-aliasing`。ほかのオプション（`-O2`、`-Wall` など）は「unknown option (ignored)」と出して無視する。
- `cc` のコマンドでも同じものを付けられる: `cc -o prog -std=c17 -DDEBUG -I/usr/lib/x prog.c`。
- 既定の規格は `gnu23`。`-std=c17`・`c11`・`c99`・`gnu17` なども選べる。
- アプリには `_POSIX_MONOTONIC_CLOCK` が定義される（`CLOCK_MONOTONIC` を使えるように）。
- POSIX の関数（`strdup`、`strcasecmp`、`localtime_r`、`getline` など）の宣言は、picolibc のヘッダーでは規格によって隠れる。必要なら `-D_DEFAULT_SOURCE` か `-D_GNU_SOURCE`（`compile_flags.txt` に書く）。

### 3.5 除くファイル（.runignore）

（/usr/lib のライブラリのフォルダで使う。）テスト用の main を持つファイルなど、入れたくないものはフォルダの `.runignore` に 1 行に 1 つ書く（.gitignore と同じ考え方。`*`・`?`・`[]` が使える）。

```
# テスト用（main を持つ）
test_buffer.c
*_test.c
tests/
/tools/*.c
```

`/` を含まない名前はどのサブフォルダでも、`/` を含むものはフォルダからの位置で比べる。終わりの `/` はフォルダだけ、先頭の `/` はそのフォルダの直下だけ。`compile_flags.txt` に書かないのは、clangd がその行をすべてオプションとして読むため。

### 3.6 共有のライブラリ（/usr/lib）

- `/usr/lib/名前/` に C のソースとヘッダーを置く（サブフォルダも、`.runignore` も使える）。
- `run`・`cc` に `-l名前`（Makefile なら `LDLIBS`）を付けると、ライブラリの .c も一緒にコンパイルしてつなぐ（静的リンクと同じ）。`-I/usr/lib/名前` も足すので `#include <名前.h>` と書ける。
- ライブラリは、**ライブラリのフォルダの Makefile か `compile_flags.txt`（なければ既定の gnu23）でコンパイル**する。アプリのオプションはライブラリに効かず、ライブラリのオプションもアプリに効かない。
- ライブラリの `compile_flags.txt` の `-l別の名前` で、別のライブラリも使える（同じものは 1 回だけ、輪になってもよい、16 個まで）。
- 別のファイルとしてつなぐので、ライブラリとアプリで `static` の名前がぶつかっても問題ない。グローバルの名前はぶつかるとエラー。

### 3.7 速さの目安（実機）

- 初めて動かす（コンパイル）: mi（38 ファイル）を 3 コアで 約 1.2 秒、Lua（33 ファイル）約 2 秒。1 つのファイルなら 0.1 秒前後。
- 動かす（2 回目から）: キャッシュから読み込んで OS の関数とつなぐだけ。前につないだ結果も覚えておくので、小さなもの（ls など）は 0.01 秒前後、vim でも 0.4 秒以下。

---

## 4. コンパイラ（slimcc-a64）の特性

### 4.1 型と定義済みマクロ（実機で確かめた値）

| 項目 | 値 |
|---|---|
| `__STDC_VERSION__` | 202311（既定の gnu23） |
| `char` | **符号なし**（AArch64 の約束。`(char)-1 < 0` は偽） |
| `int` / `long` / ポインタ / `long long` | 4 / 8 / 8 / 8 バイト（LP64） |
| `long double` | 16 バイト（binary128。ソフトウェアで計算するので遅い） |
| `wchar_t` | 4 バイト、符号なし |
| 定義されるもの | `__aarch64__`、`__unix__`、`__ELF__`、`__MONOCS__`、`__GNUC__`=4・`__GNUC_MINOR__`=2・`__GNUC_PATCHLEVEL__`=1（picolibc のため。GCC の新しい機能はない）、`__STDC_NO_COMPLEX__` |
| 定義されないもの | `__linux__`、`__SIZEOF_INT128__`、`__ARM_NEON` |

Monocs かどうかを見分けるなら `#ifdef __MONOCS__`。

### 4.2 使えない・違うもの

- **`_Complex`（複素数）**: 使えない。
- **アプリの `_Thread_local`**: リンクでエラー（`thread-local variables are not supported`）。
- **NEON の組み込み関数（`arm_neon.h`）**: 使えない。インラインアセンブリなら NEON の命令も書ける。
- **`__int128`**: `_BitInt(128)` として扱う（使えるが、`__SIZEOF_INT128__` は定義されない）。
- **宣言のない関数の呼び出し**: エラー（`implicit declaration of a function`）。
- **警告**: ほとんど出さない。GCC なら警告になる誤りもそのまま通ることがある。
- **最適化の指定**: `-O` はない（いつも同じ最適化をする）。
- **ループの最適化**（2026-11-15 から）: ループの中で変わらない計算（大域の配列の番地・定数・その組み合わせ）と、ループの中で書き換えられない場所の読み出し（大域変数・ポインタのメンバーなど。2026-11-16 から）を外に出す、for・while を「前で 1 回、後ろで 1 回調べる」形にする（条件がメモリを読むものは除く）、`for (i = a; i < n; i += s)` の形（符号つきの整数の i、ループを含まない小さな本体）を 2 回分ずつ回す（`-fno-unroll-loops` でしない）。配列の和のループで約 3 倍、nbody で約 1.5 倍速くなった。コンパイルは数 % 遅くなる。
- **型による別名の判定**（strict aliasing。2026-11-16 から。GCC・clang の `-O2` と同じ）: 型の違う読み書き（例: `int` を書く・ポインタを読む、`float` と `int`）は同じ場所でないとみなし、読み出しをまとめたり、ループの外に出したりする。C の規則どおり、`char`・`unsigned char` での読み書きは何とでも重なる、符号だけ違う整数どうしは重なる、共用体のメンバーで読み替えるのはよい。規則を破るコード（`*(int *)&f` で float のビットを読む、など）は、Linux の GCC と同じく正しく動かないことがある。その形のコードは `memcpy` で書き直すか、`-fno-strict-aliasing`（`run`・compile_flags.txt・Makefile の `CFLAGS`・シェルの `cc`）を付ける。
- **命令の並べ替え**（2026-11-15 から）: ループの中のブロックの命令を、Cortex-A53 で待ちが少なくなる順に並べ替える（GCC の `-fschedule-insns2` と同じ名前。浮動小数点の計算のループで約 1 割速くなる。整数・分岐の多いループはほとんど変わらない）。しないときは `-fno-schedule-insns2`（`run`・compile_flags.txt・Makefile の `CFLAGS`・シェルの `cc`）。動かさないもの: インラインアセンブリ、volatile・アトミックの読み書き（互いの順も、ほかの読み書きとの順も変わらない）。
- **浮動小数点の積和演算**: gnu の規格（既定）では `a*b+c` を fmadd（丸めが 1 回）にする。ISO の規格（`-std=c17` など）ではしない。結果の最後の桁が GCC の `-O0` と違うことがある。`-ffp-contract=off` は `run` からは渡せない。
- **printf の精度**: double は 17 桁、long double は 34 桁より先を 0 で出す（規格の範囲）。

### 4.3 使えるもの

- C23 の機能（`nullptr`、`constexpr`、`typeof`、`auto`、`[[maybe_unused]]` などの属性、`_BitInt`、`#embed` など）、VLA、文の式 `({ ... })`、`__attribute__((weak/aligned/packed/always_inline/...))`、`__builtin_*` の主なもの、C11 のアトミック（`<stdatomic.h>`。LSE ではなく ldaxr/stlxr のループになる）、GCC の `__atomic_*`・`__sync_*`（2026-11-20 から。中身は C11 のアトミックと同じ。メモリの順序の引数は見ずにいつも seq_cst。nand の形はない）、数学関数の `__builtin_ceil`・`floor`・`trunc`・`round`・`fabs`・`sqrt`（と `f` の形）。どれも動くことを確かめてある。
- インラインアセンブリ（制約 r、w、m、i、Z、g、X、数字、修飾子 w、x、s、d、q など）。条件フラグの出力（`=@cc…`）とビットフィールドへの出力は未対応。

### 4.4 エラーの見方

```
/p1.c:33:
    printf("isatty0=%d\n", isatty(0));
                           ^ implicit declaration of a function
```

コンパイルのエラーは「ファイル:行」と、その行・位置を出す。リンクのエラーは `link error: undefined symbol 'NAME'`（表にない関数か、定義を書き忘れた関数）、`multiple definition of 'NAME'`（同じグローバルが 2 つ）。

---

## 5. 使える関数（OS API の表）

アプリから呼べる関数・変数は `os/api/api.def` にあるものだけ（OS に組み込まれた picolibc・curses・Monocs 独自の関数）。**ヘッダーに宣言があっても、表になければリンクでエラーになる**。正確な一覧は api.def を見ること。

### 5.1 ある（主なもの）

- **stdio**: printf 系（`printf` `fprintf` `sprintf` `snprintf` と v 版）、scanf 系、`puts` `fputs` `putchar` `fputc` `getchar` `fgetc` `fgets` `ungetc`、`fopen` `fdopen` `freopen` `fclose` `fread` `fwrite` `fflush` `fseek` `ftell` `rewind` `fgetpos` `fsetpos` `feof` `ferror` `clearerr` `fileno`、`remove` `rename` `perror` `setvbuf` `setbuf`、`getline` `getdelim`
- **stdlib**: `malloc` `calloc` `realloc` `free`、`atoi` 系、`strto*`（`strtold` まで）、`abs` 系、`div` 系、`qsort` `bsearch` `rand` `srand`、`exit` `_Exit` `abort` `atexit`（32 個まで） `getenv`
- **string**: `mem*`、`str*`（`strdup` `strndup` `strcasecmp` `strtok_r` を含む）
- **ctype**、**math**（double・float・一部の long double）、`isnan` などのマクロ
- **time**: `time` `clock` `difftime` `localtime(_r)` `gmtime(_r)` `mktime` `tzset` `strftime` `gettimeofday` `clock_gettime` `clock_settime` `settimeofday` `nanosleep`
- **ファイル**: `open` `close` `read` `write` `lseek` `unlink` `rmdir` `mkdir` `chdir` `getcwd` `stat` `lstat`（`stat` と同じ。シンボリックリンクはない） `fstat` `isatty` `access` `realpath` `utime` `utimes` `statvfs` `ftruncate` `truncate` `fsync` `fdatasync`、`fcntl` のレコードのロック（`F_SETLK` など。いつも取れる → 9.1）、`chmod` `fchmod` `chown` `fchown`（何もしない → 9.1）、`readlink`（いつも EINVAL）、`symlink` `link`（EPERM）、`opendir` `readdir` `closedir`（readdir は Linux と同じく最初に `.` と `..` を返す。2026-11-14 から）
- **プログラムの実行**: `system`（→ 6.4）
- **形だけのもの**: `getpid`（動かすたびに変わる番号）、`kill`（今のアプリにだけ送れる。→ 6.5）
- **プログラムの実行**: `popen` `pclose`（一時ファイルを使う。→ 6.4）
- **シグナル**: `signal` `sigaction` `raise` `sigprocmask` `sigpending` `sigsuspend` `pause` `psignal`、`sigemptyset` など（→ 6.5）
- **ロケール**: `setlocale`（→ 8.4 の近く、%ls の説明）。受け付ける名前は `C`・`POSIX` と、文字の種類が UTF-8 のもの（`C.UTF-8`・`ja_JP.UTF-8` など。どれも小数点などは C と同じ）だけで、ほかは NULL（Linux でその言語のロケールを入れていないときと同じ。2026-11-20 から）
- **端末**: `ioctl(fd, TIOCGWINSZ, &ws)`（`<sys/ioctl.h>` だけで `struct winsize` も使える。Linux と同じ。2026-11-14 から）、`tcgetattr` `tcsetattr` `tcflush` `tcdrain` `tcflow` `tcsendbreak` `tcgetsid` `tcgetwinsize` `tcsetwinsize` `cfget/setispeed` `cfget/setospeed`、`ioctl`（`TIOCGWINSZ` だけ）。`cfmakeraw` はない（フラグを自分で外す）
- **文字（UTF-8）**: `wcwidth`、`mbrtowc` `mbrlen` `mbtowc` `mblen` `wcrtomb` `wctomb` `mbstowcs` `wcstombs` `mbsinit`、`MB_CUR_MAX`（4）。Linux の UTF-8 のロケールと同じ（RFC 3629。U+10FFFF より大きいものは誤り）
- **その他**: `setjmp` `longjmp`、`errno`、`assert`、`strcasestr`、`basename` `dirname`
- **端末の型（termcap。2026-11-20）**: `tgetent` `tgetflag` `tgetnum` `tgetstr` `tgoto` `tputs` `tparm` `tiparm`（`<termcap.h>`）。端末はどれも xterm と同じに扱い、どの名前でも xterm-256color の値（Pi 5 の ncurses と同じ。文字列は terminfo の形）。環境変数 `TERM` の既定は `xterm-256color`
- **プロセスの関数（形だけ）**: `fork` `vfork` `execv` `execvp` `execve` `dup` `dup2` はいつも -1（ENOSYS）、`wait` `waitpid` は -1（ECHILD）。Linux のプログラムがつながるように置いてある。別のプログラムは `system`・`popen` で
- **pipe（2026-11-21）**: `pipe` `pipe2` は同じプログラムの中の pipe（自分自身・pthread のスレッドの間。poll で待つ間に自分を起こす self-pipe など）。64KB、待つ・`O_NONBLOCK`、書く口がなくなれば読む側は 0（poll は `POLLHUP` だけ）、読む口がなくなれば `EPIPE` と `SIGPIPE`、`fstat` は `S_IFIFO`、`FIONREAD`。ほかのプログラムへはつながらない（プロセスがない。→ 6.4）。128 個まで
- **fd の数（2026-11-21）**: ファイル・ソケット・pipe で fd 3〜255 を分け合う（`sysconf(_SC_OPEN_MAX)`・`RLIMIT_NOFILE` は 256）。`select` の `fd_set` は Linux と同じ `FD_SETSIZE` 1024（Monocs がアプリに `-DFD_SETSIZE=1024` を渡す。picolibc の既定は 64）
- **struct tm の `tm_gmtoff`・`tm_zone`（2026-11-21）**: glibc と同じ（`localtime`・`gmtime`・`mktime`・`timegm` が埋める。`gmtime` は 0 と "GMT"）
- **glibc と同じく `_GNU_SOURCE` で宣言されるもの（2026-11-21）**: `environ`（`<unistd.h>`）、`malloc_usable_size`（`<stdlib.h>` にも。glibc は `<malloc.h>` だけ）。`setuid` `setgid`（0 だけ成功）
- **そのほか（2026-11-20）**: `umask`（覚えるだけ）、`getgrgid` `getgrnam`（root だけ）、`setitimer` `getitimer`（ITIMER_REAL だけ。alarm と同じく、アプリが待っているところで届く）、`environ`、`_exit`、`<utime.h>` の `utime`。`struct winsize` は Linux と同じ形（`ws_xpixel`・`ws_ypixel` は 0）。`SSIZE_MAX` は `#if` に書ける
- **SQLite などが使うもの**（2026-11-20）:
  - `mmap` `munmap` `msync` `mprotect` `madvise`: `MAP_ANONYMOUS` は 0 で埋めたメモリ。ファイルは、確保したメモリに中身を読んだもの（`MAP_SHARED` で `PROT_WRITE` なら `msync`・`munmap` のときに書き戻す。書いたそばからファイルに見えるわけではなく、同じところを 2 回 mmap すると別々のメモリ）。`MAP_FIXED` はない。アプリが終わると消える（書き戻さない）
  - `getrusage`（`ru_utime` はアプリを始めてからの、アプリのコアの CPU 時間。`ru_maxrss` はアプリ用のヒープ。`<sys/resource.h>` は Linux と同じ形）、`getrlimit` `setrlimit`（開けるファイル 256、スタック 8MB、ほかは上限なし）
  - ユーザー: `getuid` `geteuid` `getgid` `getegid`（いつも 0）、`getpwuid` `getpwnam`（root だけ。ホームは /root）。`struct passwd` は picolibc の形（`pw_gecos`・`pw_passwd` はない）
  - `dlopen` `dlsym` `dlclose` `dlerror`（`<dlfcn.h>`）: 共有ライブラリはないので、`dlopen` はいつも NULL を返し、理由を `dlerror` で返す（読み込めなかったときの道で動く）
  - GCC の `__atomic_*`・`__sync_*` の組み込み関数（コンパイラ。→ 4.3）
- **Linux のプログラムがよく使うもの**（2026-11-14）:
  - 引数: `getopt` `getopt_long` `getopt_long_only`（`optind` は Linux と同じく 1 から。アプリごとに初めに戻る）、`getsubopt`
  - 環境変数: `setenv` `unsetenv` `putenv`（アプリが終わると元に戻る。Linux で子のプロセスの変更が親に残らないのと同じ。`putenv` は文字列の写しを入れる）
  - 待つ: `sleep` `usleep`
  - 文字列: `strsep` `stpcpy` `stpncpy` `strlcpy` `strlcat` `memmem` `memrchr` `strchrnul` `strverscmp` `strsignal` `strerror_r`（GNU 版と POSIX 版） `explicit_bzero` `bzero` `bcopy` `index` `rindex` `ffs`
  - 書式・ストリーム: `asprintf` `vasprintf` `dprintf` `vdprintf` `open_memstream` `fmemopen`
  - その他: `qsort_r`（GNU 版と BSD 版）、`reallocarray`、`mkstemp`、`random` `srandom` `rand_r` `drand48` `srand48` `lrand48`、`ctime(_r)` `asctime(_r)` `timegm` `strptime`
  - パターン: `fnmatch`、`glob` `globfree`（Linux と同じ形の `glob.h`。結果は glibc と同じ）、`regcomp` `regexec` `regerror` `regfree`（POSIX の BRE・ERE）
  - ワイド文字: `wcslen` などの文字列の関数、`wcsto*`、`wmem*`、`swprintf` `swscanf`、`fgetwc` `fputws` などの入出力（ファイルの中は UTF-8。glibc と違い、同じストリームに printf と fputws を混ぜてもよい）、`isw*` `towupper` `towlower`（glibc の C.UTF-8 と同じ表。日本語・ラテン文字なども）、`wcswidth`
  - printf の `%ls`・`%lc`（2026-11-14 から）: Linux と同じく、ロケールが UTF-8 のときだけ日本語などを UTF-8 で出す（「C」のロケールでは ASCII だけ）。アプリは Linux と同じく「C」で始まり、`setlocale(LC_ALL, "")` で環境変数 `LC_ALL`・`LC_CTYPE`・`LANG` の順に選ぶ。Monocs の既定は `LANG=C.UTF-8`。`mbrtowc`・`wcrtomb`・`fputws` などはロケールを見ず、いつも UTF-8
  - 動きは Linux（glibc）と比べてある: `sh tools/test/linuxapi.sh`
- **curses**（12 章）、**Monocs 独自**（`monocs.h`。下の 5.3）

### 5.2 ない（よく使うのに無いもの）

`ualarm`、`fork` `exec*` `dup` `dup2`、`<threads.h>`、`aligned_alloc` `memalign` `posix_memalign`（→ 7.3）、`times`、`iconv`、共有ライブラリの読み込み（`dlopen` は形だけ。→ 5.1）、`fork`・`exec`・`dup`（形だけ。→ 5.1）、IPv6・Unix ドメインのソケット・`sendmsg`・`recvmsg`（→ 11.1）。

対処: 小さなものは自分で書く。どうしても OS 側に要るなら、カーネルの `os/api/api.def` に足してカーネルを作り直す（picolibc にあるものなら 1 行。OS の変更なので相談のうえで）。

### 5.3 Monocs 独自の関数（`#include <monocs.h>`）

| 分類 | 関数 |
|---|---|
| 時間 | `timer_ticks()`（100Hz）、`timer_now_us()`、`timer_delay_us(us)` |
| LED | `led_set(on)`（OS の点滅は止まり、アプリが終わると戻る） |
| 端末 | `monocs_term_size(&rows, &cols)`、`monocs_term_cols()`、`uart_putc`、`uart_getc`、`uart_has_input` |
| TCP・UDP | `monocs_tcp_listen/listen_addr/accept/connect/send/recv/close/netif/peer`、`monocs_resolve`（名前から IP アドレス）、`monocs_udp_open/sendto/recvfrom/close`（11 章） |
| 日時 | `monocs_set_timezone(分)`、`monocs_clock_known()` |
| マルチコア | `monocs_cores()`、`monocs_core_start(core, fn, arg)`、`monocs_core_wait(core)`、`monocs_mutex`・`MONOCS_MUTEX_INIT`・`monocs_mutex_lock/unlock`（10 章） |
| SD カード | `monocs_disk_read/write/sync/discard`、`monocs_reboot`（ファイルシステムを壊しうる。普通は使わない） |
| コンソールの写し | `monocs_console_open/close/read/input/set_size`（telnet のサービス用） |
| システムの様子 | `monocs_sysstat`（コアごとの眠っていた時間・割り込みの数・様子、メモリ、サービスごとの時間、温度。`/usr/bin/top` が使う） |
| シェルの 1 行を動かしてもらう | `monocs_rexec_start/read/write/write_eof/status/release`（sshd の exec 用。シェルがプロンプトで待つ間に動かす。出力・入力は輪で受け渡す） |
| コンパイラ | `monocs_cc(argc, argv)`、`monocs_ld(argc, argv)`（シェルの cc・ld と同じ。セルフホストのビルド用） |


---

## 6. 実行のしくみと終わり方

### 6.1 動き方

1. シェルがソースをコンパイルし（キャッシュがあれば読むだけ）、または実行ファイルを読み込み、メモリの上で OS の関数とつなぐ（外の名前は OS API の表から探す）。
2. コア 0 で `main(argc, argv)` を呼ぶ。アプリはカーネルと同じスタック（8MB）を使う。
3. 終わったら（下の 6.2 のどれでも）シェルに戻る。

### 6.2 終わり方

| 終わり方 | 動き |
|---|---|
| `main` から戻る / `exit(n)` | 終了状態は戻り値 / n |
| 例外（NULL、変な番地、スタックのあふれなど） | 内容（ESR・ELR・FAR、原因の推測）を表示して `*** program stopped by exception`。終了状態は 139（Linux のシェルで SIGSEGV のときと同じ）。内容は `dmesg` にも残る |
| Ctrl-C | `^C at 番地, called from ...` を表示して止まる（Ctrl-C はタイマー割り込み（10ms ごと）で気づく。割り込みを止めて無限ループすると止められない）。終了状態は 130（Ctrl-\ は 131） |
| シェルの `timeout 秒 コマンド` の期限 | SIGTERM（`-s` で変えられる）が届く。関数を登録していなければ、何も表示せずに止まる（Ctrl-C と同じしくみ）。関数は、コア 0 が待つところ（sleep・入力・ソケットの待ちなど）で呼ばれる。`-k 秒` を付けると、その時間の後は必ず止まる。`timeout` の終了状態は 124 |
| curses の `raw()` の間、termios で `ISIG` を外した間 | Ctrl-C はキーとして届く。止めるのは Ctrl-\ |

止めるのを後回しにする場面: OS や C ライブラリのロックを持っている間（malloc、ファイル操作の途中）、アプリから呼んだコンパイル（`monocs_cc`）の途中。終わり次第止まる。

### 6.3 終わったときに OS がすること

- ほかのコアで動かした関数を止める。
- 開いたままのファイル・ディレクトリ・TCP/UDP の接続を閉じる（サービスのものは閉じない）。
- SD カードへの書き込みを済ませる。
- curses の画面（代わりの画面、マウスなど）を元に戻す（endwin を呼ばずに終わっても）。
- **アプリ用のヒープを丸ごと捨てる**（7.2）。

`atexit` で登録した関数は、`main` から戻ったとき・`exit` のときに、登録の逆の順に呼ばれる（32 個まで。2026-11-14 から）。例外・Ctrl-C で止まったとき、`_Exit`・`abort` のときは呼ばれない。

しないこと: 開いた FILE のバッファは書き出されないことがある（書いたファイルは自分で `fclose` か `fflush` すること）。

### 6.4 プロセスがないことの帰結

- `fork`・`exec` は使えない。
- `popen("コマンド", "r")`・`popen("コマンド", "w")`（2026-11-14）: Monocs のパイプと同じく一時ファイル（`/.popenN`）を使う。`"r"` はその場でコマンドを最後まで動かして出力を読む（コマンドと同時には動かない）。`"w"` は書いたものを `pclose` のときにコマンドの標準入力にして動かす。`pclose` は `system()` と同じ形の終了状態。
- `system("コマンド")`（2026-11-14）: シェルの 1 行（`ls -l`、`less +g -- 'FILE'`、`a | b`、`&&`、リダイレクトなど）を動かし、終わるまで待つ。戻り値は Linux と同じ形（`WEXITSTATUS(r)` で終了の値。例外で止まったときは 139、Ctrl-C は 130、コマンドがないときは 127。2026-11-15 までは止まったときは 1）。`system(NULL)` は 1。
  - 動かしたプログラム（子）は、呼んだアプリ（親）の中で入れ子に動く。親の malloc したもの・開いたファイル・curses の画面・端末の設定・カレントディレクトリはそのまま残り、子が終わると戻る（子の chdir は親に残らない）。子のメモリ・ファイル・接続は子が終わると捨てる。入れ子は 8 段まで。
  - curses のアプリは ncurses と同じく `def_prog_mode(); endwin(); system(...); reset_prog_mode(); refresh();` で画面を明け渡して戻る。
  - Ctrl-C は子だけを止める（親は system から戻って続く）。子を動かす前のシェルの解析・コンパイルの間は、Ctrl-C は子の始まりまで待たされる。
  - 子の `cd`・`export`・`setenv` は、親に残らない。
  - コア 0 のアプリからだけ（ほかのコアの仕事・サービスからは -1、errno は ENOSYS）。
- アプリが `chdir` しても、終わるとシェルのカレントディレクトリは元に戻る（Linux と同じ。2026-11-14 から。それまではシェルに残った）。
- シェルのパイプ（`a | b`）は、a を最後まで動かして一時ファイル（`/.pipe0` など）に書き、それを b が読む（MS-DOS と同じ）。同時には動かない。
- アプリが `setenv` などで変えた環境変数は、アプリが終わると元に戻る（シェルには残らない。Linux と同じ）。今あるのは `PATH`（`/root/apps:/usr/bin`）、`HOME`（`/`）、`TZ`（例 `JST-9`）など。`TZ` はいつも POSIX の形（`JST-9`）で入っている（Linux はふつう `TZ` がなく /etc/localtime）。IANA の名前（`Asia/Tokyo`）だけを読むプログラム（QuickJS の Intl など）は、`TZ` がそのまま名前として使われて違う時刻になるので、`env -u TZ` で動かす（そのときは UTC）。シェルの `export` で変え、残すなら `/root/.profile` に書く。
- アプリの static・グローバルの変数は、動かすたびに初期値から始まる（前の実行の値は残らない）。残したいものはファイルに書く。

### 6.5 シグナル（2026-11-14）

- `signal`・`sigaction` で関数を登録できる。アプリを始めるときはすべて `SIG_DFL`（`system()` の子が終わったら親の登録に戻る）。
- 届くもの: Ctrl-C（SIGINT。raw でないとき）、telnet・ssh で画面の大きさが変わったとき（SIGWINCH）、`raise`・`kill(getpid(), sig)`（その場で）。時間のシグナル（`alarm`）、子の終わり（SIGCHLD）、ほかから送られるものはない。
- **関数が呼ばれるのは、アプリが待っているところ**（キー入力・`sleep`・ネットワークの待ちなど）。割り込みの中からは呼ばない。待たずに計算し続けるアプリには、Ctrl-C を押しても届かない（Ctrl-\ で止める）。
- 待っていた読み込み（`read`・`getchar`・`fgets` など）は、関数を呼んだ後も読み続ける（glibc の `signal()` の既定の SA_RESTART と同じ）。`sleep`・`nanosleep` は Linux と同じく、関数を呼んだら残りを返して早く戻る（`nanosleep` は -1、EINTR）。
- `SIG_IGN` にすると、Ctrl-C を無視する。Ctrl-\ は、登録に関係なくいつもアプリを止める（Linux の SIGQUIT の既定と同じ。逃げ道）。
- 既定の動き（`SIG_DFL`）は Linux と同じ: SIGCHLD・SIGWINCH・SIGURG・SIGCONT は無視、ほかはアプリを終わらせる（終了の値は 128 + 番号。Linux の system() の WIFSIGNALED ではなく、ふつうの終了として返る）。
- シグナルの番号は picolibc のもの（SIGINT 2・SIGTERM 15・SIGWINCH 28 は Linux と同じ。SIGUSR1 30・SIGUSR2 31・SIGCHLD 20 は違う）。名前で書けば問題ない。
- `sa_flags` の `SA_SIGINFO`（`si_signo`）・`SA_RESETHAND`・`SA_NODEFER`、`sa_mask`、`sigprocmask` での一時的な止め（止めている間に届いたものは、止めるのをやめたときに渡す）が使える。

---

## 7. メモリ

### 7.1 大きさ

- アプリが使えるヒープは、実機で最大 約 370〜390MB（全体 約 400MB からカーネルの使用分を引いたもの）。足りなければ malloc は NULL。
- malloc は TLSF（O(1) で速い）。境界は 16 バイト。

### 7.2 アプリ用のヒープ（終わったら丸ごと捨てる）

- アプリの実行中（コア 0〜2 のアプリの仕事）の malloc は、カーネルとは別の「アプリ用のヒープ」から取る。アプリが終わったら丸ごと捨てるので、free し忘れても次のアプリに影響しない。
- 捨てた領域は 0 で塗る（終わってから次のアプリまで、アプリが使った 1MB あたり約 0.65ms。300MB で約 0.2 秒）。
- アプリ用が足りなくなったら、カーネル用の空きからも取る（それもアプリの終わりに解放する）。
- `free` はどちらのヒープのものでもよい（番地で判断する）。どちらのものでもない番地を free すると `free: invalid pointer ... (ignored)` と出して無視する。
- 注意: アプリが終わった後まで残るもの（サービスのデータなど）を、アプリから作ることはできない。

### 7.3 境界をそろえた確保

`aligned_alloc`・`memalign`・`posix_memalign` は表にない。必要なら余分に取ってそろえる:

```c
void *raw = malloc(size + 63);
void *p = (void *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);   // 64 バイト境界。free するのは raw
```

### 7.4 スタック

- アプリは 8MB のスタック（カーネルと共用）。いちばん下の 64KB は触ると例外になる「ガードページ」で、再帰が深すぎると `*** stack overflow` と出て止まる。
- **64KB より大きな局所変数（配列・VLA）は、ガードページを飛び越えて下のメモリを壊すことがある**（捕まえられない）。大きなものは malloc か static にする。
- コア 1〜3 で動く関数も、それぞれ 8MB のスタック（同じくガードページ付き）。

### 7.5 守られるもの・守られないもの

- 例外で止まるもの: NULL（0 番地のページ）、カーネルのコード・定数への書き込み、スタックのあふれ（ガードページ）、実機に RAM がない番地。
- 守られないもの: カーネルのデータ・ヒープ・ほかのアプリの領域への誤った書き込み（静かに壊れることがある）。アプリは EL1 なので、わざとなら何でもできる。おかしくなったら `reboot`。

---

## 8. 入出力とコンソール

### 8.1 出力

- `stdout`・`stderr` はバッファなしで、書いたらすぐ端末に出る（`fflush` は要らない）。`'\n'` は `"\r\n"` にして送る。
- 端末は USB シリアル（`/dev/ttyACM0`）・UART（3Mbps）・telnet のどれでもよく、同じ出力がすべてに出る。表示は xterm 互換のエスケープシーケンス（ANSI）で行う。
- 速さの目安（実機）: USB で 200KB の表示が約 0.05 秒、UART で約 0.5 秒。大量に出すと、その分待つ。
- シェルのリダイレクト（`>`、`>>`、`2>`、`2>&1`、`/dev/null`）とパイプは stdout をファイルに向ける。`stderr` はリダイレクトしなければいつも画面。`isatty(1)` で向け先がわかる（stderr は `isatty(2)`。2026-11-21 から、`2>`・`2>&1`・ssh のコマンドの間は 0。それまではいつも 1 だった）。
- `/dev/null` はシェルのリダイレクトだけ。アプリから `fopen("/dev/null")` はできない。
- 複数のコアから同時に出力すると、文字の単位で混ざることがある。

### 8.2 入力（Linux の端末と同じ。termios）

2026-11-12 から、アプリの stdin（端末から読むとき）は Linux の端末の既定と同じに動く（POSIX の termios。`os/libc/termios.c`）。

- **既定（`ICANON`・`ECHO`・`ICRNL`・`ISIG`）**: 打った文字を表示し、Backspace（127。^H も）・Ctrl-U（行を消す）・Ctrl-W（単語を消す）で直せる。Enter で 1 行が確定し、`'\n'` 付きで届く。行の頭の Ctrl-D は EOF（途中なら、そこまでを改行なしで渡す）。**`fgets`・`scanf`・`getline` がそのまま使える**。日本語（UTF-8）も 1 文字ずつ消せる（全角は 2 桁分）。矢印キーなどは `^[[A` のように表示され、行にそのまま入る（Linux と同じ）。
- **キーを 1 つずつ受け取る（ゲーム、TUI など）**: Linux と同じく `ICANON` と `ECHO` を外す:

```c
#include <termios.h>
#include <unistd.h>

struct termios saved, raw;
tcgetattr(STDIN_FILENO, &saved);
raw = saved;
raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);   // 1 文字ずつ、表示なし
raw.c_cc[VMIN] = 1;                          // 1 文字来るまで待つ（0 にすると待たない）
raw.c_cc[VTIME] = 0;                         // VMIN = 0 なら、待つ時間（0.1 秒単位）
tcsetattr(STDIN_FILENO, TCSANOW, &raw);
int c = getchar();                           // Enter は '\n'（ICRNL を外すと '\r'）。矢印は ESC [ A〜D
tcsetattr(STDIN_FILENO, TCSANOW, &saved);    // 戻す（戻さずに終わっても、OS が既定に戻す）
```

- `ISIG` も外すと、Ctrl-C はキー（3）として届き、アプリを止めるのは Ctrl-\ になる。
- `VMIN`・`VTIME` は POSIX と同じ意味（`VMIN = 0, VTIME = 5` なら 0.5 秒待って、何も来なければ `read` は 0）。
- 出力の `OPOST`・`ONLCR`（`'\n'` を `"\r\n"` にする。既定はあり）も効く。
- 端末の設定は、アプリが終わると既定に戻る。前のアプリが読み残した入力（`scanf` が押し戻した文字など）も、次のアプリには渡さない。
- キー入力の値: Enter は `'\n'`（`ICRNL` を外すと `'\r'`）、Backspace は 127、Esc は 27、矢印は `ESC [ A`〜`D` の 3 文字、日本語は UTF-8 のバイト列。
- シェルのリダイレクト（`app < file`、パイプ）で stdin がファイルのときは、ファイルの中身がそのまま来て、終わりで EOF。`isatty(0)` で区別できる（端末の設定は効かない。`tcgetattr` は ENOTTY）。
- キー入力を待つ間、コア 0 は眠る（電気を使わない）。待たずに調べるなら `uart_has_input()`、または `VMIN = 0, VTIME = 0`。
- curses は端末を自分で扱う（`raw()`・`cbreak()`・`noecho()` など。termios とは別）。

### 8.3 端末の大きさ

Linux と同じく `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)`（`ws.ws_row`・`ws.ws_col`）か `tcgetwinsize`。中身は `monocs_term_size(&rows, &cols)` と同じで、端末に問い合わせる（`ESC [ 6 n`）。答えがなければ 24 行 80 桁で false。telnet なら、つないだ端末が知らせた大きさ（ウィンドウを変えると変わる）。USB シリアル・UART には大きさの変更を知らせる仕組みがない。

### 8.4 curses（端末の画面を描くアプリ）

`#include <curses.h>`（`ncurses.h`・`ncursesw/curses.h` も同じ）で、ncurses 互換の小さな curses が使える。定数（キー・属性・マウス）の値は ncurses 6 と同じ。

- Enter は `'\n'` で届く（ncurses の既定の `nl()`。`nonl()` を呼ぶと `'\r'`。2026-11-14 から。それまではいつも `'\r'`）。
- `def_prog_mode`・`reset_prog_mode`（system() の前後。→ 6.4）。
- 使えるのは **stdscr だけ**（`newwin` などの窓はない）、**スクロール（scrollok）はしない**。
- UTF-8・全角、256 色・既定の色（-1）、罫線（ACS。枠の `box`・`border`・`wborder` と `hline`・`vline` も。2026-11-26）、マウス（押した・離した。クリック・ダブルクリックは作らない）、`define_key`、`ungetch`。
- `KEY_RESIZE` は telnet のときだけ届く。
- `raw()` の間は Ctrl-C がキーになり、Ctrl-\ でアプリが止まる。
- endwin を呼ばずに終わっても（例外・Ctrl-\ でも）、OS が端末を元に戻す。
- 例: `samples/curses_demo.c`、`/usr/bin/less.c`、エディタ mi（`/root/apps/mi`）。
- サンドボックス（`sandbox FILE.c`。docs/sandbox.md）の中でも、同じ curses が使える（OS の curses.c をそのまま写したもの）。サンドボックスの中では、Ctrl-C はキーとして届かず、いつも止める。

---

## 9. ファイル

### 9.1 FAT32 の性質

- **大文字と小文字を区別しない**（`/Hello.TXT` と `/hello.txt` は同じファイル）。表示は作ったときの名前。
- 長い名前（255 文字まで）・日本語の名前は使える。パーミッション・所有者・シンボリックリンク・ハードリンクはない。`stat` の `st_mode` はいつも 0644（ディレクトリは 0755）。`chmod`・`chown` は、ファイルがあれば何もせずに成功する（Linux の vfat の quiet と同じ。読み出し専用の属性は `fatattr`）。
- `stat` の `st_ino` は、絶対パス（小文字にそろえたもの）のハッシュ（`st_dev` は 1）。同じファイルなら同じ、違うファイルならまず違う番号（SQLite はこれで同じファイルかを決める）。名前を変えると番号も変わる（Linux と違う）。
- ファイルの終わりより先への `lseek` は、大きさを変えない（POSIX と同じ）。そこで `write` すると、間は 0 になる。`ftruncate` で長くしたところも 0。
- `fcntl` のレコードのロック（`F_SETLK`・`F_SETLKW`・`F_GETLK`）は、いつも取れる（`F_GETLK` は `F_UNLCK`）。POSIX のロックはプロセスごとで、同じプロセスのロックはぶつからない。Monocs のアプリは 1 つなので、ぶつかる相手がいない。
- `fsync` は、そのファイルとたまっている書き込みを、SD カードに書き出してから戻る（下の 9.3 の 1 秒を待たない）。
- 日時は 2 秒単位、地方時（TZ）。日時が合う前に書いたファイルは 2026-01-01 00:00 になる。
- 同時に開けるファイルは、ソケット・パイプと合わせて 253 個（fd 3〜255。Linux と同じく、空いているいちばん小さい番号から。アプリの FD_SETSIZE は Linux と同じ 1024）、ディレクトリは 32 個。超えると開けない（errno は EMFILE）。シェルのリダイレクト（`> file`）は、向け先のファイルに fd を 1 つ使う（Linux は dup2 で fd 1 に付け替えるので使わない）。
- `rename` は POSIX と同じく、行き先のファイルを置き換える（安全に書き換えるなら「`PATH.tmp` に書いて `rename`」）。

### 9.2 読み出しのキャッシュ

- **読み出し専用で開いたファイル（4MB まで）は、最初に全体を読んでメモリに置く**（合計 64MB まで）。2 回目からは SD カードを読まない。最初の open は、ファイル全体を読む分だけ時間がかかる。
- ほかの fd で書いたものは、読み出し専用で開いている fd からも見える（Linux と同じ。2026-11-20 から。書いた fd の write・close・fsync・ftruncate の後、読む側の次の read・lseek・fstat で読み直す）。書き込みで開く・削除・名前の変更で、そのファイルのキャッシュは捨てる。削除・名前の変更の前に開いていた fd は、前の中身のまま読める。
- `O_APPEND` で開いた fd の write は、いつもファイルの終わりに書く（POSIX と同じ。前は開いたときだけ終わりに移していた）。
- ないファイルを探した結果も覚えている（同じものを何度探しても速い）。
- SD カードを PC で書き換えたら、Monocs を再起動すること（キャッシュが古いまま残るため）。

### 9.3 書き込み

- 書き込みはいったんメモリにためて、**最初の書き込みから約 1 秒後**（またはファイル操作のついで、シェルの入力待ち、アプリの終わり、`sync`）にまとめて SD カードに書く。**電源が切れると、書き出す前の分（最大 約 1 秒）は失われる**。大事なものを書いたら、アプリの終わりを待つか、シェルで `sync`。電源を切るときは、シェルで `poweroff`（書き出して止まる。電源を入れ直すと起動する）。
- 速さの目安（実機）: 小さなファイルの作成は 1 つ 0.1ms ほど、大きなファイルの書き込みは数 MB/s。

### 9.3.1 止まったとき・固まったとき（2026-11-20）

- カーネルが止まると（カーネルの中の例外など）、表示の 5 秒後に自分で再起動する。割り込みが止まって固まると、ウォッチドッグが約 16 秒で再起動する。アプリの例外・Ctrl-C は今までどおりアプリだけが止まる。
- 次の起動の `dmesg` の頭に「previous boot: …」（前の起動の終わり方）。ふつうに終わらなかったときは、止まる直前の記録が `/var/log/dmesg.crash` に残る（RAM に残っていたもの。電源を切ると消える）。
- 止まった様子を UART やデバッガで見たいときは、先にシェルで `watchdog off`。

### 9.4 SD カードを直接

`monocs_disk_read/write` は区画をそのまま読み書きする（FAT を壊しうる。HTTP のサービスの区画の取り出し・書き戻しのためのもの）。普通のアプリは使わない。

---

## 10. マルチコア

```c
#include <stdio.h>
#include <monocs.h>

static void work(void *arg) { int *n = arg; *n = 42; }

int main(void)
{
    int result = 0;
    if (monocs_cores() > 1 && monocs_core_start(1, work, &result))
        monocs_core_wait(1);        // 例外・中断なら false
    printf("%d\n", result);
    return 0;
}
```

- `monocs_cores()` は使えるコアの数（コア 0 を含む）。サービスが動いているとコア 3 はサービス用なので 3。
- `monocs_core_start(core, fn, arg)` でコア 1〜3 に関数を任せ、`monocs_core_wait` で待つ。まだ動いているコアや、サービス用に取ってあるコア 3 に頼むと、待たずに false を返す（任せられなかったら、自分で計算するなどの代わりを用意する）。
- ほかのコアの関数からも printf・malloc・ファイル操作を呼べる（C ライブラリと VFS の中はロックで守っている）。errno、strtok・rand などの状態はコアごと。
- 自分のデータを守るロックは `monocs_mutex`（`static monocs_mutex m = MONOCS_MUTEX_INIT;`、`monocs_mutex_lock(&m)`、`monocs_mutex_unlock(&m)`）。C11 のアトミック（`<stdatomic.h>`）も使える。
- アプリが終わると（Ctrl-C・例外でも）、ほかのコアの関数は止められる。
- ほかのコアで例外が起きたら、内容を表示してそのコアだけ止まり、`monocs_core_wait` が false を返す。
- 2 つのコアが同じ 64 バイトのキャッシュラインに書くと遅くなる（フォールス・シェアリング）。コアごとのデータは 64 バイトずつ離して置く。
- 例: `samples/primes_mc.c`（4 コアで約 3.3 倍）、`samples/libc_mc.c`。

### 10.1 pthread（2026-11-15 から。`#include <pthread.h>`）

Linux と同じ書き方で、空いたコアを使える。スケジューラはないので、**スレッド 1 つに空いたコアを 1 つまるごと渡す**。
- 同時に動かせるスレッドは、空いたコアの数（サービスがあるとコア 1・2 の 2 つ）まで。それより多く作ると `pthread_create` が `EAGAIN`。終わったスレッドのコアは、また使える。`sysconf(_SC_NPROCESSORS_ONLN)`・`get_nprocs()`（`<sys/sysinfo.h>`）はコア 0 を含むコアの数（3）なので、「コアの数 − 1 本のスレッド + main」で分けるとちょうど埋まる（例: `samples/primes_pthread.c`。実機で 3 スレッド 497ms、`primes_mc.c` と同じ）。「コアの数だけスレッドを作って main は待つ」プログラムは、最後の 1 本が EAGAIN になる。
- ある: `pthread_create`・`join`・`detach`・`exit`・`self`・`equal`・`setname_np`、attr（detachstate。stacksize は覚えるだけで、スタックはコアごとに 8MB）、mutex（NORMAL・RECURSIVE・ERRORCHECK、trylock・timedlock）、条件変数（timedwait。CLOCK_REALTIME・CLOCK_MONOTONIC）、`pthread_once`、key（`pthread_key_create`・`setspecific`、スレッドの終わりに destructor）、読み書きのロック、バリア、スピンロック、`sched_yield`、`pthread_sigmask`・`pthread_kill`。ない: `pthread_cancel`（ENOSYS）、優先度、アプリが定義する `_Thread_local`・`__thread` の変数（→ 4.2）。
- 待つ関数（mutex・条件変数・join など）は、コアを wfe で眠らせて待つ。条件変数の signal は、待っているもの全部を起こす（POSIX が認める「余分に起きる」）。
- スレッドの中の `exit()`・例外（NULL への書き込みなど）は、アプリ全体を終わらせる（Linux と同じ。例外なら `$?` は 139）。main から戻ると、動いているスレッドは止まる。シグナル（Ctrl-C など）は main のスレッドに届く。
- スレッドの中では system()・popen・curses・`monocs_core_start` は使えない（コア 0 のもの）。サービスからはスレッドを作れない。
- 型の中身は glibc と違う（ソースの互換だけ）。`_POSIX_THREADS` が定義される。

### 10.2 OpenMP（2026-11-15 から。`-fopenmp`）

GCC と同じく `-fopenmp` を付けたときだけ `#pragma omp` が効き、`_OPENMP`（201511）が定義される。付け方: `cc -fopenmp -o PROG FILE.c`、Makefile の `CFLAGS` に `-fopenmp`。関数は `#include <omp.h>`。
- 中身はコンパイラ（slimcc）が `#pragma omp parallel` の文を関数に切り出し、pthread（10.1）でチームを作って動かす。チームは「空いたコアの数 + 1」（サービスがあれば 3）まで（`omp_get_max_threads`。`OMP_NUM_THREADS`・`omp_set_num_threads`・`num_threads(n)` で減らせる）。入れ子の parallel は 1 人のチーム。
- 使える指示: `parallel`・`for`・`parallel for`（`simd` は付けてもよい）、`critical`・`atomic`（どちらも 1 つのロック）、`barrier`、`single`、`master`・`masked`、`flush`、`task`（その場で動かす）・`taskwait`。`simd`・`declare`・`ordered` は読み捨てる。
- 使える節: `num_threads`、`if`、`private`・`lastprivate`（private と同じ）、`firstprivate`、`shared`・`default`（変数は既定で共有）、`reduction`（+ - * & | ^ && || max min）、`schedule(static[,n])`・`schedule(dynamic[,n])`（guided・auto・runtime は dynamic）、`nowait`、`collapse`（外側の繰り返しだけを分ける）。
- `omp for` の繰り返しは `for (int i = A; i < B; i++)` の形（比べるのは < <= > >= !=、増やし方は ++ -- += -= `i = i + s`）。繰り返しの変数は整数（ポインタは使えない）。
- ない: `sections`、`threadprivate`、`copyin`、`ordered` の中身、`task` の依存、`#pragma omp` の行の中のマクロの展開、parallel の外の関数の中の VLA を中で使うこと。
- 例: `samples/primes_omp.c`（`#pragma omp parallel for reduction(+ : count) schedule(dynamic, 1000)`。実機で 3 スレッド 391ms、1 スレッドの 3.0 倍。pthread で均等に分けた primes_pthread.c の 497ms より速い）。

---

## 11. ネットワーク

- つながる相手は、USB でつないだ Pi 5（192.168.7.1。Monocs は 192.168.7.2）と、内蔵 Wi-Fi（2.4GHz、WPA2。アドレスは DHCP）の先（家の LAN とインターネット）。IPv4 だけ。Wi-Fi がつながっていれば名前（DNS）も使える（下）。外から Monocs へは、USB からはそのまま、Wi-Fi からはパスワードを確かめて受ける（HTTP・telnet のサービス。パスワードは Monocs の `passwd` で決める。決めるまでは Wi-Fi からは断る）。暗号化（TLS・SSH）はないので、中身は Wi-Fi の中をそのまま流れる。
- Linux と同じソケットが使える（→ 11.1）。下は Monocs 独自の、待つ形の API（`timeout_ms`: 0 = 待たない、負 = いつまでも）。短く書けるので、Monocs のためだけのプログラムには便利。

```c
int h = monocs_tcp_connect("192.168.7.1", 8000, 3000);     // つなぐ（3 秒まで待つ）
if (h >= 0) {
    const char *req = "GET / HTTP/1.0\r\n\r\n";
    monocs_tcp_send(h, req, (int)strlen(req), -1);         // 長さは数字で書かない（文字列を変えたときにずれる）
    char buf[1024];
    int n;
    while ((n = monocs_tcp_recv(h, buf, sizeof buf, 5000)) > 0)   // 0 = 相手が閉じた、-1 = 時間切れ、-2 = エラー
        fwrite(buf, 1, n, stdout);
    monocs_tcp_close(h);
}
```

- TCP: 接続は全部で 16 個まで、接続ごとに 64KB の受信バッファ。待ち受けは `monocs_tcp_listen(port)` → `monocs_tcp_accept(l, timeout)`。`monocs_tcp_listen` は USB と Wi-Fi のどちらからも受ける。**認証のないサーバーは `monocs_tcp_listen_addr("192.168.7.2", port)`（USB から届いたものだけ）を使う**。Wi-Fi からも受けるサーバーは、受け付けた接続ごとに `monocs_tcp_netif(h)`（`MONOCS_NETIF_USB`・`MONOCS_NETIF_WIFI`）を見て、Wi-Fi からのものはパスワードを確かめる（`#include <monocs_auth.h>`: `/etc/htdigest` を読む `monocs_auth_load`、打たれたものから同じ値を作る `monocs_auth_ha1`、比べる `monocs_auth_equal`、MD5。例は `services/telnetd.c` のログインと `services/httpd.c` のダイジェスト認証）。コンソールにつなぐサービス（`monocs_console_open` など）は、`monocs_console_hangup()` が true を返したら（シェルで `exit` が打たれた）、残りの出力を送り切ってから接続を閉じる。相手のアドレスは `monocs_tcp_peer(h, ip, &port)`。
- 内蔵 Wi-Fi と名前（DNS）: `/etc/wpa_supplicant.conf`（Linux と同じ形）にネットワークを書いておくと、起動時にサービス wifid が自動でつなぎ（約 10 秒）、切れたらつなぎ直す。様子はシェルの `wifi status`。つながっていれば、`monocs_tcp_connect("example.com", 80, 5000)` のように名前でつなげる（待つのは timeout まで）。名前だけ引くなら `monocs_resolve(name, ip, sizeof ip, timeout_ms)`（0 で成功。ip は 16 バイト以上）、シェルでは `host 名前`。`monocs_udp_sendto` に名前を渡したときは待たない（まだ引けていなければ -1。引けたら覚えているので、少し後にもう一度送る。待ちたいなら先に `monocs_resolve`）。USB だけ（Wi-Fi なし）では名前は引けない。
- HTTPS・TLS: シェルでは `curl https://...`（`-o FILE`・`-L`・`-I`・`-s`・`-f` など。Linux の curl に似せた小さなもの）。アプリからは共有のライブラリ `-ltls`（`cc -ltls`）と `#include <monocs_tls.h>`: `monocs_tls_connect(host, 443, timeout_ms)`（NULL なら `monocs_tls_error()`）→ `monocs_tls_send`・`monocs_tls_recv`（戻り値は `monocs_tcp_recv` と同じ）→ `monocs_tls_close`。相手の証明書を `/etc/ssl/certs/ca-certificates.crt` で確かめ、名前と期限も見る（時計が合っていること）。TLS 1.2 まで。中身は BearSSL（`/usr/lib/bearssl`。直接使うなら `-lbearssl` と `#include <inc/bearssl.h>`。入っているのは TLS のクライアントに要る 86 ファイルだけ）。使うアプリは、最初の 1 回（とカーネルを更新した直後）にライブラリのコンパイルで 6〜9 秒かかり、その後は約 1 秒で始まる。例は `bin/curl/curl.c`。
- `/dev/null` はファイルとして開ける（`fopen("/dev/null", "w")`。書いたものを捨て、読むとすぐ終わり）。
- 圧縮されたもの: シェルでは `gunzip FILE.gz`・`zcat`・`tar xf FILE.tar.gz`（`tar tf` で一覧、`curl -sL URL | tar xz` も）、`curl --compressed`。アプリからは共有のライブラリ `-linflate` と `#include <monocs_inflate.h>`: `monocs_inflate(MONOCS_INFLATE_GZIP, 読む関数, ctx, 書く関数, ctx, &err)`（gzip・zlib・DEFLATE だけ・自動。入力も出力も少しずつでよい）。圧縮する側（gzip、tar の作成）はない。
- 乱数: 当てられては困るもの（鍵・nonce）は `getentropy(buf, len)`（`<unistd.h>`。256 バイトまで。ハードウェアの乱数生成器）。`rand()` は当てられる。
- UDP: ソケット 8 個、それぞれ受け取って未読のデータグラムは 8 個まで（あふれたら捨てる）、1 つ 1472 バイトまで。

### 11.1 Linux と同じソケット（2026-11-14）

`#include <sys/socket.h>`・`<netinet/in.h>`・`<netinet/tcp.h>`・`<arpa/inet.h>`・`<netdb.h>`・`<poll.h>`・`<sys/select.h>` で、Linux のネットワークのプログラムがそのままコンパイルできる（構造体の並び・定数の値は Linux の glibc と同じ。中身は上の Monocs の API）。

- 使えるもの: `socket`（`AF_INET` の `SOCK_STREAM`・`SOCK_DGRAM`、`SOCK_NONBLOCK`）、`bind`・`listen`・`accept`・`accept4`・`connect`・`send`・`recv`・`sendto`・`recvfrom`・`shutdown`・`close`、`read`・`write`、`getsockname`・`getpeername`、`setsockopt`・`getsockopt`（`SO_RCVTIMEO`・`SO_SNDTIMEO` は効く。`SO_ERROR`・`SO_TYPE` を返す。`SO_REUSEADDR`・`TCP_NODELAY` などは受け取るだけ）、`fcntl(F_SETFL, O_NONBLOCK)`・`ioctl(FIONBIO / FIONREAD)`。
- 名前とアドレス: `getaddrinfo`・`freeaddrinfo`・`gai_strerror`（`ai_socktype` が 0 なら TCP・UDP・RAW の 3 つ。glibc と同じ）、`getnameinfo`（逆引きはせず数字を返す）、`gethostbyname`、`getservbyname`（よく使うサービスだけ）、`inet_pton`・`inet_ntop`・`inet_aton`・`inet_addr`・`inet_ntoa`、`htons` など、`gethostname`（"monocs"）。
- まとめて待つ: `select`・`pselect`・`poll`。キー入力（fd 0）・ソケット・ファイルをまとめて待てる（待つ間は眠る）。fd 0 は、termios の行の入力（ICANON）でも 1 文字届けば「読める」とする（Linux は行が終わるまで読めない）。
- ソケットの fd は 16 から（ファイルは 3〜15）。アプリが閉じ忘れたものは、アプリの終わりに閉じる。
- `bind(INADDR_ANY)` は USB と Wi-Fi のどちらからも受ける（Linux と同じ）。**Wi-Fi の LAN に守りなしで開く**ので、USB からだけ受けたいサーバーは `192.168.7.2` に bind する。
- 待つ操作は、待たない形（`O_NONBLOCK`・`MSG_DONTWAIT`）なら EAGAIN（connect は EINPROGRESS）。待つ間に SA_RESTART のないシグナルの関数が呼ばれたら EINTR。つながっていない TCP に送ると EPIPE と SIGPIPE（`MSG_NOSIGNAL` がなければ。Linux と同じ）。
- `alarm` も使える（SIGALRM。ほかのシグナルと同じく、アプリが待っているところで届く）。
- ないもの: IPv6（`AF_INET6` のソケットは EAFNOSUPPORT）、Unix ドメインのソケット、`SOCK_RAW`、マルチキャスト、`sendmsg`・`recvmsg`、`dup`・`dup2`、127.0.0.1（ループバック。つなぐと ECONNREFUSED）。数の上限は上の Monocs の API と同じ（TCP 16・UDP 8。サービスと分け合う）。
- Monocs のコンパイラ（slimcc）は `__builtin_bswap32` などを知らないので、`<endian.h>` の `be32toh` などは使えない（`htonl` などは Monocs のヘッダーが自分で書いているので使える）。
- 確かめ方: `sh tools/test/netapi.sh`（相手のいらないもの。実機で Linux と出力を比べる）、`python3 tools/test/sockets/run.py example.com`（Pi 5 を相手に TCP・UDP・select のサーバー・DNS と HTTP）。
- 使われているポート: 80（HTTP のサービス）、23（telnet のサービス）、22（SSH のサービス。`ssh ユーザー名@アドレス`。USB からはパスワードなし、Wi-Fi からは `passwd` のパスワードか `/root/.ssh/authorized_keys` の公開鍵（ECDSA・RSA））、7（カーネルの TCP エコー）。123（SNTP）は問い合わせる側。
- アプリが終わると、開いたままの接続は閉じる。
- 速さの目安（実機）: TCP で片方向 約 15〜18MB/s。
- 例: `samples/tcp_echo.c`（サーバー）、`samples/http_get.c`（クライアント）。

---

## 12. 時刻と待ち

- `time()`・`clock_gettime(CLOCK_REALTIME)` は日時。Pi 5 から合わせてもらうまでは 2026-01-01 00:00 UTC から数える（`monocs_clock_known()` で合ったかわかる）。合わせるのは SNTP のサービス（起動の直後と 1 時間ごと。Pi 5 に聞き、答えがなければ Wi-Fi の先の pool.ntp.org）と、nos.py がつなぐたび。
- `clock_gettime(CLOCK_MONOTONIC)`・`timer_now_us()` は起動からの時間（マイクロ秒の精度。戻らない）。時間を測るのはこちら。
- `localtime` は TZ（既定は JST、`date -z +0900` で変える）。
- 待つ: `nanosleep`・`timer_delay_us(us)`。コア 0 では 10ms（1 ティック）より長い分は眠り、残りは回って待つ。コア 1〜3 ではずっと回って待つ。
- タイマーの割り込みは 100Hz（`timer_ticks()`）。

---

## 13. サービス（裏で動き続けるプログラム）

アプリの実行中も動き続けるもの（HTTP サーバーなど）は、サービスとして書く。コア 3 で、すべてのサービスを順に呼び続ける（協調型）。

```c
#include <monocs.h>

int service_start(int argc, char **argv)   // 始めるとき（0 で成功。なくてもよい）
{
    return 0;
}

int service_poll(void)                     // 何度も呼ばれる。待たずにすぐ戻る。仕事をしたら 0 以外
{
    return 0;
}

void service_stop(void)                    // 止めるとき（なくてもよい）
{
}
```

- `main` の代わりに上の 3 つを書く。`svc start FILE.c [引数]`（またはフォルダ）で始め、`svc stop 名前`（ファイル名から .c を除いたもの）で止める、`svc` で一覧。起動時に始めるものは `/usr/sbin.txt` に 1 行ずつ（`/usr/sbin/httpd.c` のように。引数も書ける。`#` で始まる行は飛ばす）。
- **`service_poll` の中で待たない**（ほかのサービスが止まる）。TCP・UDP は `timeout_ms` を 0 にして使う。
- サービスのメモリは、止めるまで残る（アプリのように捨てられない）。free を忘れると、そのまま減っていく。
- サービスが開いたファイル・接続は、そのサービスを止めたときに閉じる（アプリの終わりでは閉じない）。
- サービスで例外が起きると、すべてのサービスが止まる（シェルの入力待ちのときに気づいて後始末する）。
- サービスの出力（printf）は画面に出るが、シェルの行に割り込むので、ふだんは出さない。
- 例: `services/httpd.c`、`services/telnetd.c`、`services/sntp.c`、`services/wifid.c`（内蔵 Wi-Fi に自動でつなぐ）。

---

## 14. ハードウェアを直接触る

- アプリも EL1 なので、周辺機器のレジスタ（`0x3F000000` から。GPIO は `0x3F200000`）を直接読み書きできる。`volatile` のポインタで 32 ビットずつ触る。
- OS が使っているもの（UART、USB、SD、タイマー、割り込み、メールボックス、LED）は触らないこと。壊れたら `reboot`（効かなければ電源を入れ直す）。
- LED は `led_set(on)` を使う（アプリが終わると OS の点滅に戻る）。
- **GPIO** は共有のライブラリ `/usr/lib/gpio`（`-lgpio`、`#include <monocs_gpio.h>`。2026-11-16）を使う:
  - `gpio_mode(pin, GPIO_IN / GPIO_OUT / GPIO_ALT0〜5)`、`gpio_pull(pin, GPIO_PULL_OFF / DOWN / UP)`、`gpio_write(pin, 0/1)`、`gpio_read(pin)`、`gpio_toggle`、`gpio_read_all()`・`gpio_write_mask(mask, value)`（GPIO0〜31 をまとめて）、`gpio_get_mode`、`gpio_pin_ok`。
  - `gpio_wait_edge(pin, GPIO_EDGE_RISING / FALLING / BOTH, timeout_ms)`: 起きたエッジ（RISING・FALLING）か 0（時間切れ）。ピンのエッジの印（GPEDS）で拾うので、待つ間の短いパルスも落とさない。待つ間は CPU を使い続ける（Ctrl-C で止まる）。割り込みは使わない。
  - 番号は BCM の GPIO の番号（ヘッダーの 11 番 = GPIO17。`pinctrl -p get` で対応がわかる）。変えられるのは GPIO0〜27 のうち、コンソールの UART（14・15）を除くもの（ほかは -1、errno は EBUSY・EINVAL）。読むのは 0〜53 のどれでも。
  - 設定はアプリが終わっても残る（Linux の pinctrl・pigpio と同じ）。プルは読み出せない（BCM2837 にその仕組みがない）。電源を入れたときは全部入力で、GPIO0〜8 はプルアップ、9〜27 はプルダウン。
  - 例: `samples/gpio_blink.c`（`cc -o blink -lgpio /usr/share/samples/gpio_blink.c && ./blink`。LED の点滅、ボタンで止める）。3.3V、ピンあたり 16mA まで。5V をつながない。
  - シェルの **`pinctrl`**（/usr/bin。Raspberry Pi OS の pinctrl と同じ書き方）: `pinctrl get [GPIO]`、`pinctrl set 17 op dh`、`pinctrl set 27 ip pu`、`pinctrl lev 27`、`pinctrl funcs 18`、`pinctrl poll 27`（変わるたびに 1 行）、`-p`（ヘッダーのピンの番号）、`-e`（set の後に状態を出す）。プルの欄はいつも `--`。Pi 5 からは `nos.py cmd 'pinctrl set 17 op dh'`。
- **I2C** は共有のライブラリ `/usr/lib/i2c`（`-li2c`、`#include <monocs_i2c.h>`。2026-11-16。中で /usr/lib/gpio を使う）:
  - バス 1 = GPIO2（SDA、ヘッダーの 3 番）・GPIO3（SCL、5 番）。基板に 1.8kΩ のプルアップがある。バス 0 = GPIO0・1（27・28 番。HAT の ID の EEPROM 用で、基板のプルアップがない。外に付ける）。
  - `i2c_open(bus, hz)`（省くと最初に使うときに 100kHz で開く）、`i2c_read_reg(bus, addr, reg)`・`i2c_write_reg`・`i2c_read_regs(bus, addr, reg, buf, len)`・`i2c_write_regs`、`i2c_write`・`i2c_read`、`i2c_write_read`（リピーテッドスタート。書くのは 16 バイトまで）、`i2c_probe`、`i2c_close`。失敗は -1 で、errno は ENXIO（応答がない）・ETIMEDOUT・EINVAL・EIO。
  - BCM2835 の I2C は、相手のクロックストレッチ（SCL の引き伸ばし）を正しく扱えない（ハードウェアの誤り。Linux の Raspberry Pi でも同じ）。そういう機器は遅いクロックで。
  - アプリが `monocs_gpio.h` も読むなら `-lgpio` も書く（ライブラリが使うライブラリのヘッダーは、アプリには見えない）。
  - シェルの **`i2cdetect`・`i2cget`・`i2cset`**（/usr/bin。Linux の i2c-tools と同じ書き方・出力）: `i2cdetect -y 1`、`i2cget -y 1 0x76 0xd0`、`i2cset -y 1 0x76 0xf4 0x27`。i2cdetect はどのアドレスも 1 バイト読んで調べる（`-r` と同じ。`-q` はない）。SMBus block（s）・PEC はない。
- **SPI** は共有のライブラリ `/usr/lib/spi`（`-lspi`、`#include <monocs_spi.h>`。2026-11-16。中で /usr/lib/gpio を使う）:
  - SPI0: GPIO11（SCLK、ヘッダーの 23 番）・10（MOSI、19 番）・9（MISO、21 番）・8（CE0、24 番）・7（CE1、26 番）。補助の SPI1・2 はない。
  - `spi_open(cs, hz, mode)`（cs は 0・1。省くと最初に使うときに 1MHz・モード 0）、`spi_transfer(cs, tx, rx, len)`（全二重。tx が NULL なら 0x00 を送る、rx が NULL なら捨てる）、`spi_write`・`spi_read`、`spi_write_read(cs, tx, txlen, rx, rxlen)`（送ってから受ける。その間 CS はつけたまま）、`spi_get_hz`、`spi_close`。
  - mode は Linux の spidev と同じ値（`SPI_MODE_0`〜`3`、`SPI_CS_HIGH`、`SPI_NO_CS`（CE のピンを使わない。ほかの GPIO を CS にするなら自分で動かす））。8 ビット、上位のビットが先。速さは 400MHz の偶数分の 1（近い遅いほう）。
  - シェルの **`spi`**（/usr/bin。Monocs のコマンド）: `spi 9f -r 3`（0x9F を送って 3 バイト受け、受けたもの全部を 16 進で出す）。`-c 0|1`・`-s HZ`・`-m 0〜3`・`-H`（CS が High でつく）・`-N`。バイトは 16 進。
- **UART**（2026-11-16）: Zero 2 W の本物の UART は、ヘッダーに出せるのが GPIO14・15 だけで、Monocs のコンソールが使っている。そのため、ソフトウェアの UART の共有のライブラリ `/usr/lib/softuart`（`-lsoftuart`、`#include <monocs_softuart.h>`。中で /usr/lib/gpio を使う）を使う:
  - `int u = softuart_open(tx_pin, rx_pin, baud)`（好きな GPIO。-1 で使わない。8N1、300〜115200bps）、`softuart_write`・`softuart_putc`、`softuart_read(u, buf, len, timeout_ms)`（届いた分。0 = 時間切れ）・`softuart_getc`・`softuart_available`、`softuart_errors`（フレームの誤り・あふれ）、`softuart_close`。4 つまで。
  - 受信は空いたコア（1〜3）を 1 つ使って裏で受け続ける（4KB の輪）。空いたコアがなければ、read・getc を呼んでいる間だけ受ける（`softuart_rx_background` でわかる）。pthread・OpenMP・`monocs_core_start` とコアを取り合うことに注意。
  - 送信は呼んだコアで行い、1 バイトの間は割り込みを止める（9600bps で約 1ms）。
  - TX と RX を同じピンにすると、送ったものを自分で受ける（線なしの試験）。電圧は 3.3V（5V の機器の TX を直接つながない）。
  - シェルの **`softuart`**（/usr/bin。Monocs のコマンド）: `softuart [-b BAUD] TX RX`（端末のように使う。Ctrl-] で終わる）、`softuart -b 115200 -x "AT\r\n" -w 500 17 27`（送って、返ってきたものを出して終わる。Pi 5 から nos.py で使える）。
- **PWM** は共有のライブラリ `/usr/lib/pwm`（`-lpwm`、`#include <monocs_pwm.h>`。2026-11-16。中で /usr/lib/gpio を使う）。ハードウェアが波を出し続ける（CPU を使わない）:
  - ピン: チャンネル 0 = GPIO12（ヘッダーの 32 番）・18（12 番）、チャンネル 1 = GPIO13（33 番）・19（35 番）。同じチャンネルの 2 本は同じ波になる。
  - `pwm_set(pin, freq_hz, duty)`（duty は 0.0〜1.0。1Hz〜4.8MHz）、`pwm_set_ns(pin, period_ns, duty_ns)`（Linux の /sys/class/pwm と同じ形）、`pwm_servo(pin, pulse_us)`（50Hz）、`pwm_set_polarity`、`pwm_off`（ピンは休みの値の出力にする）、`pwm_get`（今の設定をレジスタから）。
  - PWM のクロックは 9.6MHz（水晶 19.2MHz の 1/2）に決める。細かさは 9.6MHz ÷ 周波数 段（1kHz で 9600 段、20kHz で 480 段）。
  - **アプリが終わっても波は止まらない**（`pwm_off` か、シェルの `pwm PIN off` で止める）。
  - 例: `samples/servo.c`（サーボ SG90: `cc -o servo -lpwm /usr/share/samples/servo.c && ./servo` で端から端へ。つなぎ方はファイルの頭）。
  - シェルの **`pwm`**（/usr/bin。Monocs のコマンド）: `pwm`（今の設定）、`pwm 18 1000 25`（1kHz・25%）、`pwm 18 servo 1500`、`pwm 18 invert on`、`pwm 18 off`。
  - ほかのピンで PWM が要るときは、GPIO をソフトウェアで切り替える（ライブラリはない）。
- 起動に関係ないドライバは、カーネルに入れずに C のソースとして置く。
  - I2C・SPI のセンサー、ボタン・LED などの GPIO の部品のように、呼ばれたときにレジスタを読み書きするだけのものは、共有のライブラリ（`/usr/lib/名前`、`-l名前`。3.6）として書く。使うアプリと一緒にコンパイルされ、2 回目からはキャッシュで速い。
  - 音や HDMI の画面のように、常駐して割り込みを受けたり、アプリをまたいで状態を持ったりするものは `/drivers` に置く方針（仕組みはまだない。最初のそういうドライバを作るときに作る）。

---

## 15. 速くするコツ

- **時間を測る**: シェルの `time コマンド`（bash と同じく予約語でパイプライン全体に掛かる。`time -p` は POSIX の形）。real は経った時間、user はアプリが使えるコア（0〜2）が動いていた時間の合計（3 コアで動けば real より大きい）、sys はいつも 0（Monocs はアプリと OS を分けて数えない）。
- **様子を見る**: シェルの `top`（コアごとの忙しさ・割り込みの数・何が動いているか、メモリ、サービスごとの CPU と「仕事をした」回数）。`top -b -n 2` で文字だけ（2 回目が直近の値）。アプリからは `monocs_sysstat(&st)`（`monocs_sysstat.h`）。

- **まず測る**: シェルの `perf コマンド`（命令数、L1・L2 のキャッシュのアクセスとミスなど）、`perf record [-e cycles] [-g] コマンド`（どの関数で時間・ミスが多いか、呼び出し元も）。アプリを動かすときは、コンパイルを除いてアプリの実行中だけを数える。QEMU ではサイクルしか数えないので、実機で測る。
- **Cortex-A53 の性質**: 命令を順に実行する（アウトオブオーダーでない）。64 ビットの割り算は遅いので、`int`（32 ビット）で足りるなら `int` を使う。
- **slimcc の性質**: ベクトル化（NEON）をしない、レジスタに置ける変数に限りがある、インライン展開は小さな関数だけ。整数の計算は GCC -O2 と数 % しか違わない（primes 1071ms・GCC 1006ms、strhash 402ms・397ms）が、浮動小数点の重い計算では 1.5 倍ほどかかる（nbody 443ms・301ms。どれも Pi 5 で測った値）。内側のループは単純に書く。
- **`long double` は遅い**（ソフトウェアで計算。足し算 1 回が数百命令）。必要なときだけ。
- **キャッシュ**: L1 は 32KB、L2 は 512KB。それより大きなデータを何度もなめると L2 のミスが増える。
- **ファイル**: 小さな read/write を何度も呼ぶより、FILE（fread/fwrite）か大きめのバッファでまとめる。読み出し専用で開いたファイルは 2 回目からメモリから読める。
- **表示**: 大量の printf は端末の速さで待つ。必要な分だけ出す。

---

## 16. デバッグ

- **コンパイル・リンクのエラー**: 4.4。
- **例外**: `ESR`（原因）、`ELR`（止まった命令の番地）、`FAR`（触ろうとした番地）と、原因の推測（`(NULL pointer)`、`(write to read-only memory ...)`、`(stack guard page ...)`）を出し、続けて止まった場所を関数名で出す（`at outer+0x3c`、呼び出し元の `from main+0x6c`。OS の関数の中なら `strlen+0xc [kernel]`。2026-11-16 から）。小さな関数はインライン展開されて、呼び出し元の関数の中に見える。Ctrl-C で止めたときも同じ。記録（dmesg・`/var/log/dmesg`）にも止まった関数が残る。
- **止まらない・遅い**: Ctrl-C で止めると、止めた番地と呼び出し元の番地を出す。`perf record -e cycles` なら、アプリの関数名で、どこで時間を使っているかがわかる。
- **一時ファイル**: `tmpnam`・`tmpfile` は `/tmp` に作る（なければ作る。2026-11-16）。`tmpfile` のファイルは、FAT が開いたままのファイルを消せないので、アプリの終わりに消す。`L_tmpnam` は 20、`P_tmpdir` は `"/tmp"`（Linux と同じ）。
- **前の起動の記録**: `/var/log/dmesg`（今の起動）、`/var/log/dmesg.1`（前の起動。固まって再起動したときに、何が起きたかを見る。最後の 5 秒ほどは入らないことがある）。
- **メモリ**: `mem`（ヒープ、最後のアプリのヒープの大きさと free されなかった量、ファイルのキャッシュ、SD カードの読み書き）。
- **QEMU**（`make qemu`）: USB・ネットワークはない（`usb: not available`）、PMU はサイクルだけ、速さは実機と違う。動きの確認は QEMU でもよいが、速さ・ネットワーク・USB は実機で。

---

## 17. よくある落とし穴（チェックリスト）

- [ ] キーを 1 つずつ読みたいのに、既定のまま `getchar` している → Enter を押すまで届かない（行の単位）。`tcsetattr` で `ICANON`・`ECHO` を外す（8.2）。
- [ ] ヘッダーを `#include` していない関数がある → エラー。
- [ ] `fork`・`alarm`・`socket`・`aligned_alloc` などを使っている → `undefined symbol`。5.2。
- [ ] SIGINT の関数を登録して、待たずに計算し続けている → Ctrl-C が届かない。Ctrl-\ で止める（6.5）。
- [ ] POSIX の関数の宣言が見えない → `-D_DEFAULT_SOURCE` を `compile_flags.txt` に。
- [ ] `atexit` に後始末を任せている → Ctrl-C・例外で止まったときは呼ばれない（`main` から戻る・`exit` のときは呼ばれる）。
- [ ] 大きな配列を局所変数にしている（64KB 超） → malloc か static に。
- [ ] `_Thread_local` を使っている → 使えない。コアごとの配列（`data[4]` を `cpu` で引く）などにする。
- [ ] `char` を符号つきだと思っている → 符号なし。必要なら `signed char`。
- [ ] 2 つのファイルに同じグローバル（`int x;`）がある → `multiple definition`。片方を `extern` に。
- [ ] テスト用の main が同じフォルダにある → Makefile があればそれに従う。なければ `.runignore` に書く。
- [ ] ファイル名の大文字・小文字で区別しようとしている → FAT は区別しない。
- [ ] 書いたらすぐ電源を切る → 最大 1 秒分失われる。`sync`。
- [ ] `/dev/null` をアプリで開いている → 開けない。
- [ ] ホスト名でつなごうとしている → DNS はない。IP アドレスで。インターネットには出られない。
- [ ] サービスの `service_poll` の中で待っている → ほかのサービスが止まる。
- [ ] `__DATE__`・`__TIME__` を使っている → 毎回コンパイルし直すので起動が遅い。
- [ ] 同時に 13 個を超えるファイルを開いている → 開けない。
- [ ] 実行のたびに前の値が残ると思って static 変数を使っている → 毎回初期値から。ファイルに書く。

---

## 18. 開発の流れ

- **Pi 5 で書いて送る**: `nos.py run prog.c [引数]`（送って作って動かす）、`nos.py push`・`pushdir`（置く）、`nos.py cmd 'シェルのコマンド'`。HTTP でも送れる（`curl -T FILE http://192.168.7.2/root/apps/x.c`）。ssh なら `scp FILE monocs:/root/apps/`。
- **Monocs の上で書く**: `mi ファイル`。ssh・telnet でつなぐと、ウィンドウの大きさも伝わる。PC のエディタで直接書くなら `sshfs monocs:/ ~/monocs`。
- **Pi 5 のエディタ（clangd）**: フォルダの `compile_flags.txt` はそのまま clangd が読める。`-I` に Monocs のヘッダー（リポジトリの `os/api/include`、picolibc のヘッダー）を足すと補完が効く。
- **QEMU で試す**: `make sdimage` のイメージに入れて `make qemu`（または `tools/test/qdrive.py`）。
- **OS の側を変えたくなったら**（使える関数を増やすなど）: リポジトリの `CLAUDE.md` の作業ルールに従う（`make`、`tools/test/verify.sh`、実機は `tools/test/hwcheck.sh`）。

---

## 19. 参考にするファイル

| 知りたいこと | ファイル |
|---|---|
| 使える関数の一覧 | `os/api/api.def` |
| 独自の API の説明 | `os/api/include/monocs.h`、`os/api/include/curses.h` |
| サンプル | `samples/`（hello、primes_mc、libc_mc、tcp_echo、http_get、tui、curses_demo、crash） |
| 付いてくるアプリ（書き方の例） | `bin/`（ls、less、grep など） |
| サービスの例 | `services/`（httpd、telnetd、sntp、wifid） |
| 仕組みと経緯 | `CLAUDE.md` |
| 実機の手順 | `docs/hardware-test.md` |
