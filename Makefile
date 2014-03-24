SRCDIR=./udpscanner
CC=gcc
CFLAGS=-I$(SRCDIR) -Wall -Werror

udpscan: $(SRCDIR)/udpscanner.c $(SRCDIR)/udpscanner.h
        $(CC) $(CFLAGS) -o $@ $(SRCDIR)/udpscanner.c

.PHONY: clean

clean:
        rm -f $(SRCDIR)/udpscanner.o
        rm -f udpscan