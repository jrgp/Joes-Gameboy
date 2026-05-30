CFLAGS  := $(shell pkg-config --cflags sdl2)
LDFLAGS := $(shell pkg-config --libs sdl2)
CC      := gcc
STRICT  := -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror

gb: gb.c bios.h bits.h constants.h opnames.h
	$(CC) $(CFLAGS) $(STRICT) -g $< -o $@ $(LDFLAGS)

asan: gb.c bios.h bits.h constants.h opnames.h
	$(CC) $(CFLAGS) $(STRICT) -g -fsanitize=undefined,address $< -o gb_asan $(LDFLAGS)

test: gb
	./tests/run_tests.sh

clean:
	rm -f gb gb_asan
