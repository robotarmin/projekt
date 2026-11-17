CC     = gcc
CFLAGS = -Wall -Wextra -O2

test_rtp: tests/test_rtp.c src/rtp.c src/rtp.h
	$(CC) $(CFLAGS) -o test_rtp tests/test_rtp.c src/rtp.c

test: test_rtp
	./test_rtp

clean:
	rm -f test_rtp
