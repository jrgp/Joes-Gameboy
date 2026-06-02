#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "gb.h"
#include "savestate.h"
#include "ws_server.h"
#include "constants.h"

void sdl_init(void);
void sdl_main_impl(void);
void server_main_impl(void);
extern uint8_t RAM[];
extern void cpu_close_debug_file(void);

int main(int argc, char **argv) {
    char *rom = "tetris.gb";
    bool load_from_state = false;
    bool server_mode = false;
    int server_port = 8080;
    const char *server_bind = NULL;
    const char *ppm_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--server") == 0) {
            server_mode = true;
            headless = true;
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --port\n");
                return 1;
            }
            server_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bind") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --bind\n");
                return 1;
            }
            server_bind = argv[++i];
        } else if (strcmp(argv[i], "--gbmicrotest") == 0) {
            gbmicrotest_mode = true;
        } else if (strcmp(argv[i], "--cycles") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --cycles\n");
                return 1;
            }
            max_cycles = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --model\n");
                return 1;
            }
            const char *m = argv[++i];
            if (!gb_set_model(m)) {
                fprintf(stderr, "Unknown model: %s\n", m);
                return 1;
            }
        } else if (strcmp(argv[i], "--ppm") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --ppm\n");
                return 1;
            }
            ppm_path = argv[++i];
        } else if (strncmp(argv[i], "--", 2) == 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        } else {
            rom = argv[i];
            if (savestate_is_cbor_path(rom))
                load_from_state = true;
        }
    }

    if (load_from_state) {
        pixels_init();
        if (!headless)
            sdl_init();
        if (!load_state(rom)) {
            fprintf(stderr, "Failed to load save state: %s\n", rom);
            return 1;
        }
    } else {
        char auto_ss[4096] = {0};
        if (server_mode) {
            savestate_default_path(rom, auto_ss, sizeof(auto_ss));
            FILE *probe = fopen(auto_ss, "rb");
            if (probe) {
                fclose(probe);
                load_from_state = true;
            }
        }

        if (load_from_state) {
            pixels_init();
            if (!load_state(auto_ss)) {
                fprintf(stderr, "[server] auto-load of %s failed, booting fresh\n", auto_ss);
                load_from_state = false;
                cart_load(rom);
                mem_init();
                sav_load(rom);
                gpu_init();
                cpu_fake_init();
                pixels_init();
                joypad_init();
            } else {
                fprintf(stderr, "[server] auto-loaded savestate %s\n", auto_ss);
            }
        } else {
            cart_load(rom);
            mem_init();
            sav_load(rom);
            gpu_init();
            cpu_fake_init();
            pixels_init();
            if (!headless)
                sdl_init();
            joypad_init();
        }
    }

    if (server_mode) {
        if (!ws_server_init(server_bind, server_port)) {
            fprintf(stderr, "Failed to start WebSocket server\n");
            return 1;
        }
        server_main_impl();
        ws_server_destroy();
    } else if (headless) {
        headless_main_impl();
    } else {
        sdl_main_impl();
    }

    if (ppm_path) {
        fprintf(stderr, "PPU state at dump: LCDC=%02X LY=%02X SCX=%02X SCY=%02X WX=%02X WY=%02X BGP=%02X\n",
                RAM[LCDC], RAM[LY], RAM[SCX], RAM[SCY], RAM[WX], RAM[WY], RAM[BGP]);
        FILE *pf = fopen(ppm_path, "wb");
        if (pf) {
            fprintf(pf, "P6\n%d %d\n255\n", VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
            for (int i = 0; i < VIEWPORT_WIDTH * VIEWPORT_HEIGHT; i++) {
                uint32_t px = pixels[i];
                uint8_t rgb[3] = {(uint8_t)(px & 0xFF), (uint8_t)((px >> 8) & 0xFF), (uint8_t)((px >> 16) & 0xFF)};
                fwrite(rgb, 1, 3, pf);
            }
            fclose(pf);
            fprintf(stderr, "Frame dumped to %s\n", ppm_path);
        }
    }

    if (!load_from_state)
        sav_save(rom);

    if (savestate_rom_path[0] != '\0') {
        char ss_path[4096];
        savestate_default_path(savestate_rom_path, ss_path, sizeof(ss_path));
        save_state(ss_path);
    }

    cpu_close_debug_file();

    return 0;
}
