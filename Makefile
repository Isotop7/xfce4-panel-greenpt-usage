CC = gcc
PKG_CONFIG = pkg-config
PKGS = libxfce4panel-2.0 gtk+-3.0 "glib-2.0 >= 2.68" "libcurl >= 7.32.0"

CFLAGS = -Wall -Wextra -O2 -fPIC $(shell $(PKG_CONFIG) --cflags $(PKGS))
LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

SHARE = $(HOME)/.local/share
# The panel scans user plugins under ~/.local/<panel-libdir-minus-prefix>/...
# (see panel-module-factory.c), so derive the suffix relative to the prefix:
# "lib64" on Fedora, "lib/x86_64-linux-gnu" on Debian/Ubuntu.
PANEL_PREFIX = $(shell $(PKG_CONFIG) --variable=prefix libxfce4panel-2.0 2>/dev/null)
PANEL_LIBDIR = $(shell $(PKG_CONFIG) --variable=libdir libxfce4panel-2.0 2>/dev/null)
LIBDIR_SUFFIX = $(patsubst $(PANEL_PREFIX)/%,%,$(PANEL_LIBDIR))
ifeq ($(LIBDIR_SUFFIX),$(PANEL_LIBDIR))
LIBDIR_SUFFIX =
endif

TARGET = libgreenpt.so
PLUGIN_LIBDIR = $(HOME)/.local/$(LIBDIR_SUFFIX)/xfce4/panel/plugins
PLUGIN_DESKTOPDIR = $(SHARE)/xfce4/panel/plugins
ICONDIR = $(SHARE)/icons/hicolor/scalable/apps

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): src/greenpt-plugin.c
	$(CC) $(CFLAGS) -shared -Wl,--as-needed -o $@ $< $(LIBS)

install: $(TARGET)
	@test -n "$(LIBDIR_SUFFIX)" || { \
	  echo "error: cannot determine the xfce4-panel plugin libdir; is xfce4-panel-devel installed?" >&2; exit 1; }
	mkdir -p $(PLUGIN_LIBDIR) $(PLUGIN_DESKTOPDIR) $(ICONDIR)
	install -m 755 $(TARGET) $(PLUGIN_LIBDIR)/$(TARGET)
	install -m 644 greenpt.desktop $(PLUGIN_DESKTOPDIR)/greenpt.desktop
	install -m 644 data/greenpt.svg $(ICONDIR)/greenpt.svg

uninstall:
	rm -f $(PLUGIN_LIBDIR)/$(TARGET) $(PLUGIN_DESKTOPDIR)/greenpt.desktop $(ICONDIR)/greenpt.svg

clean:
	rm -f $(TARGET) *.o
