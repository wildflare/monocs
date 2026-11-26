SRCS=alloc.c assembler.c bitint.c codegen.c hashmap.c main.c options.c parse.c platform.c preprocess.c strings.c tokenize.c type.c unicode.c

TEST_SRCS!=ls test/*.c

STG2_FLAGS=

SAN_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined

.SUFFIXES: .exe .stage2.o .stage2.exe .san.o .san.exe .filc.o .filc.exe

# Stage 1

OBJS=$(SRCS:.c=.o)

slimcc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJS): slimcc.h
parse.o parse.lib.o parse.stage2.o parse.san.o: omp.inc

# codegen.c は ir.inc（中間表現を使うコード生成。AArch64 の部分は ir_a64.inc・ir_a64_emit.inc）と sched.inc（命令の並べ替え）を取り込む
codegen.o codegen.lib.o codegen.stage2.o codegen.san.o: ir.inc ir_a64.inc ir_a64_emit.inc sched.inc

TESTS=$(TEST_SRCS:.c=.exe)

test/host/common.o: test/host/common.c
	$(CC) -o $@ -c $<

$(TESTS): slimcc test/host/common.o

.c.exe:
	$(SHELL) scripts/test_helper.sh ./slimcc $< $@

test: $(TESTS)
	for i in $(TESTS); do echo $$i; ./$$i || exit 1; done
	$(SHELL) scripts/test_driver.sh $(PWD)/slimcc $(CC)
	./slimcc -hashmap-test

# libslimcc（メモリ上でのコンパイル・リンク）と、Linux 上で確かめる slimcc-jit
LIB_SRCS=alloc.c assembler.c bitint.c codegen.c hashmap.c libslimcc.c memlink.c options.c parse.c platform.c preprocess.c strings.c tokenize.c type.c unicode.c zone.c
LIB_OBJS=$(LIB_SRCS:.c=.lib.o)

%.lib.o: %.c slimcc.h libslimcc.h
	$(CC) $(CFLAGS) -DSLIMCC_LIBRARY -c -o $@ $<

slimcc-jit: $(LIB_OBJS) test-a64/jit_main.c test/host/common.c
	$(CC) $(CFLAGS) -DSLIMCC_LIBRARY -rdynamic -o $@ test-a64/jit_main.c test/host/common.c $(LIB_OBJS) -ldl -lgcc

# Monocs のプラットフォーム（platform/monocs.c）で動かす slimcc-jit（picolibc のヘッダーとの組み合わせの確認用）
LIB_OBJS_MONOCS=$(filter-out platform.lib.o,$(LIB_OBJS)) platform/monocs.lib.o

slimcc-jit-monocs: $(LIB_OBJS_MONOCS) test-a64/jit_main.c test/host/common.c
	$(CC) $(CFLAGS) -DSLIMCC_LIBRARY -DSLIMCC_JIT_MONOCS -rdynamic -o $@ test-a64/jit_main.c test/host/common.c $(LIB_OBJS_MONOCS) -ldl -lgcc

# Monocs 向けのクロスコンパイラ（Linux で動き、Monocs のプラットフォームで .o を作る。リンクはしない）
OBJS_MONOCS=$(filter-out platform.o,$(OBJS)) platform/monocs.o

slimcc-monocs: $(OBJS_MONOCS)
	$(CC) $(CFLAGS) -o $@ $(OBJS_MONOCS) $(LDFLAGS)

platform/monocs.o: slimcc.h

# Monocs のカーネル用の静的リンカ（ld.c。make ld-test で GNU ld と比べる）
slimld: ld.c ld.h ld_main.c
	$(CC) $(CFLAGS) -o $@ ld_main.c ld.c

# slimcc-a64: AArch64 向けフロントエンドのテスト（test-a64/run.sh）
.PHONY: test-a64
test-a64: slimcc
	$(SHELL) test-a64/run.sh

# Stage 2

OBJS_S2=$(SRCS:.c=.stage2.o)

$(OBJS_S2): slimcc

.c.stage2.o:
	./slimcc $(STG2_FLAGS) -o $@ -c $<

slimcc-stage2: $(OBJS_S2)
	./slimcc $(STG2_FLAGS) -o $@ $(OBJS_S2) $(LDFLAGS)

TESTS_S2=$(TEST_SRCS:.c=.stage2.exe)

$(TESTS_S2): slimcc-stage2 test/host/common.o

.c.stage2.exe:
	$(SHELL) scripts/test_helper.sh ./slimcc-stage2 $< $@

test-stage2: $(TESTS_S2)
	for i in $(TESTS_S2); do echo $$i; ./$$i || exit 1; done
	$(SHELL) scripts/test_driver.sh $(PWD)/slimcc-stage2 $(CC)
	./slimcc-stage2 -hashmap-test

# -fsanitize build

OBJS_SAN=$(SRCS:.c=.san.o)

$(OBJS_SAN): slimcc.h

.c.san.o:
	$(CC) $(SAN_FLAGS) -g -fno-omit-frame-pointer -o $@ -c $<

slimcc-san: $(OBJS_SAN)
	$(CC) $(SAN_FLAGS) -g -fno-omit-frame-pointer -o $@ $(OBJS_SAN)

TESTS_SAN=$(TEST_SRCS:.c=.san.exe)

$(TESTS_SAN): slimcc-san test/host/common.o

.c.san.exe:
	$(SHELL) scripts/test_helper.sh ./slimcc-san $< $@

test-san: $(TESTS_SAN)
	for i in $(TESTS_SAN); do echo $$i; ./$$i || exit 1; done
	$(SHELL) scripts/test_driver.sh $(PWD)/slimcc-san $(CC)
	./slimcc-san scripts/amalgamation.c -c -o/dev/null
	./slimcc-san -hashmap-test

test-misc: slimcc-san test/host/common.o
	$(SHELL) scripts/test_abi.sh $(PWD)/slimcc-san $(CC)
	$(SHELL) scripts/test_abi.sh $(CC) $(PWD)/slimcc-san
	$(SHELL) scripts/test_include_next.sh $(PWD)/slimcc-san
	FILE=file $(SHELL) scripts/test_linker.sh $(PWD)/slimcc-san

# Fil-C build

OBJS_FILC=$(SRCS:.c=.filc.o)

$(OBJS_FILC): slimcc.h

.c.filc.o:
	$(FILC) -g -o $@ -c $<

slimcc-filc: $(OBJS_FILC)
	$(FILC) -g -o $@ $(OBJS_FILC)

TESTS_FILC=$(TEST_SRCS:.c=.filc.exe)

$(TESTS_FILC): slimcc-filc test/host/common.o

.c.filc.exe:
	$(SHELL) scripts/test_helper.sh ./slimcc-filc $< $@

test-filc: $(TESTS_FILC)
	for i in $(TESTS_FILC); do echo $$i; ./$$i || exit 1; done
	$(SHELL) scripts/test_driver.sh $(PWD)/slimcc-filc $(CC)
	./slimcc-filc scripts/amalgamation.c -c -o/dev/null
	./slimcc-filc -hashmap-test

# Misc.

test-all: test test-stage2

format:
	$(CLANG_FORMAT) -i $(SRCS) slimcc.h platform/*.c
	perl -i -p0e 's|\:[ ]+\{\n|: {\n|g' $(SRCS) slimcc.h platform/*.c

clean:
	rm -f slimcc slimcc-stage2 slimcc-san slimcc-filc
	rm -f *.o test/*.o test/*.exe test/host/*.o test/abi/*.o

.PHONY: clean test test-stage2 test-all test-san test-filc
