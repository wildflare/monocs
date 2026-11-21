// lwIP の設定（third_party/lwip の src/include/lwip/opt.h の既定を上書きする）
// OS の仕組み（スレッド・セマフォ）を使わない NO_SYS=1 で、raw API（コールバック）だけを使う。
// lwIP を呼ぶのは os/net/net.c のロック（net_lock）の中だけ
#pragma once

#define NO_SYS                  1
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0
#define SYS_LIGHTWEIGHT_PROT    0       // net_lock で守る

// メモリ（静的に確保する）
#define MEM_ALIGNMENT           8
#define MEM_SIZE                (512 * 1024)
#define MEMP_NUM_PBUF           64
#define MEMP_NUM_TCP_PCB        16
#define MEMP_NUM_TCP_PCB_LISTEN 8
#define MEMP_NUM_TCP_SEG        512     // 1 つの接続で TCP_SND_QUEUELEN（352）まで
#define MEMP_NUM_UDP_PCB        16      // os/net/udp_api.c の MAX_SOCKS（8）+ DHCP（1）+ DNS（問い合わせごと、4 まで）
#define PBUF_POOL_SIZE          128

// プロトコル
#define LWIP_ETHERNET           1
#define LWIP_ARP                1
// 相手の MAC アドレスを調べている間（ARP）に送るパケットを、最後の 1 つだけでなく 3 つまで待たせる（ARP_QUEUE_LEN
// の既定）。内蔵 Wi-Fi では、つないでしばらくの間、最初の ARP の要求に応答が来ず、1 秒以内の再送で通ることが
// ある。その間のパケット（ping の応答など）を捨てずに、応答が来てから送る
#define ARP_QUEUEING            1
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ICMP               1
#define LWIP_RAW                0
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_DHCP               1       // 内蔵 Wi-Fi のアドレス
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_NETIF_HOSTNAME     1
#define LWIP_AUTOIP             0
#define LWIP_DNS                1       // 名前を引く（サーバーは Wi-Fi の DHCP から。os/net/dns_api.c）
#define LWIP_IGMP               0
#define IP_REASSEMBLY           1
#define IP_FRAG                 1
#define LWIP_SINGLE_NETIF       0       // USB イーサネットと内蔵 Wi-Fi
#define SO_REUSE                1       // 待ち受けを SOF_REUSEADDR にする（TIME_WAIT の接続があっても同じポートで）

// TCP（USB の上なので、窓と送信バッファを大きめにする）
#define TCP_MSS                 1460
// 受信の窓: 窓の拡大（window scale）なしの上限に近い 44 × MSS = 64,240（os/net/tcp_api.c の接続ごとの受信
// バッファ 64KB に収まる）。遠いサーバーからの受信の速さは「窓 ÷ 往復の時間」で頭打ちになる（16 × MSS では、
// 往復 0.25 秒のサーバーから約 95KB/s だった）
// 2026-11-22: 窓の拡大（RFC 7323）を使い、128KB に（Wi-Fi は負荷がかかると往復が 10〜25ms に延び、64KB の窓では
// 3.5〜4MB/s で頭打ちになっていた）。TCP_RCV_SCALE 1 で 0xFFFF << 1 まで
#define LWIP_WND_SCALE          1
#define TCP_RCV_SCALE           1
#define TCP_WND                 (88 * TCP_MSS)
// 送信バッファは受信の窓と同じにする（カーネルの TCP エコーは、受け取った分を送信バッファに入るだけ送り返し、
// 入らなければ受け取りを断る。窓のほうが大きいと、断った分を相手が送り直すまで止まり、5MB のエコーが時間切れに
// なった）
#define TCP_SND_BUF             (44 * TCP_MSS)
#define TCP_SND_QUEUELEN        ((4 * TCP_SND_BUF) / TCP_MSS)
// 既定（送信バッファの半分）は 128KB の送信バッファで u16 の決まり（0xFFFF - 4 × MSS 未満）を超えるので、4 分の 1 に
#define TCP_SNDLOWAT            (TCP_SND_BUF / 4)
// TCP の時計の刻み（既定 250ms。再送の時間切れは 2 倍の 500ms 刻みで数え、往復が短くても最小で約 3 刻み =
// 1〜1.5 秒止まった。Wi-Fi で切れ端を 1 つ落とすたびに ssh の画面が止まって見えた。2026-11-20）。50ms なら
// 刻み 100ms で、最小 約 0.3 秒（Linux の最小 0.2 秒に近い）
#define TCP_TMR_INTERVAL        50

#define LWIP_STATS              0
#define LWIP_RAND()             ((u32_t)rand())
