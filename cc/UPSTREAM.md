# slimcc-a64 の元になったソース

- 元: slimcc（https://github.com/fuhsnn/slimcc、MIT ライセンス、`LICENSE` を参照）
- コミット: 2388c39989886fee05df045b6a669d684aee3946（2026-10-24）
- 同じものが `third_party/slimcc`（サブモジュール、無改造）にある。元との差分は次で見られる:

  ```sh
  diff -ru --exclude .git --exclude .github third_party/slimcc cc
  ```

slimcc は x86-64 専用。`cc/` ではこれを AArch64（Linux、および Monocs）向けに書き換えていく。
