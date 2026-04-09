CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-result -pthread

fast_snapshot_poc: fast_snapshot_poc.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f fast_snapshot_poc /tmp/fast_snapshot_poc.dat

.PHONY: clean
