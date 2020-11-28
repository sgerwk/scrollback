PROGS=scrollback

CFLAGS=-Wall -Wextra
LDLIBS=-lutil

all: $(PROGS)

install: all
	cp scrollback $(DESTDIR)/usr/bin
	cp scrollback.service $(DESTDIR)/usr/lib/systemd/system

clean:
	rm -f $(PROGS) *.o *~ logchar logescape
