PROGS=scrollback

CFLAGS=-Wall -Wextra
LDLIBS=-lutil

all: $(PROGS)

install: all
	mkdir -p $(DESTDIR)/usr/bin
	cp scrollback vtdirect $(DESTDIR)/usr/bin
	mkdir -p $(DESTDIR)/usr/share/man/man1
	cp scrollback.1 $(DESTDIR)/usr/share/man/man1
	mkdir -p $(DESTDIR)/usr/share/kbd/keymaps/include/
	cp compose.scrollback $(DESTDIR)/usr/share/kbd/keymaps/include/
	mkdir -p $(DESTDIR)/usr/lib/systemd/system
	cp scrollback.service $(DESTDIR)/usr/lib/systemd/system
	mkdir -p $(DESTDIR)/usr/lib/udev/rules.d
	cp 91-scrollback.rules $(DESTDIR)/usr/lib/udev/rules.d

clean:
	rm -f $(PROGS) *.o *~ logchar logescape
