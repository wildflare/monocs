# 5. ファイルとフォルダ

## 5.1 フォルダの役割

フォルダの並びは Linux（FHS）に合わせています。**`/usr` は Monocs に付いてくるもの（更新で入れ替わる）、
`/root` は自分のもの**、と覚えておけば十分です。

| フォルダ | 中身 |
|---|---|
| `/boot` | カーネル（`kernel8.img`）、関数名の表（`kernel8.sym`）、デバイスツリー、`overlays/`。ふだんは触らない |
| `/`（ルートのファイル） | `config.txt`（起動の設定）。`bootcode.bin`・`start.elf`・`fixup.dat` はファームウェアで、隠し属性が付いている |
| `/usr/bin` | 付いてくるコマンド（ls、cat、less、grep、curl、gpio、top …）。C のソースのまま |
| `/usr/sbin` | サービス（httpd、telnetd、sshd、wifid、sntp）。C のソースのまま |
| `/usr/lib` | 共有のライブラリ（gpio、i2c、spi、pwm、softuart、tls、bearssl、inflate）、`slimcc/include`（コンパイラのヘッダー）、`firmware/brcm`（Wi-Fi のファームウェア） |
| `/usr/include` | アプリ用のヘッダー（C ライブラリ、`monocs.h`、`curses.h` など） |
| `/usr/share/samples` | サンプルのプログラム |
| `/usr/src/kernel` | Monocs 自身のソース一式（Monocs の上でカーネルを作り直すための材料） |
| `/etc` | 設定（下の表） |
| `/root` | ホーム（`$HOME`）。`.profile`、`.shell_history`、`.ssh/authorized_keys`、`.local/` |
| `/root/apps` | 自分のアプリ（PATH にあるので名前で動く。mi、fd、lua、vim、sqlite3、qjs など） |
| `/root/work` など | 試作のプログラム（自分で作るフォルダ。名前は自由。[第 6 章](06-programs.md) の 6.2） |
| `/tmp` | 作業用。**起動のたびに空になる** |
| `/var/cache/slimcc` | コンパイルのキャッシュ |
| `/var/cache/tls` | 認証局の読み解いた結果のキャッシュ |
| `/var/log` | カーネルの記録（`dmesg`、前の起動の `dmesg.1`、固まったときの `dmesg.crash`） |

### /etc の設定ファイル

| ファイル | 中身 |
|---|---|
| `/etc/services.txt` | 起動時に始めるサービス（[第 7 章](07-services.md)） |
| `/etc/wpa_supplicant.conf` | Wi-Fi のネットワーク（[第 3 章](03-connect.md)） |
| `/etc/htdigest` | Wi-Fi から入るときのユーザー名とパスワード（`passwd` で作る） |
| `/etc/ssh/monocs_host_key` | SSH のホスト鍵（最初の起動で作る） |
| `/etc/ssl/certs/ca-certificates.crt` | HTTPS の認証局 |
| `/etc/fake-hwclock.data` | 最後に覚えた時刻（再起動の後、ネットで合わせるまでの仮の時刻） |

## 5.2 FAT32 の性質

SD カードは FAT32 です。Linux のファイルシステムと違うところ:

- **大文字と小文字を区別しません**（`README` と `readme` は同じファイル）。
- パーミッション・所有者・実行の印はありません（`chmod` は何もしない）。
- シンボリックリンク・ハードリンクはありません。
- 日時は 1980〜2107 年、2 秒の刻みです。
- FAT の属性（隠し・読み出し専用・システム・アーカイブ）があります。`ls` は、隠し属性のファイルを `-a` のときだけ出します。
  属性は `fatattr +h FILE`・`fatattr -h FILE` で変えます。
- 同時に開けるファイルは、ソケットなどと合わせて 253 個（fd 3〜255）です。

`/dev/null` は開けます（書いたものは捨て、読むとすぐ終わり）。ほかの `/dev` はありません。

## 5.3 読み出しは速く、書き込みは少し遅れる

- **読み出し**: 読み出し専用で開いたファイル（4MB まで）は、メモリに覚えておきます（合わせて 64MB まで）。ヘッダーなどを
  何度読んでも SD カードを読みません。`mem` の「file cache」で量がわかります。
- **書き込み**: 書いたものはいったんメモリにため、**最初に書いてから約 1 秒後**、アプリが終わったとき、`sync`・`reboot`・`poweroff`
  のときにまとめて SD カードに書きます。SD カードは 1 回の書き込みが遅いので、まとめると何十倍も速くなるからです。

## 5.4 電源の切り方

**書き込みは最大約 1 秒遅れる**ので、その間に電源を切ると、その分が失われます。

- いちばん確実: **`poweroff`**（書き出してから止まります。LED が消えたら電源を切ってよい。もう一度動かすには電源を入れ直す）。
- 何もしていない状態が数秒続いていれば、そのまま電源を切っても大丈夫です。
- 心配なら `sync` してから切ります。
- Pi 5 からは `python3 tools/uart/nos.py poweroff`。

`reboot` は書き出してから再起動します。

## 5.5 ファイルの操作

コマンドは Linux と同じです（`ls`、`cat`、`cp -r`、`mv`、`rm -rf`、`mkdir -p`、`rmdir`、`touch`、`df -h`、`less`、`grep`、`head`、`tail`、`wc`、`tar`、`gunzip`）。
`ls` は GNU の ls に近い表示です（`-l`、`-a`、`-h`、`-1`、フォルダは青）。

PC との受け渡しは [第 3 章](03-connect.md) の scp・sftp・sshfs・HTTP を使います。

## 5.6 SD カードを PC で見る

SD カードを PC に差すと、ふつうの FAT32 のドライブとして読み書きできます（`config.txt`、`etc/wpa_supplicant.conf`、
`boot/kernel8.img` などを直接直せる）。Monocs はほかの機械での書き換えを知らないので、**Monocs の電源を切ってから**
抜き差しし、差し直したら起動し直してください。

## 5.7 区画を広げる

`make sdimage` のイメージは 64MB です。大きなカードを全部使うには、Pi 5 で:

1. ファイルを全部取り出して控える（`mtools` の `mcopy`、隠し属性も控える）。
2. 区画 1 をカードの終わりまで広げ、`mkfs.vfat -F 32 -s 8` で作り直す（クラスタ 4KB）。
3. ファイルを戻し、`bootcode.bin`・`start.elf`・`fixup.dat` に隠し・システムの属性を付け直す。

2026-11-20 に実機の 32GB のカードで行いました（区画 28.9GB）。詳しい手順は `CLAUDE.md` の「SD カードの FAT を広げた」。
