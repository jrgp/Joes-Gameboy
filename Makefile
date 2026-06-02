CFLAGS       := $(shell pkg-config --cflags sdl2)
LDFLAGS      := $(shell pkg-config --libs sdl2)
CBOR_CFLAGS  := $(shell pkg-config --cflags libcbor)
CBOR_LDFLAGS := $(shell pkg-config --libs libcbor)
LWS_CFLAGS   := $(shell pkg-config --cflags libwebsockets)
LWS_LDFLAGS  := $(shell pkg-config --libs libwebsockets)
CC      := gcc
STRICT  := -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror

DEPS_DIR := deps
DIST_DIR := dist

CBOR_SRC  := $(DEPS_DIR)/libcbor-0.8.0
CBOR_LIB  := $(DEPS_DIR)/libcbor.a
CBOR_WASM_SRCS := \
  src/cbor.c \
  src/cbor/streaming.c \
  src/cbor/internal/encoders.c \
  src/cbor/internal/builder_callbacks.c \
  src/cbor/internal/loaders.c \
  src/cbor/internal/memory_utils.c \
  src/cbor/internal/stack.c \
  src/cbor/internal/unicode.c \
  src/cbor/encoding.c \
  src/cbor/serialization.c \
  src/cbor/arrays.c \
  src/cbor/common.c \
  src/cbor/floats_ctrls.c \
  src/cbor/bytestrings.c \
  src/cbor/callbacks.c \
  src/cbor/strings.c \
  src/cbor/maps.c \
  src/cbor/tags.c \
  src/cbor/ints.c

WASM_EXPORTED_FUNCTIONS := \
  _wasm_init,_wasm_run_frame,_wasm_get_pixels_ptr,_wasm_set_buttons,\
  _wasm_save_state,_wasm_save_state_size,_wasm_get_save_state,\
  _wasm_load_state,_wasm_reset,_wasm_set_fast,\
  _wasm_get_sav_size,_wasm_get_sav,_wasm_load_sav,\
  _malloc,_free

WASM_SRCS := gb.c savestate.c

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

wasm-deps: $(CBOR_LIB)

$(CBOR_LIB):
	mkdir -p $(DEPS_DIR)
	cd $(DEPS_DIR) && \
	  curl -sL https://github.com/PJK/libcbor/archive/refs/tags/v0.8.0.tar.gz | tar xz
	mkdir -p $(CBOR_SRC)/src/cbor
	printf '%s\n' \
	  '#ifndef CBOR_EXPORT_H' \
	  '#define CBOR_EXPORT_H' \
	  '#define CBOR_EXPORT' \
	  '#define CBOR_NO_EXPORT' \
	  '#define CBOR_DEPRECATED' \
	  '#define CBOR_DEPRECATED_EXPORT CBOR_EXPORT' \
	  '#define CBOR_DEPRECATED_NO_EXPORT CBOR_NO_EXPORT' \
	  '#endif' > $(CBOR_SRC)/src/cbor/cbor_export.h
	printf '%s\n' \
	  '#ifndef LIBCBOR_CONFIGURATION_H' \
	  '#define LIBCBOR_CONFIGURATION_H' \
	  '#define CBOR_MAJOR_VERSION 0' \
	  '#define CBOR_MINOR_VERSION 8' \
	  '#define CBOR_PATCH_VERSION 0' \
	  '#define CBOR_CUSTOM_ALLOC 0' \
	  '#define CBOR_BUFFER_GROWTH 2' \
	  '#define CBOR_MAX_STACK_SIZE 2048' \
	  '#define CBOR_PRETTY_PRINTER 1' \
	  '#define CBOR_RESTRICT_SPECIFIER restrict' \
	  '#define CBOR_INLINE_SPECIFIER inline' \
	  '#endif' > $(CBOR_SRC)/src/cbor/configuration.h
	cd $(CBOR_SRC) && emcc -O2 -Isrc -c $(CBOR_WASM_SRCS)
	cd $(CBOR_SRC) && emar rcs $(abspath $(CBOR_LIB)) *.o

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

wasm: wasm-deps $(DIST_DIR) $(WASM_SRCS) $(HDRS)
	emcc -O2 \
	  -DGAMEBOY_LIB_MODE -DHEADLESS_ONLY \
	  -I$(CBOR_SRC)/src \
	  -sWASM=1 \
	  -sMODULARIZE=1 \
	  -sEXPORT_NAME=createGameBoy \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sFORCE_FILESYSTEM=1 \
	  -sEXPORTED_FUNCTIONS='[$(WASM_EXPORTED_FUNCTIONS)]' \
	  -sEXPORTED_RUNTIME_METHODS='["FS"]' \
	  $(WASM_SRCS) $(CBOR_LIB) \
	  -o $(DIST_DIR)/gb.js

clean-wasm:
	rm -rf $(DEPS_DIR) $(DIST_DIR)

clean: clean-wasm
	rm -f gb gb_asan tests/test_savestate_bin
