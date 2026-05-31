CFLAGS       := $(shell pkg-config --cflags sdl2)
LDFLAGS      := $(shell pkg-config --libs sdl2)
CBOR_CFLAGS  := $(shell pkg-config --cflags libcbor)
CBOR_LDFLAGS := $(shell pkg-config --libs libcbor)
LWS_CFLAGS   := $(shell pkg-config --cflags libwebsockets)
LWS_LDFLAGS  := $(shell pkg-config --libs libwebsockets)
CC      := gcc
STRICT  := -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror

SRCS    := gb.c savestate.c ws_server.c
HDRS    := bios.h bits.h constants.h opnames.h savestate.h ws_server.h

gb: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(CBOR_CFLAGS) $(LWS_CFLAGS) $(STRICT) -g $(SRCS) -o $@ $(LDFLAGS) $(CBOR_LDFLAGS) $(LWS_LDFLAGS)

asan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(CBOR_CFLAGS) $(LWS_CFLAGS) $(STRICT) -g -fsanitize=undefined,address $(SRCS) -o gb_asan $(LDFLAGS) $(CBOR_LDFLAGS) $(LWS_LDFLAGS)

test_savestate: tests/test_savestate.c savestate.c savestate.h $(HDRS)
	$(CC) $(CFLAGS) $(CBOR_CFLAGS) $(STRICT) -g -DGAMEBOY_LIB_MODE \
		tests/test_savestate.c savestate.c gb.c \
		-o tests/test_savestate_bin \
		$(LDFLAGS) $(CBOR_LDFLAGS)

test: gb
	./tests/run_tests.sh

clean:
	rm -f gb gb_asan tests/test_savestate_bin
