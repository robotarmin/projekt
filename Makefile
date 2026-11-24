# A kód mesterséges intelligencia segítségével készült. Ezt az egyetemi szabályzatnak megfelelően itt jelölöm. [AI1]
# aes67rx - AES67 vevo.  Fuggosegek: gcc, make, ALSA fejlecek (libasound2-dev / alsa-lib), linuxptp
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
OBJS    = main.o sdp.o ptp.o rx.o jitter.o audio.o

all: aes67rx tai_offset

aes67rx: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lasound -lm -pthread

%.o: %.c aes67.h
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

tai_offset: tai_offset.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OBJS) aes67rx tai_offset

.PHONY: all clean
