# charaWC and its control client.
CC         ?= cc
PKG_CONFIG ?= pkg-config
LUA_PKG    ?= $(shell for p in lua5.4 lua-5.4 lua54 lua5.3 lua-5.3 lua53 lua5.2 lua-5.2 lua52 lua; do $(PKG_CONFIG) --atleast-version=5.2 $$p 2>/dev/null && { echo $$p; break; }; done)

VERSION    ?= 0.1.0

# PNG=0 builds without libspng. Wallpaper images are then unavailable and
# appearance.wallpaper.background is used on its own.
PNG        ?= 1
PKGS        = swc xcursor xkbcommon $(LUA_PKG)
ifeq ($(PNG),0)
CPPFLAGS   += -DCHARA_NO_PNG
else
PKGS       += spng
endif
CFLAGS     += -std=c11 -D_GNU_SOURCE -DVERSION=\"$(VERSION)\" \
              -Wall -Wextra -Wshadow -Wno-unused-parameter -O2
CPPFLAGS   += -Iinclude $(shell $(PKG_CONFIG) --cflags $(PKGS)) $(EXTRA_CPPFLAGS)
LDLIBS     += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm
LDFLAGS    += $(EXTRA_LDFLAGS)

SRC   = src/charawc.c src/window.c src/border.c src/ipc.c \
        src/config.c src/bindings.c src/startup.c src/wallpaper.c src/log.c \
        src/tiling.c src/tiling_layout.c
HDR   = $(wildcard include/*.h)

.PHONY: all clean test
all: charawc charactl

# Switching PNG on or off changes how charawc is linked but touches no source
# file, so carry the setting in a stamp the binary depends on. Without it an
# existing charawc would be considered up to date and silently keep the old
# setting.
.png-$(PNG):
	@rm -f .png-* && touch $@

charawc: $(SRC) $(HDR) Makefile .png-$(PNG)
	@$(PKG_CONFIG) --atleast-version=5.2 "$(LUA_PKG)" || \
	  { echo "Lua 5.2+ development files are required; set LUA_PKG" >&2; exit 1; }
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

charactl: src/charactl.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/charactl.c

# The unit tests link the real window manager sources against stubs, so they
# run without a compositor, a session or a graphics card.
TESTS = test/focus_test test/tiling_test
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

FOCUS_SRC = src/window.c src/border.c src/tiling.c src/tiling_layout.c src/log.c
test/focus_test: test/focus_test.c $(FOCUS_SRC) $(HDR) Makefile
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
	  -Wl,-rpath,$(shell $(PKG_CONFIG) --variable=libdir swc) \
	  -o $@ test/focus_test.c $(FOCUS_SRC) $(LDLIBS)

# The layout engine calls nothing, so its test needs neither stubs nor swc:
# link it on its own, which is also the check that it stayed that way.
test/tiling_test: test/tiling_test.c src/tiling_layout.c include/tiling.h Makefile
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
	  -o $@ test/tiling_test.c src/tiling_layout.c -lm

clean:
	rm -f charawc charactl .png-* $(TESTS)
