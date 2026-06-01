#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * WebSocket/HTTP server — remote browser frontend.
 *
 * Architecture:
 *  - The emulator core (gb.c) owns the run loop and calls us each frame.
 *  - We serve a static HTML page and stream framebuffer updates over WebSocket.
 *  - Input, save/load/reset commands arrive from the browser and are applied
 *    through the emulator's public APIs (gb_set_button, gb_reset, etc.).
 *  - All persistent state (ROM, save states, battery saves) remains on the
 *    server; the browser is a remote UI only.
 */

/* Initialise the LWS context and start listening.
 * bind_addr: interface to bind (NULL = all interfaces / 0.0.0.0)
 * port:      TCP port to listen on (default 8080)
 * Returns true on success. */
bool ws_server_init(const char *bind_addr, int port);

/* Push a new framebuffer snapshot to all connected WebSocket clients.
 * pixels: 160×144 array of uint32_t in SDL_PIXELFORMAT_RGBA32 layout
 *         (little-endian: R=bits7-0, G=bits15-8, B=bits23-16, A=bits31-24)
 *         Must be DMG greyscale (R=G=B ∈ {0xFF,0xAA,0x55,0x00}).
 * Encodes as 2bpp (5760 bytes) — 16× smaller than raw RGBA.
 * After this call, ws_server_service() must be called to actually dispatch
 * the writeable callbacks. */
void ws_server_notify_frame(const uint32_t *pixels, int w, int h);

/* Drive the LWS event loop (non-blocking). Call once per emulator frame after
 * ws_server_notify_frame(); the caller handles frame-rate pacing via usleep. */
void ws_server_service(void);

/* Tear down the LWS context and free resources. */
void ws_server_destroy(void);
