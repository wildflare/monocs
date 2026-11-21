# 4. シェル

Monocs のシェルは、bash（POSIX の sh）とほぼ同じ書き方ができます。プロンプトは今のフォルダの後に `> ` です。

```
/root> ls
apps
/root> cd /usr/share/samples
/usr/share/samples> run hello.c
```

## 4.1 行の編集

| キー | 動き |
|---|---|
| ← →、Ctrl-B・Ctrl-F | 1 文字動く |
| Alt-B・Alt-F | 1 語動く |
| Home・End、Ctrl-A・Ctrl-E | 行の頭・終わり |
| ↑ ↓、Ctrl-P・Ctrl-N | 履歴（`/root/.shell_history` に 200 行。電源を切っても残る） |
| Backspace・Delete・Ctrl-D | 1 文字消す（行の頭の Ctrl-D は、telnet・ssh なら接続を閉じる） |
| Ctrl-W・Alt-Backspace・Alt-D | 1 語消す |
| Ctrl-U・Ctrl-K | 頭まで・終わりまで消す |
| Ctrl-L | 画面を消す |
| Tab | 補完（最初の語はコマンド名、ほかはファイル名。大文字・小文字を区別しない。2 回で候補の一覧） |

- 空白で始めた行は履歴に残しません（bash の ignorespace と同じ）。
- 閉じていない行（`if` の途中、引用符が閉じていない、行の終わりが `\`・`|`・`&&`）を打つと、`> ` を出して続きを読みます。

## 4.2 アプリの実行中に使うキー

| キー | 動き |
|---|---|
| Ctrl-C | アプリを止める（アプリが `signal(SIGINT, …)` を登録していれば、その関数を呼ぶ）。終了の値は 130 |
| Ctrl-\ | いつもアプリを止める（Ctrl-C を受け取るアプリの逃げ道）。終了の値は 131 |
| Ctrl-S・Ctrl-Q | 流れている出力を止める・再開する |
| Ctrl-D（行の頭） | 標準入力の終わり（EOF） |

curses のアプリ（mi・less など）は Ctrl-C をキーとして受け取るので、止めたいときは Ctrl-\ です。

## 4.3 コマンドの探し方

1. **組み込みのコマンド**（cd、run、make、svc など。`help` で一覧）がいちばん先です。
2. 次に、環境変数 `PATH`（既定 `/root/apps:/usr/bin`）のフォルダを順に、`名前`（実行ファイルかフォルダ）か `名前.c` を探します。
   見つかったらその場でコンパイルして動かします（2 回目からはキャッシュ）。
3. `/` を含む名前（`./prog`、`/root/apps/mi`）は、そのパスを動かします。

`which 名前` で、どれが動くかがわかります。

## 4.4 書き方

### つなぐ・向ける

| 書き方 | 意味 |
|---|---|
| `a ; b`、改行 | 順に動かす |
| `a && b`、`a \|\| b` | a が成功したら・失敗したら b |
| `a \| b` | a の出力を b の入力に（一時ファイルを通す。a が終わってから b が動く） |
| `> F`、`>> F`、`< F` | 標準出力をファイルに（上書き・追記）、ファイルを標準入力に |
| `2> F`、`2>> F`、`2>&1` | 標準エラーを |
| `> /dev/null` | 捨てる |
| `<<WORD` … `WORD` | ヒアドキュメント（`<<-WORD`・`<<'WORD'` も） |
| `! a` | 成功・失敗を逆に |
| `time a` | かかった時間（real・user・sys）を出す |

`&`（裏で動かす）はありません。同時に動くアプリは 1 つだけだからです。

### 引用符と展開

| 書き方 | 意味 |
|---|---|
| `'…'` | そのまま |
| `"…"` | 中の `$` だけ展開する |
| `\x` | 次の 1 文字をそのまま |
| `$NAME`、`${NAME}` | 変数 |
| `$?` | 直前のコマンドの終了の値 |
| `$1`…`$9`、`${10}`、`$#`、`$@`、`$*`、`$0` | 位置の引数（関数・`sh FILE` の中） |
| `${NAME:-語}`・`:=`・`:+`・`:?`（`:` なしも） | 値がないときの決まり |
| `${#NAME}`、`${NAME#p}`・`##`・`%`・`%%`、`${NAME/a/b}`・`//`、`${NAME:位置:長さ}` | 長さ、前後を削る、置き換え、切り出し |
| `$(コマンド)`、`` `コマンド` `` | コマンドの出力 |
| `$((式))` | 整数の計算（`+ - * / %`、比べる、`&& \|\|`、ビットの演算、`? :`） |
| `*`、`?`、`[…]` | ファイル名（名前の順。合うものがなければそのまま） |
| `{a,b}`、`{1..5}`、`{01..10..2}` | 並べる |
| `~`、`~/…` | `$HOME`（`/root`） |

引用符の外で展開した値は空白で語に分けます（`X="a b"; prog $X` は引数 2 つ、`prog "$X"` は 1 つ）。

### 変数

```sh
NAME=値              # シェルの変数（アプリには見えない）
export NAME=値       # 環境変数（アプリにも見える）
NAME=値 コマンド      # そのコマンドの間だけの環境変数
unset NAME           # 消す
env                  # 環境変数の一覧
env -i A=1 コマンド    # 環境を変えて動かす
```

既定の環境変数: `HOME=/root`、`PATH=/root/apps:/usr/bin`、`LANG=C.UTF-8`、`TERM=xterm-256color`、`TZ`（時刻合わせで決まる）。

### 制御の構文

```sh
if test -f /etc/services.txt; then echo ある; elif …; then …; else echo ない; fi
for f in *.c; do echo "$f"; done
while 条件; do …; done          # until も
case "$x" in a|b) echo AB;; *) echo ほか;; esac
for i in 1 2 3; do [ $i = 2 ] && continue; echo $i; done     # break・continue
{ echo a; echo b; } > out.txt    # まとめてリダイレクト
( cd /tmp; ls )                   # 中の cd・変数は外に残らない
hello() { echo "hello $1"; }      # 関数（local・return も）
hello world
```

`test` と `[` は POSIX と同じです（`-e -f -d -s -n -z -r -w -x`、`= !=`、`-eq -ne -lt -le -gt -ge`、`-nt -ot`、`! -a -o`）。

### スクリプト

```sh
sh script.sh 引数…     # 別のシェルとして（中の cd・変数は残らない）
source script.sh       # 今のシェルで（. script.sh も）
sh -c 'コマンド'
```

起動したときに `/root/.profile` があれば、その中身を動かします（`export PATH=…` などを書いておく）。

### 終了の値

Linux のシェルと同じです: 成功 0、例外で止まった 139、Ctrl-C 130、Ctrl-\ 131、`timeout` の期限 124、
コンパイルできなかった 1、コマンドがない 127、書き方の誤り 2。

### ないもの

`&`、`set -e` などのオプション、`$((i++))` のような代入、`<<<`、`[[ … ]]`、配列、`trap`、`exec`・`eval`・`read`・`printf`（組み込みとしては）。

## 4.5 よく使う組み込みのコマンド

全部の一覧は [第 13 章](13-commands.md) にあります。

```sh
help                         # コマンドの一覧
cd DIR / pwd
history [-c]
which NAME
timeout 5 コマンド            # 5 秒を超えたら止める（-s KILL、-k 秒 も）
dmesg                        # カーネルの記録
date                         # 日時
uptime                       # 起動からの時間
mem                          # メモリとファイルのキャッシュ
clock                        # CPU のクロックと温度
sync                         # 書き込みをすぐ SD カードへ
reboot / poweroff            # 再起動・止める
```
