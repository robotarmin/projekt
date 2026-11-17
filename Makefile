# aes67rx - AES67-2023 vevo
#
#   make            release build
#   make debug      ASan + UBSan, -O0 -g
#   make clean
#
# Fuggosegek (Raspberry Pi OS / Debian):
#   sudo apt install build-essential libasound2-dev linuxptp

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread
LDFLAGS ?= -pthread
LDLIBS  ?= -lasound -lm

TARGET := aes67rx
SRCS   := rtp.c session.c sap.c ptp_slave.c jitter.c udp_receiver.c audio_output.c main.c
OBJS   := $(SRCS:.c=.o)
HDRS   := aes67.h

.PHONY: all clean debug

all: $(TARGET) tai_offset

tai_offset: tai_offset.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) tai_offset

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O0 -g -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-pthread -fsanitize=address,undefined" all
