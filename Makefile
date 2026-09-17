CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

pCad: pCad.c
	$(CC) -O2 -Wall -Wextra -o pCad pCad.c -lm

install: pCad
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 pCad $(DESTDIR)$(PREFIX)/bin/pCad

clean:
	rm -f pCad
