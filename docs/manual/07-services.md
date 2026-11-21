# 7. サービス

サービスは、**裏で動き続けるプログラム**です（Linux のデーモンに当たります）。どれもカーネルの外の C のプログラムで、
**コア 3** で動きます。そのためアプリ（コア 0〜2）の邪魔をせず、アプリが動いている間も HTTP や SSH は答えます。

## 7.1 付いてくるサービス

| 名前 | ソース | 役目 |
|---|---|---|
| httpd | `/usr/sbin/httpd.c` | HTTP（ポート 80）。ファイルの読み書き、`/.status`、`/.log` |
| telnetd | `/usr/sbin/telnetd.c` | telnet（ポート 23） |
| sshd | `/usr/sbin/sshd/`（フォルダ） | SSH（ポート 22）。sftp・scp も |
| wifid | `/usr/sbin/wifid.c` | Wi-Fi に自動でつなぐ |
| sntp | `/usr/sbin/sntp.c` | 時刻を合わせる（1 時間ごと） |

## 7.2 起動時に始めるもの

`/etc/services.txt` に 1 行に 1 つ、「ソース（ファイルかフォルダ）と引数」を書きます。`#` で始まる行は飛ばします。

```
/usr/sbin/httpd.c
/usr/sbin/telnetd.c
/usr/sbin/sshd
/usr/sbin/wifid.c
/usr/sbin/sntp.c 192.168.7.1 pool.ntp.org -i 3600 -z +0900
```

起動するときに上から順にコンパイル（キャッシュがあれば読むだけ）して始めます。止めたいサービスは行の頭に `#` を付けます。

## 7.3 操作

```sh
svc                          # 一覧（とアプリが使えるコアの数）
svc start FILE.c [引数]       # 始める（フォルダも）
svc stop 名前                 # 止める（名前はファイル名から .c を除いたもの）
top                          # サービスごとの CPU の使い方も見える
```

- サービスが 1 つでもある間はコア 3 をサービス用に取っておくので、アプリには 3 コアに見えます。全部止めると 4 コアに戻ります。
- サービスが例外で止まると、シェルが気づいて後始末をします（`dmesg` に記録が残る）。

## 7.4 サービスの書き方

`main` の代わりに、次の 3 つの関数を書きます。

```c
#include <monocs.h>

int service_start(int argc, char **argv)   // 始めるとき。0 を返すと成功（省いてもよい）
{
    return 0;
}

int service_poll(void)                     // 何度も呼ばれる。待たずに戻ること。仕事をしたら 0 以外
{
    return 0;
}

void service_stop(void)                    // 止めるとき（省いてもよい）
{
}
```

- コア 3 は、すべてのサービスの `service_poll` を順に呼び続けます（協調型）。**1 つのサービスが待つと、ほかも止まります**。
  ソケットは待たない形（`O_NONBLOCK` か `monocs_tcp_*` の timeout 0）で使ってください。
- どのサービスも仕事をしなかったら、コア 3 は眠って電力を節約します（約 1ms ごとに起きる）。
- アプリと同じ関数（ファイル、ソケットなど）が使えます。開いたものは、サービスを止めたときに閉じます。
- 例: `services/httpd.c`、`services/sntp.c`（小さい）。
