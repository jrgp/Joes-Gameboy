CFLAGS       := $(shell pkg-config --cflags sdl2)
LDFLAGS      := $(shell pkg-config --libs sdl2)
CBOR_CFLAGS  := $(shell pkg-config --cflags libcbor)
CBOR_LDFLAGS := $(shell pkg-config --libs libcbor)
LWS_CFLAGS   := $(shell pkg-config --cflags libwebsockets)
LWS_LDFLAGS  := $(shell pkg-config --libs libwebsockets)
CC      := gcc
STRICT  := -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror

# ---- Test asset configuration ----
# Pinned to c-sp/game-boy-test-roms v7.0 (2024-02-25).
# To update: change CSPTEST_VERSION and CSPTEST_SHA256, then run make clean-test-assets.
CSPTEST_VERSION := v7.0
CSPTEST_URL     := https://github.com/c-sp/game-boy-test-roms/releases/download/$(CSPTEST_VERSION)/game-boy-test-roms-$(CSPTEST_VERSION).zip
CSPTEST_SHA256  := b9a9d7a1075aa35a3d07c07c34974048672d8520dca9e07a50178f5860c3832c
TESTS_ASSETS    := tests/assets
TESTS_STAMP     := $(TESTS_ASSETS)/.downloaded

DEPS_DIR := deps
DIST_DIR := dist

CBOR_SRC  := $(DEPS_DIR)/libcbor-0.8.0
CBOR_LIB  := $(DEPS_DIR)/libcbor.a        # WASM static lib (built with emcc)
CBOR_NATIVE_LIB := $(DEPS_DIR)/libcbor_native.a  # native static lib (built with gcc)
CBOR_MAC_LIB    := $(DEPS_DIR)/libcbor_mac.a     # native static lib (built with xcrun clang)
CBOR_STAMP := $(DEPS_DIR)/.cbor-src-ready  # marks source + headers extracted
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
  _wasm_set_palette,_wasm_get_palette_count,_wasm_get_palette_name,\
  _malloc,_free

WASM_SRCS := gb.c savestate.c palette.c

SRCS    := gb.c savestate.c palette.c ws_server.c frontend_sdl.c frontend_server.c main.c
SERVER_SRCS := gb.c savestate.c palette.c ws_server.c frontend_server.c main.c
HDRS    := bios.h bits.h constants.h opnames.h savestate.h ws_server.h gb.h palette.h

gb: ws_server_html.h $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(CBOR_CFLAGS) $(LWS_CFLAGS) $(STRICT) -g -DHAVE_SDL $(SRCS) -o $@ $(LDFLAGS) $(CBOR_LDFLAGS) $(LWS_LDFLAGS)

gb-server: ws_server_html.h $(CBOR_NATIVE_LIB) $(SERVER_SRCS) $(HDRS)
	$(CC) -I$(CBOR_SRC)/src $(LWS_CFLAGS) $(STRICT) -g $(SERVER_SRCS) -o $@ $(CBOR_NATIVE_LIB) $(LWS_LDFLAGS)

ws_server_html.h: frontend/index.html tools/gen_html_header.py
	python3 tools/gen_html_header.py $< > $@

asan: ws_server_html.h $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(CBOR_CFLAGS) $(LWS_CFLAGS) $(STRICT) -g -fsanitize=undefined,address -DHAVE_SDL $(SRCS) -o gb_asan $(LDFLAGS) $(CBOR_LDFLAGS) $(LWS_LDFLAGS)

test_savestate: tests/test_savestate.c savestate.c savestate.h gb.c gb.h palette.c palette.h $(HDRS)
	$(CC) $(CBOR_CFLAGS) $(STRICT) -g \
		tests/test_savestate.c savestate.c gb.c palette.c \
		-o tests/test_savestate_bin \
		$(CBOR_LDFLAGS)

test: gb test-assets
	./tests/run_tests.sh
	./tests/run_mooneye.sh

# ---- Test asset download ----
# Downloads and extracts the pinned c-sp/game-boy-test-roms bundle.
# Stamp file prevents re-downloading on subsequent runs.
test-assets: $(TESTS_STAMP)

$(TESTS_STAMP):
	@echo "[test-assets] Downloading c-sp/game-boy-test-roms $(CSPTEST_VERSION) ..."
	@mkdir -p $(TESTS_ASSETS)
	@curl -L --fail --progress-bar \
	    "$(CSPTEST_URL)" -o $(TESTS_ASSETS)/roms.zip \
	    || { echo "ERROR: download failed" >&2; rm -f $(TESTS_ASSETS)/roms.zip; exit 1; }
	@printf "[test-assets] Verifying checksum ... "
	@echo "$(CSPTEST_SHA256)  $(TESTS_ASSETS)/roms.zip" | sha256sum -c - \
	    || { echo "ERROR: checksum mismatch — archive may be corrupt" >&2; rm -f $(TESTS_ASSETS)/roms.zip; exit 1; }
	@echo "[test-assets] Extracting ..."
	@cd $(TESTS_ASSETS) && unzip -q -o roms.zip
	@rm $(TESTS_ASSETS)/roms.zip
	@touch $(TESTS_STAMP)
	@echo "[test-assets] Ready."

clean-test-assets:
	rm -rf $(TESTS_ASSETS)

wasm-deps: $(CBOR_LIB)

# --- Shared: download source and generate cmake-produced headers ---
$(CBOR_STAMP):
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
	touch $@

# --- WASM static lib (emcc) ---
$(CBOR_LIB): $(CBOR_STAMP)
	cd $(CBOR_SRC) && emcc -O2 -Isrc -c $(CBOR_WASM_SRCS)
	cd $(CBOR_SRC) && emar rcs $(abspath $(CBOR_LIB)) *.o

# --- Native static lib (gcc) — used by gb-server to avoid needing system libcbor ---
$(CBOR_NATIVE_LIB): $(CBOR_STAMP)
	mkdir -p $(DEPS_DIR)/cbor_native_obj
	cd $(DEPS_DIR)/cbor_native_obj && \
	  $(CC) -O2 -I../libcbor-0.8.0/src -c \
	    $(addprefix ../libcbor-0.8.0/,$(CBOR_WASM_SRCS))
	ar rcs $@ $(DEPS_DIR)/cbor_native_obj/*.o

# --- Mac static lib (xcrun clang) — used by the macOS .app bundle ---
$(CBOR_MAC_LIB): $(CBOR_STAMP)
	mkdir -p $(DEPS_DIR)/cbor_mac_obj
	cd $(DEPS_DIR)/cbor_mac_obj && \
	  xcrun clang -O2 \
	    -isysroot $(MAC_SDK) -mmacos-version-min=11.0 \
	    -I../libcbor-0.8.0/src -c \
	    $(addprefix ../libcbor-0.8.0/,$(CBOR_WASM_SRCS))
	xcrun ar rcs $@ $(DEPS_DIR)/cbor_mac_obj/*.o

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

wasm: wasm-deps $(DIST_DIR) $(WASM_SRCS) frontend_wasm.c $(HDRS)
	emcc -O2 \
	  -I$(CBOR_SRC)/src \
	  -sWASM=1 \
	  -sMODULARIZE=1 \
	  -sEXPORT_NAME=createGameBoy \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sFORCE_FILESYSTEM=1 \
	  -sEXPORTED_FUNCTIONS='[$(WASM_EXPORTED_FUNCTIONS)]' \
	  -sEXPORTED_RUNTIME_METHODS='["FS","UTF8ToString"]' \
	  $(WASM_SRCS) frontend_wasm.c $(CBOR_LIB) \
	  -o $(DIST_DIR)/gb.js

clean-wasm:
	rm -rf $(DEPS_DIR) $(DIST_DIR)

MAC_APP_NAME := Joe's Gameboy
MAC_APP_EXE  := JoesGameboy
MAC_APP_DIR  := build/Release
MAC_SRCS     := gb.c savestate.c palette.c frontend_mac.m
MAC_CC       := xcrun clang
MAC_SDK      := $(shell xcrun --sdk macosx --show-sdk-path)
MAC_CFLAGS   := -isysroot $(MAC_SDK) -mmacos-version-min=11.0 \
                -fobjc-arc -I. -I$(CBOR_SRC)/src
MAC_LFLAGS   := -isysroot $(MAC_SDK) -mmacos-version-min=11.0 \
                -framework Cocoa -framework CoreGraphics

.PHONY: mac
mac: $(CBOR_MAC_LIB) icon.png
	@mkdir -p "$(MAC_APP_DIR)/$(MAC_APP_NAME).app/Contents/MacOS" \
	          "$(MAC_APP_DIR)/$(MAC_APP_NAME).app/Contents/Resources"
	$(MAC_CC) $(MAC_CFLAGS) -O2 \
	  -o "$(MAC_APP_DIR)/$(MAC_APP_NAME).app/Contents/MacOS/$(MAC_APP_EXE)" \
	  $(MAC_SRCS) $(CBOR_MAC_LIB) $(MAC_LFLAGS)
	@rm -rf /tmp/AppIcon.iconset && mkdir /tmp/AppIcon.iconset
	@sips -z 16   16   icon.png --out /tmp/AppIcon.iconset/icon_16x16.png    >/dev/null
	@sips -z 32   32   icon.png --out /tmp/AppIcon.iconset/icon_16x16@2x.png >/dev/null
	@sips -z 32   32   icon.png --out /tmp/AppIcon.iconset/icon_32x32.png    >/dev/null
	@sips -z 64   64   icon.png --out /tmp/AppIcon.iconset/icon_32x32@2x.png >/dev/null
	@sips -z 128  128  icon.png --out /tmp/AppIcon.iconset/icon_128x128.png  >/dev/null
	@sips -z 256  256  icon.png --out /tmp/AppIcon.iconset/icon_128x128@2x.png >/dev/null
	@sips -z 256  256  icon.png --out /tmp/AppIcon.iconset/icon_256x256.png  >/dev/null
	@sips -z 512  512  icon.png --out /tmp/AppIcon.iconset/icon_256x256@2x.png >/dev/null
	@sips -z 512  512  icon.png --out /tmp/AppIcon.iconset/icon_512x512.png  >/dev/null
	@sips -z 1000 1000 icon.png --out /tmp/AppIcon.iconset/icon_512x512@2x.png >/dev/null
	@iconutil -c icns /tmp/AppIcon.iconset \
	  -o "$(MAC_APP_DIR)/$(MAC_APP_NAME).app/Contents/Resources/AppIcon.icns"
	@rm -rf /tmp/AppIcon.iconset
	@printf '<?xml version="1.0" encoding="UTF-8"?>\n\
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n\
<plist version="1.0"><dict>\n\
  <key>CFBundleExecutable</key>      <string>JoesGameboy</string>\n\
  <key>CFBundleIdentifier</key>      <string>com.joesgb.cgb</string>\n\
  <key>CFBundleName</key>            <string>Joe'"'"'s Gameboy</string>\n\
  <key>CFBundleDisplayName</key>     <string>Joe'"'"'s Gameboy</string>\n\
  <key>CFBundleIconFile</key>        <string>AppIcon</string>\n\
  <key>CFBundleVersion</key>         <string>1</string>\n\
  <key>CFBundlePackageType</key>     <string>APPL</string>\n\
  <key>LSMinimumSystemVersion</key>  <string>11.0</string>\n\
  <key>NSPrincipalClass</key>        <string>NSApplication</string>\n\
  <key>NSHighResolutionCapable</key> <true/>\n\
  <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>\n\
</dict></plist>\n' > "$(MAC_APP_DIR)/$(MAC_APP_NAME).app/Contents/Info.plist"
	codesign --force --sign - "$(MAC_APP_DIR)/$(MAC_APP_NAME).app"
	@echo "Built: $(MAC_APP_DIR)/$(MAC_APP_NAME).app"
	@echo "Run:   open \"$(MAC_APP_DIR)/$(MAC_APP_NAME).app\""

clean: clean-wasm
	rm -f gb gb-server gb_asan tests/test_savestate_bin ws_server_html.h
	rm -rf build DerivedData
