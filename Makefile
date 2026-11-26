# 開発マシン（Pi 5、AArch64）ではネイティブの GCC をそのまま使う。
# x86 などで作業するときは make CROSS_COMPILE=aarch64-linux-gnu- とする。
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
GDB     := gdb

BUILD   := build
ELF     := $(BUILD)/kernel.elf
IMG     := $(BUILD)/kernel8.img
# 標準のカーネル（slimcc 版。下の「カーネルを slimcc-a64 でビルドする」）。規則の前提で使うので先に決める
KSLIM          := $(BUILD)/kslim
KSLIM_IMG      := $(KSLIM)/kernel8.img
LDSCRIPT := os/boot/linker.ld

# picolibc（third_party/picolibc を build/ 以下でビルドしてインストールする）
PICOLIBC_SRC   := third_party/picolibc
PICOLIBC_BUILD := $(BUILD)/picolibc
PICOLIBC       := $(BUILD)/picolibc-install
PICOLIBC_LIB   := $(PICOLIBC)/lib/libc.a
# io-long-double: printf・scanf の long double（%Lg など）に対応する
# single-thread=false: ロック（__retarget_lock_*）を os/kernel/lock.c に任せる（マルチコア）
# thread-local-storage: errno・strtok・rand などをコアごとにする（TLS の初期化は os/kernel/tls.c）
# fstat-bufsiz: FILE のバッファの大きさを fstat の st_blksize にする（VFS は 16KB）。fast-bufio: 大きな fread・fwrite はバッファを通さない
# stdio-locking: printf などは呼び出しごとに 1 回だけファイルのロックを取る（false だと 1 文字ごとに取り、遅い）
# （オプションを変えたら rm -rf build/picolibc build/picolibc-install で作り直す）
PICOLIBC_OPTS  := -Dmultilib=false -Dpicocrt=false -Dsemihost=false \
                  -Dtests=false -Dthread-local-storage=true -Dthread-local-storage-api=false -Dsingle-thread=false -Dstdio-locking=true \
                  -Dfstat-bufsiz=true -Dfast-bufio=true \
                  -Dio-long-double=true -Dmb-capable=true -Dio-wchar=true \
                  -Dspecsdir=none -Dprefix=/ -Dincludedir=include -Dlibdir=lib \
                  --buildtype=release
LIBGCC         := $(shell $(CC) -print-libgcc-file-name)

# FatFs（third_party/fatfs を build/fatfs にコピーし、公式パッチと ffconf.h を当てる）
FATFS_SRC      := third_party/fatfs/source
FATFS_PATCHES  := $(sort $(wildcard third_party/patches/fatfs/*.diff))
FATFS          := $(BUILD)/fatfs
FATFS_STAMP    := $(FATFS)/.prepared
FATFS_OBJS     := $(FATFS)/ff.o $(FATFS)/ffunicode.o

# lwIP（TCP/IP。third_party/lwip は無改造。設定は os/net/port/lwipopts.h）
LWIP           := third_party/lwip/src
LWIP_SRCS      := core/init core/def core/inet_chksum core/ip core/mem core/memp core/netif core/pbuf \
                  core/raw core/stats core/sys core/tcp core/tcp_in core/tcp_out core/timeouts core/udp \
                  core/dns core/ipv4/dhcp core/ipv4/etharp core/ipv4/icmp core/ipv4/ip4 core/ipv4/ip4_addr core/ipv4/ip4_frag \
                  netif/ethernet
LWIP_OBJS      := $(LWIP_SRCS:%=$(BUILD)/lwip/%.o)
LWIP_INC       := -I$(LWIP)/include -Ios/net/port

# slimcc-a64 をライブラリ（libslimcc）としてカーネルに組み込む（cc/libslimcc.h）
SLIMCC         := cc
SLIMCC_SRCS    := alloc assembler bitint codegen hashmap ld libslimcc memlink options parse \
                  preprocess strings tokenize type unicode zone platform/monocs
SLIMCC_OBJS    := $(SLIMCC_SRCS:%=$(BUILD)/slimcc/%.o)

SDIMG          := $(BUILD)/sdcard.img

CFLAGS  := -std=c23 -mcpu=cortex-a53 -ffreestanding -nostdlib -nostartfiles \
           -mstrict-align -O2 -g -Wall -Wextra \
           -fno-pie -fno-stack-protector -mno-outline-atomics \
           -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
           -nostdinc -isystem $(PICOLIBC)/include -isystem $(shell $(CC) -print-file-name=include) \
           -I$(FATFS) $(LWIP_INC) \
           -MMD -MP
ASFLAGS := -mcpu=cortex-a53 -g -MMD -MP
# --no-warn-rwx-segments: 単一アドレス空間なので RWX は意図どおり
LDFLAGS := -nostdlib -T $(LDSCRIPT) --no-warn-rwx-segments

SRCS_S  := $(shell find os -name '*.S')
SRCS_C  := $(shell find os -name '*.c')
OBJS    := $(SRCS_S:%.S=$(BUILD)/%.o) $(SRCS_C:%.c=$(BUILD)/%.o)

# QEMU で動かすのは標準の slimcc 版のカーネル（qemu-gcc で GCC 版）
QEMU    := qemu-system-aarch64 -M raspi3b -kernel $(KSLIM)/kernel8.img -serial stdio -display none \
           -drive file=$(SDIMG),if=sd,format=raw

.PHONY: all qemu debug clean distclean picolibc sdcard sdimage sdimage-blink sdimage-self sdimage-self-auto sdimage-smp-auto picolibc-slimcc cc cc-test \
        kernel-slimcc qemu-slimcc ld-test kernel-gcc qemu-gcc

# 標準のカーネルは slimcc 版（build/kslim/kernel8.img。2026-11-16 から。Monocs の中で作り直せる、自作の
# コンパイラ・リンカで作ったもの）。GCC 版（build/kernel8.img）は比べるために kernel-gcc で作る
all: kernel-slimcc

kernel-gcc: $(IMG)

$(IMG): $(ELF) tools/perf/mksym.py
	$(OBJCOPY) -O binary $< $@
	python3 tools/perf/mksym.py $@ --elf $(ELF) -o $(BUILD)/kernel8.sym   # perf record の関数名（/boot/kernel8.sym）

$(ELF): $(OBJS) $(FATFS_OBJS) $(LWIP_OBJS) $(SLIMCC_OBJS) $(LDSCRIPT) $(PICOLIBC_LIB)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(FATFS_OBJS) $(LWIP_OBJS) $(SLIMCC_OBJS) --start-group $(PICOLIBC_LIB) $(LIBGCC) --end-group

picolibc: $(PICOLIBC_LIB)

$(PICOLIBC_LIB): os/libc/picolibc-cross.txt
	rm -rf $(PICOLIBC_BUILD) $(PICOLIBC)
	meson setup $(PICOLIBC_BUILD) $(PICOLIBC_SRC) --cross-file $< $(PICOLIBC_OPTS)
	meson compile -C $(PICOLIBC_BUILD)
	DESTDIR=$(abspath $(PICOLIBC)) meson install -C $(PICOLIBC_BUILD) --quiet

$(FATFS_STAMP): $(wildcard $(FATFS_SRC)/*) $(FATFS_PATCHES) os/fs/ffconf.h
	rm -rf $(FATFS)
	mkdir -p $(FATFS)
	for f in ff.c ff.h diskio.h ffunicode.c; do sed 's/\r$$//' $(FATFS_SRC)/$$f > $(FATFS)/$$f; done
	for p in $(FATFS_PATCHES); do patch -s $(FATFS)/ff.c $$p; done
	cp os/fs/ffconf.h $(FATFS)/ffconf.h
	touch $@

$(FATFS)/ff.c $(FATFS)/ffunicode.c: $(FATFS_STAMP) ;

$(FATFS)/%.o: $(FATFS)/%.c | $(PICOLIBC_LIB)
	$(CC) $(CFLAGS) -c -o $@ $<

# lwIP も第三者のコードなので警告は出さない
$(BUILD)/lwip/%.o: $(LWIP)/%.c | $(PICOLIBC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Wall -Wextra,$(CFLAGS)) -w -c -o $@ $<

# 第三者のコードなので警告は出さない（改造しないため）
# slimcc-a64 は GNU C17 で書かれている。SLIMCC_LIBRARY でゾーン（cc/zone.c）を使う
$(BUILD)/slimcc/%.o: $(SLIMCC)/%.c $(SLIMCC)/slimcc.h $(SLIMCC)/libslimcc.h $(SLIMCC)/ld.h $(SLIMCC)/ir.inc $(SLIMCC)/ir_a64.inc $(SLIMCC)/ir_a64_emit.inc $(SLIMCC)/sched.inc | $(PICOLIBC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c23 -Wextra,$(CFLAGS)) -std=gnu17 -Wno-switch -DSLIMCC_LIBRARY -c -o $@ $<

# os/slimcc と、それを使う os/shell は cc/libslimcc.h を使う
$(BUILD)/os/slimcc/%.o: CFLAGS += -I$(SLIMCC)
$(BUILD)/os/shell/%.o: CFLAGS += -I$(SLIMCC)

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c | $(PICOLIBC_LIB) $(FATFS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

qemu: $(KSLIM_IMG) $(SDIMG)
	$(QEMU)

qemu-gcc: $(IMG) $(SDIMG)
	qemu-system-aarch64 -M raspi3b -kernel $(IMG) -serial stdio -display none \
	    -drive file=$(SDIMG),if=sd,format=raw

# 別の端末で: gdb build/kernel.elf -ex 'target remote :1234'
debug: $(IMG) $(SDIMG)
	$(QEMU) -s -S

# 実機用の SD カードの中身を build/sdcard/ に集める
sdcard: $(KSLIM_IMG)
	tools/mksd.sh

# SD カードイメージ（QEMU 用。実機用に Rufus などで書き込むこともできる）
sdimage: $(SDIMG)

$(SDIMG): $(KSLIM_IMG) os/boot/config.txt tools/mksd.sh tools/mkimg.sh LICENSE THIRD_PARTY.md $(wildcard os/api/include/*.h os/api/include/*/*.h) $(wildcard samples/*) $(wildcard samples/*/*) $(wildcard bin/*) $(wildcard bin/*/*) $(wildcard lib/*/*) $(wildcard cc/slimcc_headers/include/*) $(wildcard services/*) $(wildcard services/*/*)
	tools/mksd.sh
	tools/mkimg.sh

# 実機確認用: samples/blink.c を起動時に自動で実行するイメージ（UART なしで slimcc-a64 の動作を LED で確かめる）
sdimage-blink: $(KSLIM_IMG)
	tools/mksd.sh
	tools/mkimg.sh samples/blink.c
	mv $(SDIMG) $(BUILD)/sdcard-blink.img
	@echo "$(BUILD)/sdcard-blink.img を作りました"

# 実機でのマルチコアの確認用: 起動時に 4 コアで計算し、結果を LED で知らせる（tools/smp/autorun.c）
sdimage-smp-auto: $(KSLIM_IMG)
	tools/mksd.sh
	tools/mkimg.sh tools/smp/autorun.c
	mv $(SDIMG) $(BUILD)/sdcard-smp-auto.img
	@echo "$(BUILD)/sdcard-smp-auto.img を作りました"

# セルフホスト用: /usr/src/kernel にカーネルのソース一式とビルドスクリプトを入れたイメージ build/sdcard-self.img
# （Monocs 上で cd /usr/src/kernel、cc -o build-kernel build.c && ./build-kernel。結果は Linux 上の make kernel-slimcc と比べる）
sdimage-self: $(KSLIM_IMG) $(FATFS_STAMP) $(KSLIM_LIBC)
	tools/mksd.sh
	tools/mkimg.sh
	tools/selfhost/mksrc.sh $(BUILD)/selfsrc $(KSLIM_LIBC) $(KSLIM_OBJS)
	python3 tools/selfhost/mklibc.py $(BUILD)/selfsrc
	mmd -i "$(SDIMG)@@1048576" ::/usr/src/kernel
	mcopy -s -i "$(SDIMG)@@1048576" $(BUILD)/selfsrc/* ::/usr/src/kernel/
	mv $(SDIMG) $(BUILD)/sdcard-self.img
	@echo "$(BUILD)/sdcard-self.img を作りました"

# 実機でのセルフホストの確認用: 起動時に /usr/src/kernel のビルドを自動で行い、結果を LED で知らせる
# （tools/selfhost/autorun.c。手順は docs/hardware-test.md）
sdimage-self-auto: sdimage-self
	mv $(BUILD)/sdcard-self.img $(BUILD)/sdcard-self-auto.img
	mcopy -i "$(BUILD)/sdcard-self-auto.img@@1048576" tools/selfhost/autorun.c ::/autorun.c
	@echo "$(BUILD)/sdcard-self-auto.img を作りました"

# slimcc-a64（cc/。Pi 5 の Linux 上で動くコンパイラ）
cc:
	$(MAKE) -C cc slimcc

# カーネルを slimcc-a64 でビルドする（セルフホストの準備）
# 生成物は build/kslim/。C と .S をすべて cc/slimcc-monocs（Monocs 向けのクロスコンパイラ）でコンパイルし、
# cc/slimld（自作のリンカ。配置は os/boot/linker.ld と同じ）で kernel8.img を作る
KSLIM          := $(BUILD)/kslim
KSLIM_CC       := cc/slimcc-monocs
KSLIM_INC      := -nostdinc -isystem $(SLIMCC)/slimcc_headers/include -isystem $(PICOLIBC)/include
# libgcc の代わり（long double の演算、__clear_cache。cc/rt/）
RT_SRCS        := softfp128 clear_cache
KSLIM_OBJS     := $(SRCS_S:%.S=$(KSLIM)/%.o) $(SRCS_C:%.c=$(KSLIM)/%.o) \
                  $(KSLIM)/fatfs/ff.o $(KSLIM)/fatfs/ffunicode.o $(LWIP_SRCS:%=$(KSLIM)/lwip/%.o) \
                  $(SLIMCC_SRCS:%=$(KSLIM)/slimcc/%.o) $(RT_SRCS:%=$(KSLIM)/rt/%.o)
KSLIM_IMG      := $(KSLIM)/kernel8.img
SLIMLD         := cc/slimld
# slimcc でビルドした picolibc（tools/picolibc/build.py。複素数の関数は除く）
KSLIM_LIBC     := $(BUILD)/picolibc-slimcc/libc.a

kernel-slimcc: $(KSLIM_IMG)

$(KSLIM_IMG): $(KSLIM_OBJS) $(SLIMLD) $(KSLIM_LIBC)
	$(SLIMLD) -Map $(KSLIM)/kernel.map -o $@ $(KSLIM_OBJS) $(KSLIM_LIBC)
	python3 tools/perf/mksym.py $@ --map $(KSLIM)/kernel.map -o $(KSLIM)/kernel8.sym

picolibc-slimcc: $(KSLIM_LIBC)

# GCC で設定・ビルドした picolibc（build/picolibc）の compile_commands.json を使う
$(KSLIM_LIBC): $(PICOLIBC_LIB) $(KSLIM_CC) tools/picolibc/build.py
	python3 tools/picolibc/build.py

$(SLIMLD): $(SLIMCC)/ld.c $(SLIMCC)/ld.h $(SLIMCC)/ld_main.c
	$(MAKE) -C cc slimld

# slimld と GNU ld の結果を、GCC でビルドしたカーネルと slimcc でビルドしたカーネルで比べる
ld-test: $(ELF) $(KSLIM_OBJS) $(SLIMLD)
	rm -rf $(BUILD)/ld-test
	sh cc/test-a64/ld_test.sh $(BUILD)/ld-test gcc $(OBJS) $(FATFS_OBJS) $(LWIP_OBJS) $(SLIMCC_OBJS) $(PICOLIBC_LIB) $(LIBGCC)
	sh cc/test-a64/ld_test.sh $(BUILD)/ld-test slimcc $(KSLIM_OBJS) $(KSLIM_LIBC)

$(KSLIM_CC): $(wildcard $(SLIMCC)/*.c $(SLIMCC)/*.h $(SLIMCC)/*.inc $(SLIMCC)/platform/*.c)
	$(MAKE) -C cc slimcc-monocs

# 中のコンパイラの識別（run のコンパイルのキャッシュの鍵。os/slimcc/cache.c が読む）。コンパイラの出力に効くものから作る
COMPILER_ID_SRCS := $(sort $(wildcard $(SLIMCC)/*.c $(SLIMCC)/*.h $(SLIMCC)/*.inc)) $(SLIMCC)/platform/monocs.c \
                    os/slimcc/cache.c os/slimcc/cache.h os/slimcc/run.c
COMPILER_ID      := $(BUILD)/gen/compiler_id.h
$(COMPILER_ID): $(COMPILER_ID_SRCS) tools/compiler_id.sh
	@sh tools/compiler_id.sh $@ $(COMPILER_ID_SRCS)
$(BUILD)/os/slimcc/cache.o $(KSLIM)/os/slimcc/cache.o: $(COMPILER_ID)

# 依存関係（ヘッダーなど）は slimcc の -MMD で .d に書く
KSLIM_DEP       = -MMD -MP -MF $(@:.o=.d) -MT $@

$(KSLIM)/%.o: %.c $(KSLIM_CC) | $(PICOLIBC_LIB) $(FATFS_STAMP)
	@mkdir -p $(dir $@)
	$(KSLIM_CC) -std=c23 $(KSLIM_INC) -I$(FATFS) -I$(SLIMCC) $(LWIP_INC) $(KSLIM_DEP) -c -o $@ $<

$(KSLIM)/%.o: %.S $(KSLIM_CC)
	@mkdir -p $(dir $@)
	$(KSLIM_CC) $(KSLIM_DEP) -c -o $@ $<

$(KSLIM)/fatfs/%.o: $(FATFS)/%.c $(KSLIM_CC) | $(FATFS_STAMP)
	@mkdir -p $(dir $@)
	$(KSLIM_CC) -std=c23 $(KSLIM_INC) -I$(FATFS) $(KSLIM_DEP) -c -o $@ $<

$(KSLIM)/lwip/%.o: $(LWIP)/%.c $(KSLIM_CC) | $(PICOLIBC_LIB)
	@mkdir -p $(dir $@)
	$(KSLIM_CC) -std=c23 $(KSLIM_INC) $(LWIP_INC) $(KSLIM_DEP) -c -o $@ $<

$(KSLIM)/slimcc/%.o: $(SLIMCC)/%.c $(SLIMCC)/ir.inc $(SLIMCC)/ir_a64.inc $(SLIMCC)/ir_a64_emit.inc $(SLIMCC)/sched.inc $(KSLIM_CC) | $(PICOLIBC_LIB)
	@mkdir -p $(dir $@)
	$(KSLIM_CC) -std=gnu17 -DSLIMCC_LIBRARY $(KSLIM_INC) $(KSLIM_DEP) -c -o $@ $<

$(KSLIM)/rt/%.o: $(SLIMCC)/rt/%.c $(KSLIM_CC) | $(PICOLIBC_LIB)
	@mkdir -p $(dir $@)
	$(KSLIM_CC) -std=gnu17 $(KSLIM_INC) $(KSLIM_DEP) -c -o $@ $<

# picolibc を作り直したら（設定を変えるとヘッダーの picolibc.h も変わる）、カーネルの .o も作り直す
# （-MMD はシステムヘッダーを依存に含めないため）
$(OBJS) $(FATFS_OBJS) $(LWIP_OBJS) $(SLIMCC_OBJS) $(KSLIM_OBJS): $(PICOLIBC_LIB)

-include $(KSLIM_OBJS:.o=.d)

qemu-slimcc: $(KSLIM_IMG) $(SDIMG)
	qemu-system-aarch64 -M raspi3b -kernel $(KSLIM_IMG) -serial stdio -display none \
	    -drive file=$(SDIMG),if=sd,format=raw

cc-test:
	$(MAKE) -C cc test-a64

# カーネルの生成物だけを消す（picolibc のビルド結果は残す）
clean:
	rm -rf $(BUILD)/os $(BUILD)/sdcard $(BUILD)/fatfs $(BUILD)/slimcc $(KSLIM) $(BUILD)/picolibc-slimcc $(BUILD)/ld-test $(SDIMG) $(BUILD)/sdcard-blink.img $(BUILD)/sdcard-self.img $(BUILD)/sdcard-self-auto.img $(BUILD)/sdcard-smp-auto.img $(BUILD)/selfsrc $(ELF) $(IMG)

distclean:
	rm -rf $(BUILD)

# FatFs・lwIP・組み込みの slimcc の .o も（設定のヘッダー（lwipopts.h など）を変えたら作り直す。
# 入れていなかったので、lwipopts.h に SO_REUSE を足しても lwIP が作り直されなかった）
-include $(OBJS:.o=.d) $(FATFS_OBJS:.o=.d) $(LWIP_OBJS:.o=.d) $(SLIMCC_OBJS:.o=.d)
