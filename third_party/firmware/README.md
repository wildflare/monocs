# 内蔵 Wi-Fi のファームウェア（無改造）

Raspberry Pi Zero 2 W の Wi-Fi のチップ（CYW43436s。ChipCommon の番号 43430、版 1）に読み込ませるもの。
SD カードの `/lib/firmware/brcm` に入れる（Linux と同じ場所。`tools/mksd.sh`）。Monocs の `os/drivers/wifi_auto.c`・`wifi.c` が読み込む。

| ファイル | 元（Pi 5 の Raspberry Pi OS） | ライセンス |
|---|---|---|
| `brcm/brcmfmac43436s-sdio.bin`（ファームウェア） | `/usr/lib/firmware/brcm/brcmfmac43436s-sdio.bin` | Synaptics（`brcm/LICENCE.synaptics`） |
| `brcm/brcmfmac43436s-sdio.txt`（nvram。基板の設定） | `/usr/lib/firmware/brcm/brcmfmac43436s-sdio.txt` | Synaptics（同上） |
| `brcm/brcmfmac43430-sdio.clm_blob`（国ごとの電波の決まり） | `/usr/lib/firmware/cypress/cyfmac43430-sdio.clm_blob` | Cypress（`brcm/LICENCE.cypress`） |

- パッケージは firmware-brcm80211 1:20260519-1~bpo13+1+rpt1（2026-11-13 に写した）。ライセンスの文は、その
  `/usr/share/doc/firmware-brcm80211/copyright` の該当する部分（43436 のファイルは Synaptics、cypress/cyfmac43* は
  Cypress）。どちらも「その会社のチップで使うために、バイナリのまま複製・配布してよい」もので、変更・逆アセンブルは
  禁止。中身は手を加えずに使う。
- どれを使うかは Linux の brcmfmac と同じ: 43430 の版 1 の Zero 2 W は `brcmfmac43430-sdio.raspberrypi,model-zero-2-w.bin`
  → `brcmfmac43436s-sdio.bin`、`.txt` → `brcmfmac43436s-sdio.txt`、clm_blob は `brcmfmac43430-sdio.clm_blob`
  （Pi 5 の /lib/firmware/brcm のシンボリックリンク）。版 2 以上のチップ（43436）は別のファイル
  （brcmfmac43436-sdio.*）で、今は入れていない。
- SHA-256: bin 68b9bcc9…, txt 37a8b85a…, clm_blob 3376b9c9…
