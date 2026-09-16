CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
LUA_PKG ?= $(shell for p in lua5.4 lua-5.4 lua54 lua5.3 lua-5.3 lua53 lua5.2 lua-5.2 lua52 lua; do $(PKG_CONFIG) --atleast-version=5.2 $$p 2>/dev/null && { echo $$p; break; }; done)

BUILD := build
LAYER_XML := ../neuswc/protocol/wlr-layer-shell-unstable-v1.xml
TOPLEVEL_XML := ../neuswc/protocol/wlr-foreign-toplevel-management-unstable-v1.xml
WORKSPACE_XML := /usr/share/wayland-protocols/staging/ext-workspace/ext-workspace-v1.xml
XDG_XML := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
PROTOCOLS := layer-shell toplevel workspace xdg-shell
GEN_HEADERS := $(addprefix $(BUILD)/,$(addsuffix .h,$(PROTOCOLS)))
GEN_CODE := $(addprefix $(BUILD)/,$(addsuffix .c,$(PROTOCOLS)))

CFLAGS += -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wshadow -Wpedantic
CPPFLAGS += -I$(BUILD) $(shell $(PKG_CONFIG) --cflags wayland-client cairo pangocairo $(LUA_PKG))
LDLIBS += $(shell $(PKG_CONFIG) --libs wayland-client cairo pangocairo $(LUA_PKG))

.PHONY: all clean
all: charabar

charabar: charabar.c $(GEN_HEADERS) $(GEN_CODE)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ charabar.c $(GEN_CODE) $(LDLIBS)

$(BUILD):
	mkdir -p $@

$(BUILD)/layer-shell.h: $(LAYER_XML) | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@
$(BUILD)/layer-shell.c: $(LAYER_XML) | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@
$(BUILD)/toplevel.h: $(TOPLEVEL_XML) | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@
$(BUILD)/toplevel.c: $(TOPLEVEL_XML) | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@
$(BUILD)/workspace.h: $(WORKSPACE_XML) | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@
$(BUILD)/workspace.c: $(WORKSPACE_XML) | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@
$(BUILD)/xdg-shell.h: $(XDG_XML) | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@
$(BUILD)/xdg-shell.c: $(XDG_XML) | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -rf charabar $(BUILD)
