#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "gb.h"
#include "savestate.h"

EMSCRIPTEN_KEEPALIVE
void wasm_init(const uint8_t *rom_data, int rom_size) {
    FILE *f = fopen("/rom.gb", "wb");
    if (f) {
        fwrite(rom_data, 1, (size_t)rom_size, f);
        fclose(f);
    }
    cart_load("/rom.gb");
    mem_init();
    sav_load("/rom.gb");
    gpu_init();
    cpu_fake_init();
    pixels_init();
    joypad_init();
}

EMSCRIPTEN_KEEPALIVE
void wasm_run_frame(void) {
    frame_headless();
}

EMSCRIPTEN_KEEPALIVE
uint32_t *wasm_get_pixels_ptr(void) {
    return pixels;
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_buttons(int mask) {
    for (int i = 0; i < 8; i++)
        gb_set_button(i, ((mask >> i) & 1) != 0);
}

EMSCRIPTEN_KEEPALIVE
void wasm_save_state(void) {
    save_state("/save.cbor");
}

EMSCRIPTEN_KEEPALIVE
int wasm_save_state_size(void) {
    FILE *f = fopen("/save.cbor", "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    int sz = (int)ftell(f);
    fclose(f);
    return sz;
}

EMSCRIPTEN_KEEPALIVE
void wasm_get_save_state(uint8_t *out, int size) {
    FILE *f = fopen("/save.cbor", "rb");
    if (!f) return;
    (void)fread(out, 1, (size_t)size, f);
    fclose(f);
}

EMSCRIPTEN_KEEPALIVE
int wasm_load_state(const uint8_t *data, int size) {
    FILE *f = fopen("/save.cbor", "wb");
    if (!f) return 0;
    fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return load_state("/save.cbor") ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void wasm_reset(void) {
    gb_reset();
}

EMSCRIPTEN_KEEPALIVE
void wasm_set_fast(int on) {
    g_fast_mode = on ? true : false;
}

EMSCRIPTEN_KEEPALIVE
int wasm_get_sav_size(void) {
    return cart_ram_size;
}

EMSCRIPTEN_KEEPALIVE
void wasm_get_sav(uint8_t *out) {
    if (cart_ram_size > 0)
        memcpy(out, ext_ram, (size_t)cart_ram_size);
}

EMSCRIPTEN_KEEPALIVE
void wasm_load_sav(const uint8_t *data, int size) {
    if (size > 0x8000) size = 0x8000;
    memcpy(ext_ram, data, (size_t)size);
    ext_ram_dirty = true;
}

#endif /* __EMSCRIPTEN__ */
