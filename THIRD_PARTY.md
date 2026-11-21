# 他のプロジェクトから来たもの（Third-party components）

Monocs 自身は MIT ライセンス（`LICENSE`、Copyright (c) 2026 asamizu）です。次のものは、それぞれのライセンスに従います。
カーネル（`kernel8.img`）と SD カードのイメージには、このうち slimcc（`cc/`）・picolibc・FatFs・lwIP が入っています。
BearSSL は SD カードの `/usr/lib/bearssl`、Wi-Fi のファームウェアは `/usr/lib/firmware/brcm` に入ります。
SD カードのイメージには、それぞれのライセンスの文を `/usr/share/licenses/名前/` に入れています（`tools/mksd.sh`）。

| もの | 場所 | ライセンス | ライセンスの文 |
|---|---|---|---|
| slimcc（コンパイラ slimcc-a64 の元） | `cc/`（書き換えたもの）、`third_party/slimcc`（元のまま） | MIT（Copyright (c) 2019 Rui Ueyama、2023-2026 Hsiang-Ying Fu） | `cc/LICENSE` |
| picolibc（C ライブラリ） | `third_party/picolibc` | BSD 系（ファイルごとに複数） | `third_party/picolibc/COPYING.picolibc` |
| FatFs（FAT ファイルシステム） | `third_party/fatfs` | FatFs のライセンス（BSD 1 条項相当） | `third_party/fatfs/LICENSE.txt` |
| lwIP（TCP/IP） | `third_party/lwip` | BSD 3 条項 | `third_party/lwip/COPYING` |
| BearSSL（TLS・暗号） | `third_party/bearssl` | MIT | `third_party/bearssl/LICENSE.txt` |
| Wi-Fi のファームウェア（CYW43436s） | `third_party/firmware` | Synaptics・Cypress のバイナリの再配布の許諾（変更・逆アセンブルは禁止） | `third_party/firmware/brcm/LICENCE.cypress`・`LICENCE.synaptics`、`third_party/firmware/README.md` |
| Raspberry Pi のファームウェア・デバイスツリー | SD カードのルートと `/boot`（イメージを作る Pi 5 の `/boot/firmware` から写す。リポジトリには入れない） | Broadcom の再配布の許諾 | `/boot/LICENCE.broadcom` |
| 認証局の証明書 | SD カードの `/etc/ssl/certs/ca-certificates.crt`（イメージを作る機械のものを写す。リポジトリには入れない） | Mozilla の一覧（MPL 2.0） | Debian の ca-certificates |
