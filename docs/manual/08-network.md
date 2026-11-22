# 8. ネットワーク

## 8.1 口とアドレス

| 口 | Monocs | 相手 | 使い道 |
|---|---|---|---|
| USB イーサネット（CDC-ECM） | 192.168.7.2（固定） | Pi 5 の 192.168.7.1 | 開発（パスワードなし） |
| 内蔵 Wi-Fi（w0） | DHCP（今は 192.168.3.96） | 家の LAN とインターネット | 線なしで使う、外へ出る |

- IPv4 だけです（IPv6 はありません）。ループバック（127.0.0.1）もありません。
- 両方つながっているときは、外（インターネット）へは Wi-Fi を使います。
- 中身は lwIP（TCP/IP の部品）です。状態は `usb`（USB 側と、TCP の接続の一覧）と `wifi status` で見られます。

## 8.2 名前（DNS）と時刻（NTP）

- DNS のサーバーは Wi-Fi の DHCP でもらいます。`host example.com` で引けます（出力は Linux の host と同じ形）。
- 時刻はサービス `sntp` が合わせます。Pi 5（192.168.7.1 の chrony）と `pool.ntp.org` に同時に聞き、最初の答えを使います
  （起動から約 17 秒以内）。合うまでは、前に覚えた時刻（`/etc/fake-hwclock.data`）を仮に使います（`date` が「推定」と出す）。
- タイムゾーンは `-z +0900`（`/etc/services.txt` の sntp の行）。`date -z +0900` でも変えられます。
- HTTPS は、時刻が合うまでつなぎません（証明書の期限を確かめられないため）。

## 8.3 curl

```sh
curl https://example.com/                 # 取る（HTTPS も。TLS 1.2）
curl -o file.tar.gz -L https://…           # ファイルに保存、転送をたどる
curl -sL https://…/x.tar.gz | tar xz      # 取って展開
curl -I URL                                # ヘッダーだけ
curl -X POST -H 'Content-Type: application/json' -d '{"a":1}' URL
curl --compressed URL                     # gzip で受け取る
```

使えるオプション: `-o -O -L -I -i -s -S -f -m -X -H -d -A -v --compressed --raw`。終了の値は本物の curl と同じ番号です。
HTTP/2・TLS 1.3 はありません。

## 8.4 Monocs が待ち受けているポート

| ポート | サービス | Wi-Fi から |
|---|---|---|
| 22 | sshd | パスワードか鍵 |
| 23 | telnetd | パスワード |
| 80 | httpd | ダイジェスト認証 |
| 7 | 確認用の TCP エコー（カーネル） | 断る（USB だけ） |

## 8.5 プログラムからネットワークを使う

**Linux と同じソケット**が使えます: `socket`・`bind`・`listen`・`accept`・`connect`・`send`・`recv`・`sendto`・`recvfrom`・
`select`・`poll`・`getaddrinfo`・`setsockopt`（`SO_RCVTIMEO` など）・`fcntl(O_NONBLOCK)`。TCP と UDP（AF_INET）。
`bind(INADDR_ANY)` は USB と Wi-Fi の両方から受けます。

Monocs 独自の、待つ時間を引数で渡す簡単な API もあります（`#include <monocs.h>`）。

```c
int c = monocs_tcp_connect("example.com", 80, 5000);   // 5 秒まで待つ
monocs_tcp_send(c, req, len, 5000);
int n = monocs_tcp_recv(c, buf, sizeof buf, 5000);
monocs_tcp_close(c);
```

HTTPS は `-ltls` の `monocs_tls_connect` など。例: `/usr/share/samples/http_get.c`・`tcp_echo.c`。

上限: TCP の接続はサービスの分も含めて 16、UDP のソケットは 8、Linux のソケットは 32 です。

## 8.6 速さの目安

| 経路 | 速さ |
|---|---|
| USB イーサネット | 約 15MB/s |
| Wi-Fi（同じ LAN、TCP） | 受ける 3.6〜5MB/s、送る 2.3〜3MB/s（電波の具合で揺れる。`tools/perf/wifibench.py`）。sftp は暗号の計算もあって 2〜3MB/s |
| Wi-Fi の ping | 約 5ms |

## 8.7 Wi-Fi の省電力

`wifi pm` でチップの省電力を切り替えられます（再起動すると 0 に戻ります）。

| 設定 | 中身 | 測った様子（同じ LAN） |
|---|---|---|
| `wifi pm 0` | 切る（Monocs の既定） | ping 平均 2.9ms、ssh の 1 往復 約 61ms |
| `wifi pm 2` | 通信があれば起きている（Linux の既定） | 暇な後の最初の 1 つが 0.1〜0.2 秒遅れる。転送の速さはほぼ同じ |
| `wifi pm 1` | いちばん省電力 | ssh の 1 往復が 1 秒を超え、転送も遅い |

どれだけ電気が減るかは、まだ測っていません。電池で長く使うときに `wifi pm 2` を試してください。

## 8.8 わかっているクセ

- 家のルーターでは、つないだ直後に最初の ARP に答えが来ないことがあり、最初の通信が 1 秒ほど遅れることがあります。
- 長く使っていると、Wi-Fi で 10〜20 分に 1 回、1〜3 秒止まることがあります（ルーターから Monocs への電波が途切れている。
  Monocs の外の問題と見ています。`dmesg` に「no data received for … ms」と残ります）。
