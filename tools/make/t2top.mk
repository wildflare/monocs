# t2top: Lua の一番上の Makefile と同じ形。src（sub）の make を呼ぶだけ
PLAT= guess
PLATS= guess linux posix
all:	$(PLAT)
$(PLATS) clean:
	cd sub && $(MAKE) $@
