// wifid: 内蔵 Wi-Fi に自動でつなぐサービス。
// /etc/wpa_supplicant.conf（Linux と同じ形）にネットワークがあれば、起動したらスキャンして、見えるものに
// つなぐ（priority の大きいもの、同じなら電波の強いもの）。切れたらつなぎ直す。設定がなければ何もしない
// （30 秒ごとにファイルを見るので、後から書いてもつながる）。
//
//   country=JP
//   network={
//       ssid="名前"
//       psk="パスフレーズ"
//   }
//
// つなぐ手順そのものはカーネルの中（os/drivers/wifi_auto.c）で、ここは呼び続けるだけ。画面には何も出さない
// （様子は wifi status）。手で wifi disconnect したら、wifi connect するまで自動ではつながない。
// 止めるのは svc stop wifid（つながっている接続はそのまま）。
#include <monocs.h>

int service_poll(void)
{
    return monocs_wifi_auto_poll();
}
