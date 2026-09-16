# charaWC and its control client.
CC         ?= cc
PKG_CONFIG ?= pkg-config
LUA_PKG    ?= $(shell for p in lua5.4 lua-5.4 lua54 lua5.3 lua-5.3 lua53 lua5.2 lua-5.2 lua52 lua; do $(PKG_CONFIG) --atleast-version=5.2 $$p 2>/dev/null && { echo $$p; break; }; done)

VERSION    ?= 0.1.0
PKGS        = swc spng xcursor xkbcommon $(LUA_PKG)
CFLAGS     += -std=c11 -D_GNU_SOURCE -DVERSION=\"$(VERSION)\" \
              -Wall -Wextra -Wshadow -Wno-unused-parameter -O2
CPPFLAGS   += -Iinclude $(shell $(PKG_CONFIG) --cflags $(PKGS)) $(EXTRA_CPPFLAGS)
LDLIBS     += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
LDFLAGS    += $(EXTRA_LDFLAGS)

SRC   = src/charawc.c src/window.c src/layout.c src/border.c src/ipc.c \
        src/config.c src/bindings.c src/startup.c src/wallpaper.c src/log.c
HDR   = $(wildcard include/*.h)

.PHONY: all clean
all: charawc charactl

charawc: $(SRC) $(HDR) Makefile
	@$(PKG_CONFIG) --atleast-version=5.2 "$(LUA_PKG)" || \
	  { echo "Lua 5.2+ development files are required; set LUA_PKG" >&2; exit 1; }
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

charactl: src/charactl.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/charactl.c

clean:
	rm -f charawc charactl
