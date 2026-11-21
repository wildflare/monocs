# FatFs のパッチ

作者（ChaN 氏）が公開している公式パッチ。いずれも `ff.c` に、番号順に当てる。
出典: https://elm-chan.org/fsw/ff/patches.html

| ファイル | 内容 | SHA-256 |
|---|---|---|
| ff16p1.diff | 最小ボリュームサイズを 128 セクタから 64 セクタに変更（2025-09-13） | 7996ddc3135f3d534a8153f7d75d587030e5c8551b003c3b7ee823ffb7fc64bf |
| ff16p2.diff | CVE-2026-6682、6683、6687 の修正（2026-07-10。patch-1 の後に当てる） | 5cd39f1fc299f0f1dec9fa1ce544dc0bc048b2f6eb3b8161476ed20f9cf1e290 |

配布されたソース（`third_party/fatfs/source/`）は改行が CRLF、パッチは LF なので、
ビルド時に CR を取り除いてからパッチを当てる（Makefile の `$(FATFS_STAMP)` を参照）。

## 取得元

- FatFs R0.16: https://elm-chan.org/fsw/ff/arc/ff16.zip
  （SHA-256 99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e、2026-11-04 取得）
- `third_party/fatfs/` には、zip の `LICENSE.txt` と `source/` だけを置いている（HTML の資料は除いた）。
