# Monocs の Makefile の解釈器の試験（tools/make/test.sh）
CC      = gcc
CFLAGS  = -Wall -std=c11 \
          -DVERSION=\"1.2\" -D NAME=x -Iinclude -I /abs
CFLAGS += -O2
LATE    = $(EARLY) late
EARLY   = early
NOW    := $(LATE) now
EARLY   = changed
SRCS    = a.c b.c   sub/c.c
OBJDIR  = build/
OBJS    = $(addprefix $(OBJDIR),$(SRCS:.c=.o))
PAT     = $(SRCS:%.c=%.d)
UNSET  ?= default
CFLAGS ?= ignored
empty   =
space   = $(empty) $(empty)
comma   = ,
X = 1
ifeq ($(X),1)
  BR = one
else ifeq ($(X),2)
  BR = two
else
  BR = other
endif
ifneq "$(X)" "1"
  BR2 = no
else
  BR2 = yes
endif
ifdef UNDEFINED_THING
  D = defined
endif
ifndef UNDEFINED_THING
  ND = notdefined
endif
ifeq ($(findstring mingw,$(shell echo hi)),)
  SH = noshell
endif
define TWO_LINES
first
second
endef
reverse = $(2) $(1)
LIST = c b a b
export EXPORTED = exp
override OVR = ovr
ALL_C = $(wildcard *.c)
NESTED_A = ok
NESTED_NAME = A
URL = http://x/y\#frag

all: $(OBJS) | dirs
	@echo $(CC)   # コマンドは動かさない
x.o: CFLAGS += -DEXTRA
