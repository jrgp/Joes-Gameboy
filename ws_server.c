/*
 * ws_server.c — WebSocket/HTTP server for the browser-based remote frontend.
 *
 * One LWS protocol handles both HTTP (serving the page) and WebSocket
 * (streaming frames + receiving input/commands).
 *
 * Transport modes (per connection, selected by client via message 0x07):
 *   TRANSPORT_RELIABLE — always send full framebuffers (default).
 *                        Best for LAN and low-latency links.
 *   TRANSPORT_FAST     — tile-diff with periodic keyframes.
 *                        Best for VPN and bandwidth-constrained links.
 *
 * Wire protocol (binary WebSocket messages):
 *   Server → Client:
 *     [0x01] + 5760 bytes
 *         Full framebuffer, 2bpp packed, 4 pixels/byte, MSB-first.
 *         Palette indices 0-3: 0=white(FF), 1=lt-grey(AA), 2=dk-grey(55), 3=black(00).
 *         Always sent in TRANSPORT_RELIABLE mode.
 *         Sent in TRANSPORT_FAST mode on connection, forced keyframe events
 *         (reset, load-state), periodic keyframe interval, or when
 *         >TILE_FULL_THRESHOLD tiles changed in one frame.
 *
 *     [0x10] + [count_lo, count_hi: uint16 LE] + count × tile_entry
 *         Tile update batch.  Only changed 8×8 tiles are sent.
 *         tile_entry = [tile_x: u8, tile_y: u8] + 16 bytes of 2bpp tile data
 *                      (8 rows × 2 bytes/row; same 2bpp encoding as full frame).
 *         The client patches each tile into its local framebuffer copy.
 *         Sent only in TRANSPORT_FAST mode when ≤ TILE_FULL_THRESHOLD tiles changed.
 *
 *   Client → Server:
 *     [0x02, bitmask]   — joypad update (bit0=RIGHT,1=LEFT,2=UP,3=DOWN,
 *                                         bit4=A,5=B,6=SELECT,7=START)
 *     [0x03]            — save state
 *     [0x04]            — load state  (server sends a keyframe on next frame)
 *     [0x05]            — reset       (server sends a keyframe on next frame)
 *     [0x06]            — fast-mode toggle
 *     [0x07, mode]      — set transport mode: 0=RELIABLE (full frames), 1=FAST (tile-diff)
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

extern void gb_set_button(int bit, bool pressed);
extern void gb_reset(void);
extern char savestate_rom_path[4096];

/* ---- Embedded HTML frontend (generated from frontend/index.html) ---- */

#include "ws_server_html.h"

/* ---- Tile-diff constants ---- */

#define WS_TILES_X          20           /* 160 / 8 */
#define WS_TILES_Y          18           /* 144 / 8 */
#define WS_TILES_TOTAL      (WS_TILES_X * WS_TILES_Y)   /* 360 */
#define WS_TILE_DATA_BYTES  16           /* 8 rows × 2 bytes/row */
#define WS_ROW_BYTES        40           /* 160 pixels / 4 pixels-per-byte */

/* Fall back to full frame when more tiles than this change in one frame.
 * At 300 tiles: 3 + 300×18 = 5403 bytes (vs 5761 for full frame).       */
#define TILE_FULL_THRESHOLD 300

/* Send a periodic keyframe every N rendered frames to self-heal any
 * transient corruption caused by missed or mis-applied tile updates.
 * 180 frames ≈ 3 seconds at 60 fps.                                      */
#define KEYFRAME_INTERVAL_FRAMES 180

#define MSG_FULL_FRAME  0x01
#define MSG_TILE_BATCH  0x10

/* ---- Wire format sizes ---- */

#define WS_FRAME_2BPP_PAYLOAD (160 * 144 / 4)               /* 5760 bytes */
#define WS_FRAME_BYTES        (1 + WS_FRAME_2BPP_PAYLOAD)   /* 5761 bytes */

/* Tile-batch header (3 bytes) + up to TILE_FULL_THRESHOLD tiles × 18 bytes */
#define WS_TILE_ENTRY_BYTES  (2 + WS_TILE_DATA_BYTES)       /* tx+ty+data = 18 */
#define WS_TILE_BATCH_HDR    3                               /* type+count16 */
#define WS_TILE_BATCH_MAX    (WS_TILE_BATCH_HDR + TILE_FULL_THRESHOLD * WS_TILE_ENTRY_BYTES)

/* Send buffer: large enough for either message type */
#define WS_SEND_BUF_MAX  WS_FRAME_BYTES   /* 5761 > WS_TILE_BATCH_MAX(5403) */

/* ---- Global framebuffer state ---- */

/* Current and previous 2bpp encoded frames for tile diffing */
static uint8_t g_curr_2bpp[WS_FRAME_2BPP_PAYLOAD];
static uint8_t g_prev_2bpp[WS_FRAME_2BPP_PAYLOAD];

/* Tile-batch buffer (payload ready for lws_write, including type+count bytes) */
static uint8_t g_tile_batch[WS_TILE_BATCH_MAX];
static int     g_tile_batch_len;   /* bytes in g_tile_batch; 0 = no changes */
static bool    g_send_full_frame;  /* true = send full frame this tick */

/* Force next notify_frame() to emit a full frame (reset/load-state events). */
static bool    g_force_keyframe = true;  /* true at startup → first frame is full */

/* Counts frames since last full keyframe; resets when a keyframe is sent. */
static int     g_frames_since_keyframe = 0;

/* Frame sequence counter (monotonically increasing, per-connection drop guard) */
static uint32_t g_frame_seq = 1;

/* Send buffer (LWS_PRE headroom + payload) */
static unsigned char g_send_buf[LWS_PRE + WS_SEND_BUF_MAX];

static struct lws_context *g_context = NULL;

/* ---- Bandwidth statistics ---- */

static struct {
    uint32_t full_frames;
    uint32_t tile_batches;
    uint32_t no_change;
    uint64_t tiles_sent;
    uint64_t bytes_out;     /* payload bytes, no LWS_PRE */
    uint32_t frame_count;
    time_t   last_log;
} g_stats;

/* ---- Transport mode ---- */

typedef enum {
    TRANSPORT_RELIABLE = 0,   /* always send full framebuffers (default) */
    TRANSPORT_FAST     = 1,   /* tile-diff with periodic keyframes        */
} transport_mode_t;

/* ---- Per-connection state ---- */

typedef struct {
    bool             is_ws;
    bool             html_sent;
    uint32_t         last_sent_seq;
    bool             needs_keyframe;    /* FAST mode: send full frame next tick */
    transport_mode_t transport;         /* per-connection transport selection   */
} per_session_t;

/* ---- Forward declaration ---- */

static void ws_server_force_keyframe(void);

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
            pss->is_ws          = true;
            pss->last_sent_seq  = 0;
            pss->needs_keyframe = true;    /* always send full frame on connect */
            pss->transport      = TRANSPORT_RELIABLE;
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

        case 0x02: /* Joypad update */
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

        case 0x04: { /* Load state — framebuffer will change, force keyframe */
            char path[4096];
            savestate_default_path(savestate_rom_path, path, sizeof(path));
            if (!load_state(path))
                fprintf(stderr, "[ws] load_state failed: %s\n", path);
            ws_server_force_keyframe();
            break;
        }

        case 0x05: /* Reset — framebuffer will change, force keyframe */
            gb_reset();
            ws_server_force_keyframe();
            break;

        case 0x06: /* Fast mode toggle */
            g_fast_mode = !g_fast_mode;
            break;

        case 0x07: /* Transport mode selection */
            if (len >= 2 && pss) {
                transport_mode_t prev = pss->transport;
                pss->transport = (data[1] == TRANSPORT_FAST)
                                 ? TRANSPORT_FAST : TRANSPORT_RELIABLE;
                /* Switching to fast: send a keyframe first to establish baseline */
                if (pss->transport == TRANSPORT_FAST && prev != TRANSPORT_FAST)
                    pss->needs_keyframe = true;
                fprintf(stderr, "[ws] transport mode → %s\n",
                        pss->transport == TRANSPORT_FAST ? "FAST" : "RELIABLE");
            }
            break;

        default:
            break;
        }
        return 0;
    }

    /* ---- WebSocket: socket ready to send ---- */
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!pss || !pss->is_ws) return 0;
        if (pss->last_sent_seq == g_frame_seq) return 0;

        if (pss->transport == TRANSPORT_RELIABLE) {
            /* Reliable mode: always send the complete framebuffer. */
            g_send_buf[LWS_PRE] = MSG_FULL_FRAME;
            memcpy(g_send_buf + LWS_PRE + 1, g_curr_2bpp, WS_FRAME_2BPP_PAYLOAD);
            lws_write(wsi, g_send_buf + LWS_PRE,
                      (size_t)WS_FRAME_BYTES, LWS_WRITE_BINARY);
        } else {
            /* Fast mode: use tile-diff; full frame on keyframe events or threshold. */
            bool send_full = g_send_full_frame || pss->needs_keyframe;

            if (send_full) {
                g_send_buf[LWS_PRE] = MSG_FULL_FRAME;
                memcpy(g_send_buf + LWS_PRE + 1, g_curr_2bpp, WS_FRAME_2BPP_PAYLOAD);
                lws_write(wsi, g_send_buf + LWS_PRE,
                          (size_t)WS_FRAME_BYTES, LWS_WRITE_BINARY);
                pss->needs_keyframe = false;
            } else if (g_tile_batch_len > 0) {
                memcpy(g_send_buf + LWS_PRE, g_tile_batch, (size_t)g_tile_batch_len);
                lws_write(wsi, g_send_buf + LWS_PRE,
                          (size_t)g_tile_batch_len, LWS_WRITE_BINARY);
            }
            /* g_tile_batch_len == 0 && !send_full: no changes, nothing to write */
        }

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
        "http",
        callback_gb,
        sizeof(per_session_t),
        4096,
        0, NULL, 0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

/* ---- Public API ---- */

bool ws_server_init(const char *bind_addr, int port)
{
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = port;
    info.iface     = bind_addr;
    info.protocols = protocols;
    info.options   = 0;

    g_context = lws_create_context(&info);
    if (!g_context) {
        fprintf(stderr, "[ws] lws_create_context failed\n");
        return false;
    }

    g_stats.last_log = time(NULL);
    fprintf(stderr, "[ws] listening on port %d\n", port);
    return true;
}

static void ws_server_force_keyframe(void)
{
    g_force_keyframe        = true;
    g_frames_since_keyframe = 0;
}

void ws_server_notify_frame(const uint32_t *pixels, int w, int h)
{
    /* --- Step 1: Encode full frame to g_curr_2bpp ---
     * R channel (byte 0 of uint32) identifies shade; G=B=R for greyscale. */
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
        g_curr_2bpp[i / 4] = byte;
    }

    /* --- Step 2: Determine what to send --- */
    if (g_force_keyframe || g_frames_since_keyframe >= KEYFRAME_INTERVAL_FRAMES) {
        /* Forced or periodic keyframe: send full frame to all clients */
        g_send_full_frame        = true;
        g_tile_batch_len         = 0;
        g_force_keyframe         = false;
        g_frames_since_keyframe  = 0;
        g_stats.full_frames++;
        g_stats.bytes_out += WS_FRAME_BYTES;
    } else {
        /* Tile diff: find changed 8×8 tiles */
        uint16_t changed[WS_TILES_TOTAL];
        int changed_count = 0;

        for (int ty = 0; ty < WS_TILES_Y; ty++) {
            for (int tx = 0; tx < WS_TILES_X; tx++) {
                bool dirty = false;
                for (int r = 0; r < 8 && !dirty; r++) {
                    int off = (ty * 8 + r) * WS_ROW_BYTES + tx * 2;
                    if (g_curr_2bpp[off]   != g_prev_2bpp[off] ||
                        g_curr_2bpp[off+1] != g_prev_2bpp[off+1])
                        dirty = true;
                }
                if (dirty)
                    changed[changed_count++] = (uint16_t)(ty * WS_TILES_X + tx);
            }
        }

        if (changed_count == 0) {
            /* Nothing changed */
            g_send_full_frame = false;
            g_tile_batch_len  = 0;
            g_stats.no_change++;
        } else if (changed_count > TILE_FULL_THRESHOLD) {
            /* Too many tiles — full frame is cheaper; also resets keyframe counter */
            g_send_full_frame        = true;
            g_tile_batch_len         = 0;
            g_frames_since_keyframe  = 0;
            g_stats.full_frames++;
            g_stats.bytes_out += WS_FRAME_BYTES;
        } else {
            /* Build tile-batch payload */
            g_send_full_frame   = false;
            g_tile_batch[0]     = MSG_TILE_BATCH;
            g_tile_batch[1]     = (uint8_t)(changed_count & 0xFF);
            g_tile_batch[2]     = (uint8_t)(changed_count >> 8);
            int pos = WS_TILE_BATCH_HDR;

            for (int i = 0; i < changed_count; i++) {
                int tx = changed[i] % WS_TILES_X;
                int ty = changed[i] / WS_TILES_X;
                g_tile_batch[pos++] = (uint8_t)tx;
                g_tile_batch[pos++] = (uint8_t)ty;
                for (int r = 0; r < 8; r++) {
                    int off = (ty * 8 + r) * WS_ROW_BYTES + tx * 2;
                    g_tile_batch[pos++] = g_curr_2bpp[off];
                    g_tile_batch[pos++] = g_curr_2bpp[off + 1];
                }
            }
            g_tile_batch_len = pos;
            g_stats.tile_batches++;
            g_stats.tiles_sent += (uint64_t)changed_count;
            g_stats.bytes_out  += (uint64_t)pos;
        }
    }

    /* --- Step 3: Roll prev frame --- */
    memcpy(g_prev_2bpp, g_curr_2bpp, WS_FRAME_2BPP_PAYLOAD);
    g_frames_since_keyframe++;
    g_frame_seq++;
    g_stats.frame_count++;

    /* --- Step 4: Periodic bandwidth stats --- */
    time_t now = time(NULL);
    if (now - g_stats.last_log >= 10 && g_stats.frame_count > 0) {
        double avg = (double)g_stats.bytes_out / g_stats.frame_count;
        double reduction = 100.0 * (1.0 - avg / WS_FRAME_BYTES);
        fprintf(stderr,
            "[ws-stats] frames=%u full=%u tile-batches=%u no-change=%u "
            "avg-tiles=%.1f avg-bytes/frame=%.0f vs-full=%.0f savings=%.1f%%\n",
            g_stats.frame_count,
            g_stats.full_frames,
            g_stats.tile_batches,
            g_stats.no_change,
            g_stats.tile_batches > 0
                ? (double)g_stats.tiles_sent / g_stats.tile_batches : 0.0,
            avg,
            (double)WS_FRAME_BYTES,
            reduction);
        memset(&g_stats, 0, sizeof(g_stats));
        g_stats.last_log = now;
    }

    /* --- Step 5: Schedule writeable callbacks --- */
    if (g_context)
        lws_callback_on_writable_all_protocol(g_context, &protocols[0]);
}

void ws_server_service(void)
{
    if (g_context)
        lws_service(g_context, 0);
}

void ws_server_destroy(void)
{
    if (g_context) {
        lws_context_destroy(g_context);
        g_context = NULL;
    }
}
