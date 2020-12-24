PROGS=scrollback

CFLAGS=-Wall -Wextra
LDLIBS=-lutil

all: $(PROGS)

install: all
	cp scrollback vtdirect $(DESTDIR)/usr/bin
	cp scrollback.service $(DESTDIR)/usr/lib/systemd/system
	cp scrollback.1 $(DESTDIR)/usr/share/man/man1

clean:
	rm -f $(PROGS) *.o *~ logchar logescape
