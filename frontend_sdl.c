#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "gb.h"
#include "savestate.h"
#include "constants.h"
#include "palette.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

void sdl_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        exit(1);
    }

    window = SDL_CreateWindow("Joe's GB", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VIEWPORT_WIDTH * 2, VIEWPORT_HEIGHT * 2, SDL_WINDOW_SHOWN);

    if (window == NULL) {
        printf("Window could not be created! SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_SetWindowResizable(window, true);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        printf("Renderer could not be created! SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    if (texture == NULL) {
        printf("Texture could not be created! SDL Error: %s\n", SDL_GetError());
        exit(1);
    }
}

static void sdl_display(void) {
    int result = SDL_UpdateTexture(texture, NULL, pixels, VIEWPORT_WIDTH * (int)sizeof(uint32_t));

    if (result != 0) {
        printf("Texture could not be updated! SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    const SDL_Rect srcr = {.x = 0, .y = 0, .w = VIEWPORT_WIDTH, .h = VIEWPORT_HEIGHT};

    result = SDL_RenderCopy(renderer, texture, &srcr, NULL);
    if (result != 0) {
        printf("SDL_RenderCopy failed. SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_RenderPresent(renderer);
}

static bool sdl_frame(void) {
    const uint32_t start_ticks = SDL_GetTicks();

    frame_headless();
    sdl_display();

    const uint32_t diff = SDL_GetTicks() - start_ticks;

    if (diff < 16U) {
        uint32_t nap_time = 16U - diff;
        if (g_fast_mode) nap_time /= 4U;
        SDL_Delay(nap_time);
    }

    return true;
}

void sdl_main_impl(void) {
    bool run = true;

    SDL_Event event;
    printf("SDL window created, starting main loop...\n");
    while (run) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    printf("got quit event - window closed\n");
                    run = false;
                    return;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    int joypad_bit = -1;
                    switch (event.key.keysym.sym) {
                        case SDLK_RIGHT:  joypad_bit = 0; break;
                        case SDLK_LEFT:   joypad_bit = 1; break;
                        case SDLK_UP:     joypad_bit = 2; break;
                        case SDLK_DOWN:   joypad_bit = 3; break;
                        case SDLK_a:      joypad_bit = 4; break;
                        case SDLK_s:      joypad_bit = 5; break;
                        case SDLK_RSHIFT: joypad_bit = 6; break;
                        case SDLK_RETURN: joypad_bit = 7; break;
                        case SDLK_F5:
                            if (event.key.type == SDL_KEYDOWN) {
                                char ss_path[4096];
                                savestate_default_path(savestate_rom_path, ss_path, sizeof(ss_path));
                                if (save_state(ss_path))
                                    printf("[savestate] saved to %s\n", ss_path);
                            }
                            break;
                        case SDLK_F6:
                            if (event.key.type == SDL_KEYDOWN) {
                                int shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                                int idx = palette_get() + (shift ? -1 : 1);
                                palette_set(idx);
                                printf("[palette] %s\n", GB_PALETTES[palette_get()].name);
                            }
                            break;
                        case SDLK_f:
                            if (event.key.type == SDL_KEYDOWN)
                                g_fast_mode = !g_fast_mode;
                            break;
                    }
                    if (joypad_bit >= 0)
                        gb_set_button(joypad_bit, event.key.type == SDL_KEYDOWN);
                    break;
                }
            }
        }

        static int frame_count = 0;
        frame_count++;
        if (frame_count <= 5)
            printf("Processing frame %d...\n", frame_count);
        run = sdl_frame();
    }
}
