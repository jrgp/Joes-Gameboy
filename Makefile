CFLAGS := $(shell pkg-config --cflags sdl2)
LDFLAGS := $(shell pkg-config --libs sdl2)
CC := gcc

gb: gb.c bios.h bits.h constants.h opnames.h
	$(CC) $(CFLAGS) -Wall -g -pedantic -Wshadow -Wpointer-arith -Wcast-qual $< -o $@ $(LDFLAGS)

test: gb
	./tests/run_tests.sh

clean:
	rm -f gb
