// BCM2837 の周辺機器アドレス
// 出典: BCM2835 ARM Peripherals（BCM2837 も同じレイアウト）。
//       資料中のバスアドレス 0x7Exxxxxx は、ARM 物理アドレス 0x3Fxxxxxx に対応する。
#pragma once

#define PERIPHERAL_BASE 0x3F000000UL

// GPIO（6 章）
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000)
#define GPFSEL1         (GPIO_BASE + 0x04)  // GPIO10〜19 の機能選択
#define GPFSEL2         (GPIO_BASE + 0x08)  // GPIO20〜29 の機能選択
#define GPFSEL3         (GPIO_BASE + 0x0C)  // GPIO30〜39 の機能選択
#define GPSET0          (GPIO_BASE + 0x1C)  // GPIO0〜31 を High にする
#define GPSET1          (GPIO_BASE + 0x20)  // GPIO32〜53 を High にする
#define GPCLR0          (GPIO_BASE + 0x28)  // GPIO0〜31 を Low にする
#define GPCLR1          (GPIO_BASE + 0x2C)  // GPIO32〜53 を Low にする
#define GPLEV1          (GPIO_BASE + 0x38)  // GPIO32〜53 の値
#define GPFSEL4         (GPIO_BASE + 0x10)  // GPIO40〜49 の機能選択
#define GPFSEL5         (GPIO_BASE + 0x14)  // GPIO50〜53 の機能選択
#define GPPUD           (GPIO_BASE + 0x94)  // プルアップ／プルダウンの指定
#define GPPUDCLK0       (GPIO_BASE + 0x98)  // GPIO0〜31 への GPPUD の適用
#define GPPUDCLK1       (GPIO_BASE + 0x9C)  // GPIO32〜53 への GPPUD の適用

// PL011 UART（13 章）
#define UART0_BASE      (PERIPHERAL_BASE + 0x201000)
#define UART0_DR        (UART0_BASE + 0x00) // データ
#define UART0_FR        (UART0_BASE + 0x18) // フラグ
#define UART0_IBRD      (UART0_BASE + 0x24) // ボーレート整数部
#define UART0_FBRD      (UART0_BASE + 0x28) // ボーレート小数部
#define UART0_LCRH      (UART0_BASE + 0x2C) // ラインコントロール
#define UART0_CR        (UART0_BASE + 0x30) // コントロール
#define UART0_IFLS      (UART0_BASE + 0x34) // 割り込みを出す FIFO の量
#define UART0_IMSC      (UART0_BASE + 0x38) // 割り込みマスク
#define UART0_MIS       (UART0_BASE + 0x40) // マスク後の割り込みの状態
#define UART0_ICR       (UART0_BASE + 0x44) // 割り込みクリア

// 電源管理とウォッチドッグ（資料にはない。Linux の drivers/watchdog/bcm2835_wdt.c による）
#define PM_BASE         (PERIPHERAL_BASE + 0x100000)
#define PM_RSTC         (PM_BASE + 0x1C)    // リセットの設定
#define PM_RSTS         (PM_BASE + 0x20)    // リセットの状態（Raspberry Pi のファームウェアは、次に起動するパーティションを読む）
#define PM_WDOG         (PM_BASE + 0x24)    // ウォッチドッグの時間（1/65536 秒単位）
#define PM_PASSWORD     0x5A000000U         // PM のレジスタに書くときに上位 8 ビットに付ける
#define PM_RSTC_WRCFG_CLR       0xFFFFFFCFU
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020U
// パーティション 63（ビット 0・2・4・6・8・10 に散らして書く）は「止まる」の印。ファームウェア（bootcode.bin）は起動せずに
// 止まる（Linux の bcm2835_power_off。poweroff）
#define PM_RSTS_RASPBERRYPI_HALT 0x00000555U

// 割り込みコントローラ（7 章）。UART は GPU 割り込みの 57 番（7.5 節の表）
#define IRQ_CTRL_BASE   (PERIPHERAL_BASE + 0xB200)
#define IRQ_PENDING_1   (IRQ_CTRL_BASE + 0x04)  // 割り込み 0〜31 の保留
#define IRQ_PENDING_2   (IRQ_CTRL_BASE + 0x08)  // 割り込み 32〜63 の保留
#define IRQ_ENABLE_1    (IRQ_CTRL_BASE + 0x10)  // 割り込み 0〜31 の許可
#define IRQ_ENABLE_2    (IRQ_CTRL_BASE + 0x14)  // 割り込み 32〜63 の許可
#define IRQ_DISABLE_2   (IRQ_CTRL_BASE + 0x20)  // 割り込み 32〜63 の禁止
#define IRQ_UART        (1U << (57 - 32))
#define IRQ_EMMC        (1U << (62 - 32))   // EMMC（内蔵 Wi-Fi の SDIO のカード割り込み。デバイスツリーの interrupts = <2 30>）
#define IRQ_USB         (1U << 9)          // USB（GPU 割り込み 9 番。Linux の bcm283x.dtsi の interrupts = <1 9>）

// ARM ローカル周辺（コアごとのタイマー・割り込み）
// 出典: BCM2836 ARM-local peripherals（QA7_rev3.4）4.6 節、4.10 節
#define LOCAL_BASE              0x40000000UL
#define CORE0_TIMER_IRQCNTL     (LOCAL_BASE + 0x40) // コア 0 の汎用タイマー割り込みの配送先
#define CORE0_IRQ_SOURCE        (LOCAL_BASE + 0x60) // コア 0 の IRQ 要因
// コアごとのメールボックス（QA7_rev3.4 4.7、4.8 節）。コア n の値は + 4n（制御・要因）、+ 16n（メールボックス）
#define CORE0_MBOX_IRQCNTL      (LOCAL_BASE + 0x50) // メールボックスの割り込みの許可（ビット 0〜3）
#define CORE0_MBOX0_SET         (LOCAL_BASE + 0x80) // メールボックス 0 のビットを立てる（書き込み）
#define CORE0_MBOX0_CLR         (LOCAL_BASE + 0xC0) // メールボックス 0 の読み出し・ビットを消す（1 を書く）
#define IRQ_SOURCE_MBOX0        (1U << 4)
#define IRQ_SOURCE_GPU          (1U << 8)   // GPU 割り込み（割り込みコントローラの要因。既定でコア 0 に配送）
#define IRQ_SOURCE_PMU          (1U << 9)   // 性能カウンタ（PMU）のあふれ
// PMU の割り込みの配送（ビット n: コア n の PMU をコア n の IRQ に。4〜7 は FIQ）。
// 出典: QA7_rev3.4 の「PMU interrupts routing」、Linux の drivers/irqchip/irq-bcm2836.c
// （LOCAL_PM_ROUTING_SET 0x010・LOCAL_PM_ROUTING_CLR 0x014、LOCAL_IRQ_PMU_FAST 9）
#define LOCAL_PM_ROUTING_SET    (LOCAL_BASE + 0x10)
#define LOCAL_PM_ROUTING_CLR    (LOCAL_BASE + 0x14)

// メールボックス（VideoCore との通信）
// 出典: https://github.com/raspberrypi/firmware/wiki/Mailboxes
#define MBOX_BASE       (PERIPHERAL_BASE + 0xB880)
#define MBOX_READ       (MBOX_BASE + 0x00)
#define MBOX_STATUS     (MBOX_BASE + 0x18)
#define MBOX_WRITE      (MBOX_BASE + 0x20)

// EMMC（Arasan SDHCI。SD Host Controller 仕様 3.0 のレジスタ配置を 32 ビット単位で使う）
// 出典: BCM2835 ARM Peripherals 5 章
#define EMMC_BASE       (PERIPHERAL_BASE + 0x300000)
#define EMMC_ARG2       (EMMC_BASE + 0x00)
#define EMMC_BLKSIZECNT (EMMC_BASE + 0x04)
#define EMMC_ARG1       (EMMC_BASE + 0x08)
#define EMMC_CMDTM      (EMMC_BASE + 0x0C)
#define EMMC_RESP0      (EMMC_BASE + 0x10)
#define EMMC_RESP1      (EMMC_BASE + 0x14)
#define EMMC_RESP2      (EMMC_BASE + 0x18)
#define EMMC_RESP3      (EMMC_BASE + 0x1C)
#define EMMC_DATA       (EMMC_BASE + 0x20)
#define EMMC_STATUS     (EMMC_BASE + 0x24)
#define EMMC_CONTROL0   (EMMC_BASE + 0x28)
#define EMMC_CONTROL1   (EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT  (EMMC_BASE + 0x30)
#define EMMC_IRPT_MASK  (EMMC_BASE + 0x34)
#define EMMC_IRPT_EN    (EMMC_BASE + 0x38)
#define EMMC_CONTROL2   (EMMC_BASE + 0x3C)
#define EMMC_SLOTISR_VER (EMMC_BASE + 0xFC)
