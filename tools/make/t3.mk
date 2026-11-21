# QuickJS の形（2026-11-21）: レシピのない all の前提に、フォルダ・別のオプションの .o（check）が先に並ぶ。
# .PHONY、$(shell cat VERSION)、order-only の前提（| $(OBJDIR)）、パターンの規則
OBJDIR=.obj
DEFINES:=-D_GNU_SOURCE -DCONFIG_VERSION=\"$(shell cat VERSION)\"
CFLAGS=-O2 -Wall $(DEFINES)
PROGS=qjs
all: $(OBJDIR) $(OBJDIR)/core.check.o $(PROGS)
QJS_OBJS=$(OBJDIR)/qjs.o $(OBJDIR)/core.o $(OBJDIR)/libm/util.o
qjs: $(QJS_OBJS)
	$(CC) -o $@ $^
$(OBJDIR):
	mkdir -p $(OBJDIR) $(OBJDIR)/libm
$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJDIR)/%.check.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DCONFIG_CHECK_JSVALUE -c -o $@ $<
clean:
	rm -rf $(OBJDIR) qjs
.PHONY: all clean
