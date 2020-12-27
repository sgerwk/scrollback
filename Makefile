PROGS=scrollback

CFLAGS=-Wall -Wextra
LDLIBS=-lutil

all: $(PROGS)

install: all
	cp scrollback vtdirect $(DESTDIR)/usr/bin
	cp scrollback.1 $(DESTDIR)/usr/share/man/man1
	cp compose.scrollback $(DESTDIR)/usr/share/kbd/keymaps/include/

clean:
	rm -f $(PROGS) *.o *~ logchar logescape
