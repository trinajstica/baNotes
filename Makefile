CC=gcc
CFLAGS=`pkg-config --cflags gtk+-3.0 ayatana-appindicator3-0.1` -Iinclude -Wall -g
LDFLAGS=`pkg-config --libs gtk+-3.0 ayatana-appindicator3-0.1`
SRC=main.c src/app.c
OBJ=$(SRC:.c=.o)
TARGET=baNotes

# Installation paths
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR = $(PREFIX)/bin
SVGICONDIR = $(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOPDIR = $(PREFIX)/share/applications
ICONTHEME = $(PREFIX)/share/icons/hicolor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

	# Install desktop entry
	if [ -f baNotes.desktop ]; then \
		install -d $(DESTDIR)$(DESKTOPDIR); \
		install -m 0644 baNotes.desktop $(DESTDIR)$(DESKTOPDIR)/baNotes.desktop; \
	fi

	# Note: we prefer SVG icons; PNG icon support removed
	
	# Install SVG icon if present
	if [ -f baNotes.svg ]; then \
		install -d $(DESTDIR)$(SVGICONDIR); \
		install -m 0644 baNotes.svg $(DESTDIR)$(SVGICONDIR)/baNotes.svg; \
	fi

	# Update desktop database and icon cache if tools are available
	if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database $(DESTDIR)$(DESKTOPDIR) >/dev/null 2>&1 || true; \
	fi
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f -t $(DESTDIR)$(ICONTHEME) >/dev/null 2>&1 || true; \
	fi

uninstall:
	 rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	 if [ -f $(DESTDIR)$(DESKTOPDIR)/baNotes.desktop ]; then \
		rm -f $(DESTDIR)$(DESKTOPDIR)/baNotes.desktop; \
	fi
	# PNG uninstall steps removed; only SVG icon uninstall handled below
	 if [ -f $(DESTDIR)$(SVGICONDIR)/baNotes.svg ]; then \
		rm -f $(DESTDIR)$(SVGICONDIR)/baNotes.svg; \
	fi

	# Update desktop database and icon cache after uninstall
	if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database $(DESTDIR)$(DESKTOPDIR) >/dev/null 2>&1 || true; \
	fi
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f -t $(DESTDIR)$(ICONTHEME) >/dev/null 2>&1 || true; \
	fi

.PHONY: all clean install uninstall
