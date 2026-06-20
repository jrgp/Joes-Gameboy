/*
 * ws_server.c — WebSocket/HTTP server for the browser-based remote frontend.
 *
 * One LWS protocol handles both HTTP (serving the page) and WebSocket
 * (streaming frames + receiving input/commands).
 *
 * Rendering modes:
 *   DMG mode — optimized monochrome transport; client applies palette.
 *   CGB mode — server sends true RGB colors; client displays them directly.
 *
 * Transport modes (per connection, DMG mode only, selected by client via 0x07):
 *   TRANSPORT_RELIABLE — always send full framebuffers (default).
 *   TRANSPORT_FAST     — tile-diff with periodic keyframes.
 *
 * Wire protocol (binary WebSocket messages):
 *   Server → Client:
 *     [0x01] + 5760 bytes      — DMG full frame (2bpp, palette indices 0-3).
 *     [0x10] + count16 + tiles — DMG tile update batch (Fast transport only).
 *     [0x11] + 69120 bytes     — CGB full frame (RGB888, 160×144×3 bytes, R-G-B order).
 *     [0x12] + JSON            — Slot info update.
 *     [0x13, mode]             — Mode announcement (0=DMG, 1=CGB); sent on connect.
 *
 *   Client → Server:
 *     [0x02, bitmask]   — joypad
 *     [0x03]            — save state
 *     [0x04]            — load state
 *     [0x05]            — reset
 *     [0x06]            — fast-mode toggle
 *     [0x07, mode]      — set transport mode: 0=RELIABLE, 1=FAST (DMG only)
 *     [0x08, slot, name_len, ...name] — save to slot N
 *     [0x09, slot]      — load from slot N
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
#include <inttypes.h>

/* ---- Emulator interfaces (defined in gb.c) ---- */

extern void gb_set_button(int bit, bool pressed);
extern void gb_reset(void);
extern char savestate_rom_path[4096];
extern bool gb_is_cgb_mode(void);

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

#define MSG_FULL_FRAME  0x01   /* DMG: 2bpp full frame                      */
#define MSG_TILE_BATCH  0x10   /* DMG: tile-diff update batch                */
#define MSG_CGB_FRAME   0x11   /* CGB: RGB888 full frame (160×144×3 bytes)   */
#define MSG_MODE_INFO   0x13   /* mode announcement: [0x13, mode] (0=DMG,1=CGB) */

/* ---- Wire format sizes ---- */

#define WS_FRAME_2BPP_PAYLOAD (160 * 144 / 4)               /* 5760 bytes */
#define WS_FRAME_BYTES        (1 + WS_FRAME_2BPP_PAYLOAD)   /* 5761 bytes */

/* Tile-batch header (3 bytes) + up to TILE_FULL_THRESHOLD tiles × 18 bytes */
#define WS_TILE_ENTRY_BYTES  (2 + WS_TILE_DATA_BYTES)       /* tx+ty+data = 18 */
#define WS_TILE_BATCH_HDR    3                               /* type+count16 */
#define WS_TILE_BATCH_MAX    (WS_TILE_BATCH_HDR + TILE_FULL_THRESHOLD * WS_TILE_ENTRY_BYTES)

/* ---- Wire size constants ---- */
/* DMG send buffer covers both full-frame and tile-batch */
#define WS_SEND_BUF_MAX  WS_FRAME_BYTES   /* 5761 > WS_TILE_BATCH_MAX(5403) */

/* CGB full frame: type byte + 160×144×3 bytes of RGB888 */
#define WS_CGB_PIXELS    (160 * 144 * 3)       /* 69120 bytes */
#define WS_CGB_FRAME_BYTES (1 + WS_CGB_PIXELS) /* 69121 bytes */

/* ---- Global framebuffer state ---- */

/* DMG: current and previous 2bpp encoded frames for tile diffing */
static uint8_t g_curr_2bpp[WS_FRAME_2BPP_PAYLOAD];
static uint8_t g_prev_2bpp[WS_FRAME_2BPP_PAYLOAD];

/* CGB: current RGB888 frame (R,G,B triples, row-major) */
static uint8_t g_curr_rgb[WS_CGB_PIXELS];

/* Current rendering mode (updated in ws_server_notify_frame) */
static bool g_is_cgb = false;

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

/* ---- Slot info state ---- */

/* JSON slot info buffer, pre-built by ws_build_slot_info().
 * Layout: [LWS_PRE bytes padding][0x12][JSON bytes]                       */
#define SLOT_JSON_BUF 2048
static unsigned char g_slot_info_buf[LWS_PRE + SLOT_JSON_BUF];
static int           g_slot_info_len;       /* payload bytes (type + JSON), 0 = not built */
static uint32_t      g_slot_info_version = 1; /* incremented when slots change */

static void ws_build_slot_info(void);
static void ws_notify_all_slot_info(void);

/* Send buffers (LWS_PRE headroom + payload) */
static unsigned char g_send_buf[LWS_PRE + WS_SEND_BUF_MAX];         /* DMG frames  */
static unsigned char g_cgb_send_buf[LWS_PRE + WS_CGB_FRAME_BYTES];  /* CGB frames  */
static unsigned char g_mode_buf[LWS_PRE + 2];                        /* mode announ */

static struct lws_context *g_context = NULL;
static const struct lws_protocols *g_main_protocol = NULL;  /* set in ws_server_init */

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
    bool             needs_keyframe;       /* FAST mode: send full frame next tick   */
    bool             needs_mode_announce;  /* true until 0x13 mode msg is sent       */
    transport_mode_t transport;            /* per-connection transport selection     */
    uint32_t         last_slot_version;    /* 0 on new conn → triggers slot info send */
} per_session_t;

/* ---- Forward declarations ---- */

static void ws_server_force_keyframe(void);
static void ws_build_slot_info(void);
static void ws_notify_all_slot_info(void);

/* ---- Slot info helpers ---- */

static void ws_build_slot_info(void) {
    /* Build a JSON array describing all SS_NUM_SLOTS save slots and store it
     * into g_slot_info_buf[LWS_PRE..] as: [0x12][JSON bytes].             */
    char json[SLOT_JSON_BUF - 4];
    int pos = 0;
    json[pos++] = '[';

    for (int s = 1; s <= SS_NUM_SLOTS; s++) {
        char sp[4096], lp[4096];
        savestate_slot_path(savestate_rom_path, s, sp, sizeof(sp));

        bool exists = (savestate_rom_path[0] != '\0') && (access(sp, F_OK) == 0);
        const char *rpath = sp;

        /* Migration: if slot 1 not found but legacy game.cbor exists, expose it */
        if (!exists && s == 1 && savestate_rom_path[0] != '\0') {
            savestate_default_path(savestate_rom_path, lp, sizeof(lp));
            if (access(lp, F_OK) == 0) { exists = true; rpath = lp; }
        }

        char name[128] = "";
        int64_t ts = 0;
        if (exists) slot_read_meta(rpath, name, sizeof(name), &ts);

        /* JSON-escape the name string */
        char esc[256]; int ei = 0;
        for (int i = 0; name[i] && ei < 250; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c == '"' || c == '\\') esc[ei++] = '\\';
            else if (c < 0x20) { esc[ei++] = '\\'; esc[ei++] = 'u'; esc[ei++] = '0'; esc[ei++] = '0';
                                  esc[ei++] = "0123456789abcdef"[c >> 4];
                                  esc[ei++] = "0123456789abcdef"[c & 0xf]; continue; }
            esc[ei++] = (char)c;
        }
        esc[ei] = '\0';

        int n = snprintf(json + pos, sizeof(json) - (size_t)pos - 4,
            "%s{\"slot\":%d,\"exists\":%s,\"name\":\"%s\",\"ts\":%" PRId64 "}",
            pos > 1 ? "," : "", s, exists ? "true" : "false", esc, ts);
        if (n > 0 && pos + n < (int)sizeof(json) - 4) pos += n;
    }

    if (pos < (int)sizeof(json) - 2) { json[pos++] = ']'; json[pos] = '\0'; }

    g_slot_info_buf[LWS_PRE] = 0x12;
    size_t jlen = (size_t)pos;
    if (jlen > SLOT_JSON_BUF - 2) jlen = SLOT_JSON_BUF - 2;
    memcpy(g_slot_info_buf + LWS_PRE + 1, json, jlen);
    g_slot_info_len = (int)(jlen + 1);
}

static void ws_notify_all_slot_info(void) {
    ws_build_slot_info();
    g_slot_info_version++;
    if (g_context && g_main_protocol)
        lws_callback_on_writable_all_protocol(g_context, g_main_protocol);
}

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
            pss->is_ws               = true;
            pss->last_sent_seq       = 0;
            pss->needs_keyframe      = true;    /* always send full frame on connect */
            pss->needs_mode_announce = true;    /* announce DMG/CGB mode first       */
            pss->transport           = TRANSPORT_RELIABLE;
            pss->last_slot_version   = 0;       /* ensures slot info is sent on first writeable */
            ws_build_slot_info();               /* refresh from disk for this connection */
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

        case 0x08: { /* Save to slot N with optional name */
            if (len < 2) break;
            int slot = (int)data[1];
            if (slot < 1 || slot > SS_NUM_SLOTS) break;

            char name[128] = "";
            if (len >= 3) {
                size_t nlen = data[2];
                if (nlen > 64) nlen = 64;
                if (len >= 3 + nlen) { memcpy(name, data + 3, nlen); name[nlen] = '\0'; }
            }

            char sp[4096];
            savestate_slot_path(savestate_rom_path, slot, sp, sizeof(sp));
            if (!save_state_slot(sp, name[0] ? name : NULL))
                fprintf(stderr, "[ws] save slot %d failed: %s\n", slot, sp);
            ws_notify_all_slot_info();
            break;
        }

        case 0x09: { /* Load from slot N */
            if (len < 2) break;
            int slot = (int)data[1];
            if (slot < 1 || slot > SS_NUM_SLOTS) break;

            char sp[4096], lp[4096];
            savestate_slot_path(savestate_rom_path, slot, sp, sizeof(sp));
            /* Migration: slot 1 falls back to legacy game.cbor */
            const char *rpath = sp;
            if (slot == 1 && access(sp, F_OK) != 0) {
                savestate_default_path(savestate_rom_path, lp, sizeof(lp));
                if (access(lp, F_OK) == 0) rpath = lp;
            }
            if (!load_state(rpath))
                fprintf(stderr, "[ws] load slot %d failed: %s\n", slot, rpath);
            ws_server_force_keyframe();
            ws_notify_all_slot_info();
            break;
        }

        default:
            break;
        }
        return 0;
    }

    /* ---- WebSocket: socket ready to send ---- */
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (!pss || !pss->is_ws) return 0;

        /* Priority 1: mode announcement (once per connection, before anything else). */
        if (pss->needs_mode_announce) {
            pss->needs_mode_announce = false;
            g_mode_buf[LWS_PRE]     = MSG_MODE_INFO;
            g_mode_buf[LWS_PRE + 1] = g_is_cgb ? 1 : 0;
            lws_write(wsi, g_mode_buf + LWS_PRE, 2, LWS_WRITE_BINARY);
            lws_callback_on_writable(wsi);
            return 0;
        }

        /* Priority 2: slot info if client is out of date. */
        if (pss->last_slot_version != g_slot_info_version && g_slot_info_len > 0) {
            lws_write(wsi, g_slot_info_buf + LWS_PRE,
                      (size_t)g_slot_info_len, LWS_WRITE_BINARY);
            pss->last_slot_version = g_slot_info_version;
            if (pss->last_sent_seq != g_frame_seq)
                lws_callback_on_writable(wsi);  /* come back for frame data */
            return 0;
        }

        if (pss->last_sent_seq == g_frame_seq) return 0;

        if (g_is_cgb) {
            /* CGB mode: always send full RGB888 frame. */
            lws_write(wsi, g_cgb_send_buf + LWS_PRE,
                      (size_t)WS_CGB_FRAME_BYTES, LWS_WRITE_BINARY);
        } else if (pss->transport == TRANSPORT_RELIABLE) {
            /* DMG reliable mode: always send the complete framebuffer. */
            g_send_buf[LWS_PRE] = MSG_FULL_FRAME;
            memcpy(g_send_buf + LWS_PRE + 1, g_curr_2bpp, WS_FRAME_2BPP_PAYLOAD);
            lws_write(wsi, g_send_buf + LWS_PRE,
                      (size_t)WS_FRAME_BYTES, LWS_WRITE_BINARY);
        } else {
            /* DMG fast mode: use tile-diff; full frame on keyframe events or threshold. */
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
    g_main_protocol = &protocols[0];

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
    /* Update the rendering mode flag each frame. */
    g_is_cgb = gb_is_cgb_mode();

    if (g_is_cgb) {
        /* --- CGB mode: encode pixels as RGB888 --- */
        int n = w * h;
        for (int i = 0; i < n; i++) {
            uint32_t px = pixels[i];
            g_curr_rgb[i * 3]     = (uint8_t)(px & 0xFFu);        /* R */
            g_curr_rgb[i * 3 + 1] = (uint8_t)((px >> 8) & 0xFFu); /* G */
            g_curr_rgb[i * 3 + 2] = (uint8_t)((px >> 16) & 0xFFu);/* B */
        }
        /* Build the CGB send buffer once (shared across all connections). */
        g_cgb_send_buf[LWS_PRE] = MSG_CGB_FRAME;
        memcpy(g_cgb_send_buf + LWS_PRE + 1, g_curr_rgb, WS_CGB_PIXELS);

        g_stats.full_frames++;
        g_stats.bytes_out += (uint64_t)WS_CGB_FRAME_BYTES;
    } else {
        /* --- DMG mode: encode pixels as 2bpp --- */
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

        /* Determine what to send (same tile-diff logic as before). */
        if (g_force_keyframe || g_frames_since_keyframe >= KEYFRAME_INTERVAL_FRAMES) {
            g_send_full_frame        = true;
            g_tile_batch_len         = 0;
            g_force_keyframe         = false;
            g_frames_since_keyframe  = 0;
            g_stats.full_frames++;
            g_stats.bytes_out += WS_FRAME_BYTES;
        } else {
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
                g_send_full_frame = false;
                g_tile_batch_len  = 0;
                g_stats.no_change++;
            } else if (changed_count > TILE_FULL_THRESHOLD) {
                g_send_full_frame        = true;
                g_tile_batch_len         = 0;
                g_frames_since_keyframe  = 0;
                g_stats.full_frames++;
                g_stats.bytes_out += WS_FRAME_BYTES;
            } else {
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

        memcpy(g_prev_2bpp, g_curr_2bpp, WS_FRAME_2BPP_PAYLOAD);
        g_frames_since_keyframe++;
    }

    g_frame_seq++;
    g_stats.frame_count++;

    /* Periodic bandwidth stats */
    time_t now = time(NULL);
    if (now - g_stats.last_log >= 10 && g_stats.frame_count > 0) {
        double avg = (double)g_stats.bytes_out / g_stats.frame_count;
        double baseline = g_is_cgb ? (double)WS_CGB_FRAME_BYTES : (double)WS_FRAME_BYTES;
        double reduction = 100.0 * (1.0 - avg / baseline);
        fprintf(stderr,
            "[ws-stats] mode=%s frames=%u full=%u tile-batches=%u no-change=%u "
            "avg-bytes/frame=%.0f savings=%.1f%%\n",
            g_is_cgb ? "CGB" : "DMG",
            g_stats.frame_count,
            g_stats.full_frames,
            g_stats.tile_batches,
            g_stats.no_change,
            avg,
            reduction);
        memset(&g_stats, 0, sizeof(g_stats));
        g_stats.last_log = now;
    }

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
