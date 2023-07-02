.POSIX:

CONFIGFILE = config.mk
include $(CONFIGFILE)

OBJ = xcman.o
HDR =

all: xcman
$(OBJ): $(HDR)

xcman: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

.c.o:
	$(CC) -c -o $@ $< $(CFLAGS) $(CPPFLAGS)

install: xcman
	mkdir -p -- "$(DESTDIR)$(PREFIX)/bin"
	mkdir -p -- "$(DESTDIR)$(MANPREFIX)/man1"
	mkdir -p -- "$(DESTDIR)$(PREFIX)/share/licenses/xcman"
	cp -- xcman "$(DESTDIR)$(PREFIX)/bin"
	cp -- xcman.1 "$(DESTDIR)$(MANPREFIX)/man1"
	cp -- LICENSE "$(DESTDIR)$(PREFIX)/share/licenses/xcman"

uninstall:
	-rm -f -- "$(DESTDIR)$(PREFIX)/bin/xcman"
	-rm -f -- "$(DESTDIR)$(MANPREFIX)/man1/xcman.1"
	-rm -rf -- "$(DESTDIR)$(PREFIX)/share/licenses/xcman"

clean:
	-rm -f -- xcman

.SUFFIXES:
.SUFFIXES: .c .o

.PHONY: all install uninstall clean
