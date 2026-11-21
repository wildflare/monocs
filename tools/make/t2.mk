# t2: Lua 5.4 の src/Makefile と同じ形（2026-11-16）。既定のゴールはコマンドで make を呼び直す（Monocs は動かさない）ので
# all を見る。all の最初の .a はライブラリ（プログラムにしない）。プログラムの前提の .a は、その .o に開く
PLAT= guess
CC= gcc -std=gnu99
CFLAGS= -O2 -Wall -DCOMPAT_X $(SYSCFLAGS) $(MYCFLAGS)
SYSCFLAGS=
MYCFLAGS=
LIB_A=	libx.a
CORE_O=	core1.o core2.o
LIB_O=	lib1.o
BASE_O= $(CORE_O) $(LIB_O) $(MYOBJS)
PROG_T=	prog
PROG_O=	prog.o
TOOL_T=	tool
TOOL_O=	tool.o
ALL_T= $(LIB_A) $(PROG_T) $(TOOL_T)

default: $(PLAT)

all:	$(ALL_T)

$(LIB_A): $(BASE_O)
	ar rcu $@ $(BASE_O)

$(PROG_T): $(PROG_O) $(LIB_A)
	$(CC) -o $@ $(PROG_O) $(LIB_A)

$(TOOL_T): $(TOOL_O) $(LIB_A)
	$(CC) -o $@ $(TOOL_O) $(LIB_A)

guess:
	@echo Guessing `uname`
	@$(MAKE) `uname`

linux: linux-noreadline

linux-noreadline:
	$(MAKE) $(ALL) SYSCFLAGS="-DUSE_LINUX" SYSLIBS="-Wl,-E -ldl"

posix:
	$(MAKE) $(ALL) SYSCFLAGS="-DUSE_POSIX"

ALL= all
