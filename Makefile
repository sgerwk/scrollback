PROGS=scrollback

CFLAGS=-Wall -Wextra
LDLIBS=-lutil

all: $(PROGS)

clean:
	rm -f $(PROGS) *.o *~ logchar logescape
