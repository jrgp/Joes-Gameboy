/*
 * ws_server.c — WebSocket/HTTP server for the browser-based remote frontend.
 *
 * One LWS protocol handles both HTTP (serving the page) and WebSocket
 * (streaming frames + receiving input/commands).
 *
 * Wire protocol (binary WebSocket messages):
 *   Server → Client:
 *     [0x01] + 5760 bytes  — framebuffer update, 2bpp packed
 *                            4 pixels per byte, MSB-first, palette indices 0-3
 *                            Palette: 0=white(FF), 1=lt-grey(AA), 2=dk-grey(55), 3=black(00)
 *
 *   Client → Server:
 *     [0x02, bitmask]   — joypad update (bit0=RIGHT,1=LEFT,2=UP,3=DOWN,
 *                                         bit4=A,5=B,6=SELECT,7=START)
 *     [0x03]            — save state
 *     [0x04]            — load state
 *     [0x05]            — reset
 */

#include "ws_server.h"
#include "savestate.h"

#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

/* ---- Emulator interfaces (defined in gb.c) ---- */

/* Set/clear a joypad button and fire the joypad interrupt if state changed.
 * Bits: 0=RIGHT, 1=LEFT, 2=UP, 3=DOWN, 4=A, 5=B, 6=SELECT, 7=START */
extern void gb_set_button(int bit, bool pressed);

/* Soft-reset: save battery RAM, re-initialise all subsystems, reload battery RAM. */
extern void gb_reset(void);

/* Path of the currently loaded ROM (set by cart_load in gb.c). */
extern char savestate_rom_path[4096];

/* ---- Embedded HTML frontend (generated from frontend/index.html) ---- */

#include "ws_server_html.h"


/* ---- Global server state ---- */

/* 2bpp wire format: type byte + 160*144/4 packed bytes (4 pixels/byte, MSB-first).
 * DMG has exactly 4 colours so each pixel fits in 2 bits.  Frame is 16x smaller
 * than raw RGBA (5761 vs 92161 bytes), dramatically reducing send-buffer pressure. */
#define WS_FRAME_2BPP_PAYLOAD (160 * 144 / 4)          /* 5760 bytes */
#define WS_FRAME_BYTES        (1 + WS_FRAME_2BPP_PAYLOAD) /* + type byte */

/* Latest encoded frame and a monotonically-increasing sequence counter.
 * Incremented by ws_server_notify_frame(); per-connection last_sent_seq
 * prevents re-sending the same frame twice (frame-drop guard). */
static uint8_t   g_frame_2bpp[WS_FRAME_2BPP_PAYLOAD];
static uint32_t  g_frame_seq = 1;   /* start at 1; per-conn inits to 0 → first frame always sent */

/* Send buffer: LWS_PRE headroom + frame payload (static, single-threaded). */
static unsigned char g_send_buf[LWS_PRE + WS_FRAME_BYTES];

static struct lws_context *g_context = NULL;

/* ---- Per-connection state ---- */

typedef struct {
    bool     is_ws;          /* true after WebSocket upgrade (ESTABLISHED) */
    bool     html_sent;      /* true after HTTP body has been written */
    uint32_t last_sent_seq;  /* seq of the last frame sent to this client */
} per_session_t;

/* ---- LWS callback ---- */

static int callback_gb(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    per_session_t *pss = (per_session_t *)user;

    switch (reason) {

    /* ---- HTTP: serve the frontend page ---- */
    case LWS_CALLBACK_HTTP: {
        unsigned char hdr[LWS_PRE + 512];
        unsigned char *start = hdr + LWS_PRE;
        unsigned char *p     = start;
        unsigned char *end   = hdr + sizeof(hdr) - 1;

        if (pss) { pss->is_ws = false; pss->html_sent = false; }

        /* Access log */
        char peer[64] = "?";
        lws_get_peer_simple(wsi, peer, sizeof(peer));
        fprintf(stderr, "[ws] HTTP GET %s from %s\n",
                in ? (const char *)in : "/", peer);

        if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
                "text/html; charset=utf-8",
                (lws_filepos_t)(sizeof(HTML_PAGE) - 1),
                &p, end))
            return 1;
        if (lws_finalize_write_http_header(wsi, start, &p, end))
            return 1;

        lws_callback_on_writable(wsi);
        return 0;
    }

    case LWS_CALLBACK_HTTP_WRITEABLE: {
        if (!pss || pss->is_ws) return 0;
        if (pss->html_sent) {
            if (lws_http_transaction_completed(wsi)) return -1;
            return 0;
        }
        size_t body_len = sizeof(HTML_PAGE) - 1;
        unsigned char *buf = (unsigned char *)malloc(LWS_PRE + body_len);
        if (!buf) return -1;
        memcpy(buf + LWS_PRE, HTML_PAGE, body_len);
        lws_write(wsi, buf + LWS_PRE, body_len, LWS_WRITE_HTTP_FINAL);
        free(buf);
        pss->html_sent = true;
        if (lws_http_transaction_completed(wsi)) return -1;
        return 0;
    }

    /* ---- WebSocket: connection established ---- */
    case LWS_CALLBACK_ESTABLISHED: {
        if (pss) {
            pss->is_ws = true;
            pss->last_sent_seq = 0;  /* ensure first frame is sent immediately */
            lws_callback_on_writable(wsi);
        }
        char peer[64] = "?";
        lws_get_peer_simple(wsi, peer, sizeof(peer));
        fprintf(stderr, "[ws] WS connected: %s\n", peer);
        return 0;
    }

    /* ---- WebSocket: connection closed ---- */
    case LWS_CALLBACK_CLOSED: {
        char peer[64] = "?";
        lws_get_peer_simple(wsi, peer, sizeof(peer));
        fprintf(stderr, "[ws] WS closed:     %s\n", peer);
        return 0;
    }
    /* ---- WebSocket: data received from browser ---- */
    case LWS_CALLBACK_RECEIVE: {
        if (len < 1) return 0;
        const uint8_t *data = (const uint8_t *)in;

        switch (data[0]) {

        case 0x02: /* Joypad update: [0x02, bitmask] */
            if (len >= 2) {
                uint8_t mask = data[1];
                for (int bit = 0; bit < 8; bit++)
                    gb_set_button(bit, (mask >> bit) & 1);
            }
            break;

        case 0x03: { /* Save state */
            char path[4096];
            savestate_default_path(savestate_rom_path, path, sizeof(path));
            if (!save_state(path))
                fprintf(stderr, "[ws] save_state failed: %s\n", path);
            break;
        }

        case 0x04: { /* Load state */
            char path[4096];
            savestate_default_path(savestate_rom_path, path, sizeof(path));
            if (!load_state(path))
                fprintf(stderr, "[ws] load_state failed: %s\n", path);
            break;
        }

        case 0x05: /* Reset */
            gb_reset();
            break;

        case 0x06: /* Fast mode toggle */
            g_fast_mode = !g_fast_mode;
            break;

        default:
            break;
        }
        return 0;
    }

    /* ---- WebSocket: socket is ready to send ---- */
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!pss || !pss->is_ws) return 0;
        /* Frame-drop guard: skip if we already sent this frame to this client.
         * lws_callback_on_writable_all_protocol() is idempotent so multiple
         * notify_frame() calls between service ticks collapse to one send. */
        if (pss->last_sent_seq == g_frame_seq) return 0;
        /* Send current frame: [0x01] + 2bpp payload */
        g_send_buf[LWS_PRE] = 0x01;
        memcpy(g_send_buf + LWS_PRE + 1, g_frame_2bpp, WS_FRAME_2BPP_PAYLOAD);
        lws_write(wsi, g_send_buf + LWS_PRE, (size_t)WS_FRAME_BYTES, LWS_WRITE_BINARY);
        pss->last_sent_seq = g_frame_seq;
        return 0;
    }

    default:
        break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

/* ---- Protocol table ---- */

static struct lws_protocols protocols[] = {
    {
        "http",            /* name — also accepts unnamed WS upgrades */
        callback_gb,
        sizeof(per_session_t),
        4096,              /* rx_buffer_size */
        0, NULL, 0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }  /* terminator */
};

/* ---- Public API ---- */

bool ws_server_init(const char *bind_addr, int port)
{
    /* Suppress LWS log noise on stdout (errors still go to stderr). */
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port       = port;
    info.iface      = bind_addr;   /* NULL = all interfaces */
    info.protocols  = protocols;
    info.options    = 0;

    g_context = lws_create_context(&info);
    if (!g_context) {
        fprintf(stderr, "[ws] lws_create_context failed\n");
        return false;
    }

    fprintf(stderr, "[ws] listening on port %d\n", port);
    return true;
}

void ws_server_notify_frame(const uint32_t *pixels, int w, int h)
{
    /* Encode framebuffer as 2bpp (4 pixels per byte, MSB-first).
     * DMG greyscale palette:
     *   index 0 = white   (R=0xFF)
     *   index 1 = lt-grey (R=0xAA)
     *   index 2 = dk-grey (R=0x55)
     *   index 3 = black   (R=0x00)
     * We use the R channel (= G = B for greyscale) to identify the colour. */
    int n = w * h;
    for (int i = 0; i < n; i += 4) {
        uint8_t byte = 0;
        for (int j = 0; j < 4; j++) {
            uint8_t r = (uint8_t)(pixels[i + j] & 0xFFu);
            uint8_t idx;
            if      (r == 0xFFu) idx = 0;
            else if (r == 0xAAu) idx = 1;
            else if (r == 0x55u) idx = 2;
            else                 idx = 3;
            byte |= (uint8_t)(idx << (6 - j * 2));
        }
        g_frame_2bpp[i / 4] = byte;
    }

    g_frame_seq++;

    /* Schedule a write callback for every connected WebSocket client. */
    if (g_context)
        lws_callback_on_writable_all_protocol(g_context, &protocols[0]);
}

void ws_server_service(void)
{
    if (g_context)
        lws_service(g_context, 0);  /* non-blocking; caller handles sleep */
}

void ws_server_destroy(void)
{
    if (g_context) {
        lws_context_destroy(g_context);
        g_context = NULL;
    }
}
