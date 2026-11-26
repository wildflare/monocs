# 3. つなぎ方

Monocs には画面もキーボードもないので、PC や Pi 5 からつないで使います。つなぎ方は 5 つあり、どれも同じシェルにつながります
（どこから打った文字もシェルに届き、シェルの表示はつながっているすべてに出ます）。

| つなぎ方 | 線 | パスワード | 向いていること |
|---|---|---|---|
| USB シリアル | USB | なし | 線 1 本ですぐ使う |
| UART | ジャンパー線 3 本 | なし | 起動の表示を見る、壊れたときに戻す |
| telnet | USB か Wi-Fi | Wi-Fi からはあり | 画面の大きさが伝わる。暗号化はない |
| **SSH** | USB か Wi-Fi | Wi-Fi からはあり（パスワードか鍵） | **ふだんはこれ**。暗号化、ファイルの転送も |
| HTTP | USB か Wi-Fi | Wi-Fi からはあり | ファイルの読み書き、ブラウザで見る |

## 3.1 USB シリアル

Zero 2 W の「USB」端子を Pi 5 につなぐと、Pi 5 から次の名前で見えます。

```
/dev/serial/by-id/usb-Monocs_Monocs_console-if00     （/dev/ttyACM0 と同じもの）
```

```sh
picocom /dev/ttyACM0        # 終わるには Ctrl-A、Ctrl-X
```

- 速さの設定は関係ありません（USB なので）。約 16MB/s で流れます。
- 画面の大きさは伝わりません（シリアルにはその仕組みがないので、Monocs が端末に問い合わせます）。大きさを変えたら、
  変わったことはアプリに伝わりません。
- 起動の最初の約 1 秒の表示は、USB がつながる前なので見えません（あとで `dmesg` で見られます）。

## 3.2 UART

[第 2 章](02-setup.md) の配線をして、Pi 5 から:

```sh
picocom -b 3000000 /dev/ttyAMA0
```

起動の最初から表示が見え、USB や Wi-Fi が壊れていても使えます。ふだんは使わなくてかまいません。
速さはシェルの `baud` で変えられます（10 秒以内に `y` を打たないと元に戻ります）。

## 3.3 USB イーサネット

USB をつなぐと、Pi 5 に `usb0` というネットワークの口も増えます。

| | アドレス |
|---|---|
| Monocs | 192.168.7.2 |
| Pi 5 | 192.168.7.1（`sh tools/pi5/setup-usbnet.sh` で設定する） |

**USB から届いた接続は、パスワードなしで通します**（ケーブルの先は自分の Pi 5 なので）。

```sh
ssh monocs@192.168.7.2      # SSH（ユーザー名は何でもよい）
telnet 192.168.7.2          # telnet（終わるには Ctrl-]、quit）
curl http://192.168.7.2/    # HTTP
```

## 3.4 Wi-Fi のつなぎ方

内蔵 Wi-Fi は 2.4GHz、WPA2 です。設定は Linux と同じ形のファイル `/etc/wpa_supplicant.conf` に書きます。

```
country=JP
network={
    ssid="家の Wi-Fi の名前"
    psk="パスワード"
    priority=1
}
```

- `network={ … }` はいくつでも書けます。見えるものの中から `priority` が大きい順、電波の強い順につなぎます。
- 暗号なしのネットワークは `key_mgmt=NONE` と書きます。日本語の SSID も使えます。
- このファイルがあれば、**起動して約 10 秒で自動でつながります**（サービス `wifid`）。切れたら 3 秒後につなぎ直し、
  つながらないときは 30 秒ごとにやり直します。
- アドレスは DHCP でもらいます（今の家のルーターでは 192.168.3.96）。変わることがあるので、ルーターで固定すると楽です。

手で操作するときは `wifi` コマンドです。

```
wifi status                    # つながっているか、アドレス、電波の強さ
wifi scan                      # 見えるネットワークの一覧
wifi connect                   # 設定ファイルでつなぐ
wifi connect SSID パスワード    # 名前とパスワードを直接
wifi disconnect                # 切る（この後は wifi connect まで自動ではつながない）
```

ファイルを書くには、USB からつないで `mi /etc/wpa_supplicant.conf`（エディタ）か、Pi 5 から送ります
（`python3 tools/uart/nos.py push wpa_supplicant.conf /etc/wpa_supplicant.conf`）。
SD カードを PC に差して、`etc/wpa_supplicant.conf` を直接書いてもかまいません。

## 3.5 Wi-Fi から入るためのパスワード

**Wi-Fi から届いた接続は、パスワードか鍵がないと断ります**（同じ Wi-Fi のほかの機械から入られないように）。

Monocs のシェルで（USB かシリアルから）:

```
passwd              # パスワードを決める（打った文字は表示しない）。ユーザー名は monocs
passwd 名前          # ユーザー名も変える（ユーザーは 1 人だけ）
passwd -d           # 消す（Wi-Fi からの HTTP・telnet・SSH のパスワードを断るようになる）
```

- 保存するのは `/etc/htdigest` の 1 行（`ユーザー名:Monocs:MD5(…)`）で、パスワードそのものは残しません。
- telnet・HTTP・SSH で同じパスワードを使います。
- まちがいが続くと（続けて 5 回）、そのサービスは 30 秒の間 Wi-Fi からの接続を断ります。

## 3.6 SSH（ふだん使うつなぎ方）

```sh
ssh monocs@192.168.3.96                              # パスワード
ssh -i ~/.ssh/id_ecdsa_monocs monocs@192.168.3.96    # 鍵
```

Pi 5 では `~/.ssh/config` に次のように書いてあるので、`ssh monocs` だけで入れます。

```
Host monocs
    HostName 192.168.3.96
    User monocs
    IdentityFile ~/.ssh/id_ecdsa_monocs
```

できること:

| 使い方 | 例 |
|---|---|
| 対話のシェル | `ssh monocs` |
| コマンドを 1 つ | `ssh monocs 'ls /root'`（終了の値もそのコマンドのもの） |
| ファイルを送る・取る | `scp FILE monocs:/tmp/`、`scp monocs:/var/log/dmesg .` |
| ファイルを操作 | `sftp monocs` |
| Monocs のファイルを PC のフォルダのように | `sshfs monocs:/ ~/monocs`（外すのは `fusermount -u ~/monocs`）。PC のエディタで Monocs のファイルを直接開ける |
| Windows | Windows の `ssh`、WinSCP・FileZilla（SFTP） |

- 同時に 3 接続まで。そのうち対話のシェルは 1 つまで（シェルが 1 つしかないため）。
- `ssh monocs 'コマンド'` の形は、シェルがプロンプトで待っているときに動きます。対話のシェルでアプリ（`top` など）が
  動いていると、終わるまで待ちます（2 秒たつと「waiting for 'top' …」と知らせます）。
- 終わるには `exit`（か行の頭で Ctrl-D）。
- 方式: 鍵の交換 curve25519-sha256、ホスト鍵 ecdsa-sha2-nistp256、暗号 aes128-ctr、hmac-sha2-256。OpenSSH の既定でそのままつながります。
- 自作の実装で、専門家の検査は受けていません。家の LAN の中で使い、インターネットには直接さらさないでください。

## 3.7 SSH の鍵を登録する

パスワードを打たずに入れるようにします。Pi 5（か PC）で鍵を作り:

```sh
ssh-keygen -t ecdsa -b 256 -f ~/.ssh/id_ecdsa_monocs     # Ed25519 はまだ使えない
```

公開鍵（`~/.ssh/id_ecdsa_monocs.pub` の中身の 1 行）を、Monocs の `/root/.ssh/authorized_keys` に足します。
USB からなら次のようにできます。

```sh
scp ~/.ssh/id_ecdsa_monocs.pub monocs@192.168.7.2:/tmp/key.pub
ssh monocs@192.168.7.2 'mkdir -p /root/.ssh; cat /tmp/key.pub >> /root/.ssh/authorized_keys'
```

使える鍵は ECDSA（nistp256）と RSA（2048 ビット以上）です。公開鍵で入るときは、ユーザー名は見ません。

## 3.8 telnet

```sh
telnet 192.168.7.2       # USB から（すぐシェル）
telnet 192.168.3.96      # Wi-Fi から（「Monocs login:」「Password:」と聞かれる）
```

- 画面の大きさが伝わります（ウィンドウを変えるとアプリにも伝わる）。
- **暗号化されません**。Wi-Fi で使うと、パスワードも画面も家の Wi-Fi の中をそのまま流れます。SSH を使ってください。
- Windows の `telnet.exe` は、mi などの画面が崩れます（端末ソフトの RLogin なら正しく動きます）。

## 3.9 HTTP でファイルを読み書きする

サービス `httpd` が、SD カードのファイルを HTTP で見せます。

```sh
curl http://192.168.7.2/etc/services.txt                    # 読む（フォルダなら一覧の HTML。ブラウザでも見られる）
curl -T hello.c http://192.168.7.2/root/apps/hello.c        # 書く（PUT。フォルダも作る）
curl -X DELETE http://192.168.7.2/tmp/old.txt               # 消す
curl --digest -u monocs http://192.168.3.96/etc/services.txt  # Wi-Fi から（ダイジェスト認証）
```

**公開のフォルダ** `/root/share`: この下だけは、Wi-Fi からもパスワードなしで読めます（GET・HEAD だけ）。ほかの Monocs に C のソースを渡すための場所です（`sandbox http://相手/root/share/…/prog.c#sha256=…`）。同じ Wi-Fi の誰からでも読めるので、人に見せてよいものだけを置いてください。

特別なパス:

| パス | 中身 |
|---|---|
| `/.status` | アプリが動いているか（`app 0|1`）、telnet・ssh でつながっているか（`remote 0|1`）、最後の終了の値 |
| `/.log` | カーネルの記録（`dmesg` と同じ） |
| `/.sdcard/fat.img` | SD カードの区画をまるごと（壊れたときの診断と修復。[第 11 章](11-troubleshooting.md)） |

## 3.10 端末ソフトについて

Monocs は端末を xterm（`TERM=xterm-256color`）、文字コードを UTF-8 とみなします。Linux の端末、
Windows Terminal、RLogin などの xterm 互換のものを使ってください。日本語も表示・入力できます。
