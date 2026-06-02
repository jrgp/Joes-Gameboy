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

/* ---- Embedded HTML frontend ---- */

static const char HTML_PAGE[] =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>\n"
"<title>Game Boy</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0;-webkit-touch-callout:none;touch-action:manipulation}\n"
"html{height:100%}\n"
"body{height:100%;display:flex;flex-direction:column;align-items:stretch;background:#0d0d1a;color:#ccc;font-family:monospace;overflow:hidden;user-select:none}\n"
"#bar{flex:0 0 auto;display:flex;gap:6px;padding:6px;background:#111120;justify-content:center;align-items:center;width:100%}\n"
".tb{background:#1e1e38;color:#999;border:1px solid #3a3a5a;border-radius:4px;padding:5px 10px;cursor:pointer;font:12px/1 monospace;touch-action:manipulation;-webkit-user-select:none;user-select:none}\n"
".tb:active{background:#3a3a6a}\n"
"#st{font-size:11px;color:#555;margin-left:4px}\n"
"#sw{flex:1 1 0;min-height:0;display:flex;align-items:center;justify-content:center;overflow:hidden}\n"
"canvas{display:block;image-rendering:pixelated;image-rendering:crisp-edges;background:#111}\n"
"#ctrl-canvas{flex:0 0 auto;display:block;width:100%;height:130px;background:#0a0a18;touch-action:none}\n"
"#hint{flex:0 0 auto;font-size:11px;color:#555;text-align:center;padding:4px;width:100%}\n"
"@media(min-width:540px){#ctrl-canvas{display:none}#hint{display:block}}\n"
"@media(max-width:539px){#hint{display:none}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div id='bar'>\n"
"  <button class='tb' id='bsv'>Save</button>\n"
"  <button class='tb' id='bld'>Load</button>\n"
"  <button class='tb' id='brs'>Reset</button>\n"
"  <button class='tb' id='bft'>Fast</button>\n"
"  <span id='st'>Connecting...</span>\n"
"</div>\n"
"<div id='sw'><canvas id='screen' width='160' height='144'></canvas></div>\n"
"<canvas id='ctrl-canvas'></canvas>\n"
"<div id='hint'>Arrows: D-Pad &nbsp; A: A &nbsp; S: B &nbsp; Enter: Start &nbsp; Shift: Select &nbsp; F5: Save &nbsp; F8: Load &nbsp; F: Fast</div>\n"
"<script>\n"
"'use strict';\n"
"\n"
"class RemoteBackend {\n"
"  constructor(url) {\n"
"    this.onframe = null;\n"
"    this.onstatus = null;\n"
"    this._url = url;\n"
"    this._ws = null;\n"
"    this._g_px = new Uint8ClampedArray(160 * 144 * 4);\n"
"    this._PALETTE = new Uint8Array([255,255,255,170,170,170,85,85,85,0,0,0]);\n"
"    this._connect();\n"
"  }\n"
"  _connect() {\n"
"    this._ws = new WebSocket(this._url);\n"
"    this._ws.binaryType = 'arraybuffer';\n"
"    this._ws.onopen = () => { if (this.onstatus) this.onstatus('Connected'); };\n"
"    this._ws.onclose = () => {\n"
"      if (this.onstatus) this.onstatus('Disconnected');\n"
"      setTimeout(() => this._connect(), 2000);\n"
"    };\n"
"    this._ws.onerror = () => {};\n"
"    this._ws.onmessage = (e) => {\n"
"      const d = new Uint8Array(e.data);\n"
"      if (d[0] === 1 && d.length === 5761) {\n"
"        let bi = 0;\n"
"        for (let i = 1; i < 5761; i++) {\n"
"          const b = d[i];\n"
"          let x;\n"
"          x=((b>>6)&3)*3; this._g_px[bi++]=this._PALETTE[x];this._g_px[bi++]=this._PALETTE[x+1];this._g_px[bi++]=this._PALETTE[x+2];this._g_px[bi++]=255;\n"
"          x=((b>>4)&3)*3; this._g_px[bi++]=this._PALETTE[x];this._g_px[bi++]=this._PALETTE[x+1];this._g_px[bi++]=this._PALETTE[x+2];this._g_px[bi++]=255;\n"
"          x=((b>>2)&3)*3; this._g_px[bi++]=this._PALETTE[x];this._g_px[bi++]=this._PALETTE[x+1];this._g_px[bi++]=this._PALETTE[x+2];this._g_px[bi++]=255;\n"
"          x=(b&3)*3;      this._g_px[bi++]=this._PALETTE[x];this._g_px[bi++]=this._PALETTE[x+1];this._g_px[bi++]=this._PALETTE[x+2];this._g_px[bi++]=255;\n"
"        }\n"
"        if (this.onframe) this.onframe(this._g_px);\n"
"      }\n"
"    };\n"
"  }\n"
"  _snd(cmd, extra) {\n"
"    if (!this._ws || this._ws.readyState !== 1) return;\n"
"    const m = new Uint8Array(extra !== undefined ? 2 : 1);\n"
"    m[0] = cmd;\n"
"    if (extra !== undefined) m[1] = extra & 255;\n"
"    this._ws.send(m.buffer);\n"
"  }\n"
"  sendJoypad(mask) { this._snd(2, mask); }\n"
"  sendSave()       { this._snd(3); }\n"
"  sendLoad()       { this._snd(4); }\n"
"  sendReset()      { this._snd(5); }\n"
"  setFast(_on)     { this._snd(6); }\n"
"}\n"
"\n"
"(function() {\n"
"  const cv = document.getElementById('screen');\n"
"  const ctx = cv.getContext('2d');\n"
"  const st = document.getElementById('st');\n"
"  const cc = document.getElementById('ctrl-canvas');\n"
"  const cc2 = cc.getContext('2d');\n"
"  const frameImage = ctx.createImageData(160, 144);\n"
"  let backend = null;\n"
"  let btns = 0;\n"
"  let fast = false;\n"
"  let cBtns = 0;\n"
"  const cTouches = {};\n"
"  const CL = {};\n"
"  const km = {ArrowRight:0,ArrowLeft:1,ArrowUp:2,ArrowDown:3,a:4,A:4,s:5,S:5,Enter:7,Shift:6};\n"
"\n"
"  function updateStatus(text) {\n"
"    st.textContent = text;\n"
"  }\n"
"\n"
"  function renderFrame(pixels) {\n"
"    frameImage.data.set(pixels);\n"
"    ctx.putImageData(frameImage, 0, 0);\n"
"  }\n"
"\n"
"  function setFastUi(on) {\n"
"    fast = !!on;\n"
"    const b = document.getElementById('bft');\n"
"    b.textContent = fast ? 'Fast ⏩' : 'Fast';\n"
"    b.style.background = fast ? '#c84' : '';\n"
"  }\n"
"\n"
"  function ctrlLayout() {\n"
"    const w = cc.offsetWidth;\n"
"    if (!w) return;\n"
"    cc.width = w;\n"
"    cc.height = cc.offsetHeight || 130;\n"
"    const h = cc.height;\n"
"    CL.dpr = Math.min(w * 0.14, h * 0.44);\n"
"    CL.dpx = w * 0.19;\n"
"    CL.dpy = h * 0.5;\n"
"    CL.ar = Math.min(w * 0.075, h * 0.26);\n"
"    CL.ax = w * 0.85;\n"
"    CL.ay = h * 0.65;\n"
"    CL.bx = w * 0.74;\n"
"    CL.by = h * 0.3;\n"
"    CL.sw = Math.max(40, w * 0.11);\n"
"    CL.sh = Math.max(18, h * 0.22);\n"
"    CL.slx = w * 0.43;\n"
"    CL.sly = h * 0.5;\n"
"    CL.stx = w * 0.56;\n"
"    CL.sty = h * 0.5;\n"
"  }\n"
"\n"
"  function bitsAt(x, y) {\n"
"    let b = 0;\n"
"    const dx = x - CL.dpx;\n"
"    const dy = y - CL.dpy;\n"
"    const d = Math.sqrt(dx * dx + dy * dy);\n"
"    if (d > 8 && d < CL.dpr + 14) {\n"
"      const ax = Math.abs(dx);\n"
"      const ay = Math.abs(dy);\n"
"      if (dy < 0 && ay > ax * 0.5) b |= 4;\n"
"      if (dy > 0 && ay > ax * 0.5) b |= 8;\n"
"      if (dx < 0 && ax > ay * 0.5) b |= 2;\n"
"      if (dx > 0 && ax > ay * 0.5) b |= 1;\n"
"    }\n"
"    const r2 = CL.ar * CL.ar * 2;\n"
"    const adx = x - CL.ax;\n"
"    const ady = y - CL.ay;\n"
"    if (adx * adx + ady * ady < r2) b |= 16;\n"
"    const bdx = x - CL.bx;\n"
"    const bdy = y - CL.by;\n"
"    if (bdx * bdx + bdy * bdy < r2) b |= 32;\n"
"    if (Math.abs(x - CL.slx) < CL.sw / 2 && Math.abs(y - CL.sly) < CL.sh / 2) b |= 64;\n"
"    if (Math.abs(x - CL.stx) < CL.sw / 2 && Math.abs(y - CL.sty) < CL.sh / 2) b |= 128;\n"
"    return b;\n"
"  }\n"
"\n"
"  function ctrlPt(t) {\n"
"    const r = cc.getBoundingClientRect();\n"
"    return {x:(t.clientX-r.left)*cc.width/r.width,y:(t.clientY-r.top)*cc.height/r.height};\n"
"  }\n"
"\n"
"  function ctrlUpdate() {\n"
"    let b = 0;\n"
"    for (const id in cTouches) b |= cTouches[id];\n"
"    if (b !== cBtns) {\n"
"      cBtns = b;\n"
"      if (backend) backend.sendJoypad(b);\n"
"    }\n"
"    drawCtrl();\n"
"  }\n"
"\n"
"  function drawCtrl() {\n"
"    if (!cc2 || !cc.width || !cc.height) return;\n"
"    const w = cc.width;\n"
"    const h = cc.height;\n"
"    const c = cc2;\n"
"    c.clearRect(0, 0, w, h);\n"
"    c.textAlign = 'center';\n"
"    c.textBaseline = 'middle';\n"
"    [[2,0,-1,'▲'],[3,0,1,'▼'],[1,-1,0,'◄'],[0,1,0,'►']].forEach((d) => {\n"
"      const on = cBtns & (1 << d[0]);\n"
"      const ax = CL.dpx + d[1] * CL.dpr * 0.55;\n"
"      const ay = CL.dpy + d[2] * CL.dpr * 0.55;\n"
"      const aw = d[1] ? CL.dpr * 0.58 : CL.dpr * 0.48;\n"
"      const ah = d[2] ? CL.dpr * 0.58 : CL.dpr * 0.48;\n"
"      c.fillStyle = on ? '#3a3a70' : '#1a1a30';\n"
"      c.beginPath();\n"
"      c.roundRect(ax - aw / 2, ay - ah / 2, aw, ah, 4);\n"
"      c.fill();\n"
"      c.fillStyle = on ? '#ddd' : '#555';\n"
"      c.font = Math.round(CL.dpr * 0.32) + 'px monospace';\n"
"      c.fillText(d[3], ax, ay);\n"
"    });\n"
"    const cs = CL.dpr * 0.34;\n"
"    c.fillStyle = '#0a0a18';\n"
"    c.fillRect(CL.dpx - cs, CL.dpy - cs, cs * 2, cs * 2);\n"
"    [[CL.ax,CL.ay,4,'A','#9b2020'],[CL.bx,CL.by,5,'B','#6b2090']].forEach((b) => {\n"
"      const on = cBtns & (1 << b[2]);\n"
"      c.beginPath();\n"
"      c.arc(b[0], b[1], CL.ar, 0, Math.PI * 2);\n"
"      c.fillStyle = on ? '#5050bb' : b[4];\n"
"      c.fill();\n"
"      c.fillStyle = '#fff';\n"
"      c.font = 'bold ' + Math.round(CL.ar * 0.7) + 'px monospace';\n"
"      c.fillText(b[3], b[0], b[1]);\n"
"    });\n"
"    [[CL.slx,CL.sly,6,'SEL'],[CL.stx,CL.sty,7,'START']].forEach((b) => {\n"
"      const on = cBtns & (1 << b[2]);\n"
"      c.beginPath();\n"
"      c.roundRect(b[0] - CL.sw / 2, b[1] - CL.sh / 2, CL.sw, CL.sh, 5);\n"
"      c.fillStyle = on ? '#3a3a70' : '#1a1a30';\n"
"      c.fill();\n"
"      c.fillStyle = on ? '#bbb' : '#555';\n"
"      c.font = '9px monospace';\n"
"      c.fillText(b[3], b[0], b[1]);\n"
"    });\n"
"  }\n"
"\n"
"  function resize() {\n"
"    const sw = document.getElementById('sw');\n"
"    const aw = sw.clientWidth || window.innerWidth;\n"
"    const ah = sw.clientHeight || Math.floor(window.innerHeight * 0.6);\n"
"    let s = Math.min(aw / 160, ah / 144);\n"
"    if (s < 1) s = 1;\n"
"    cv.style.width = Math.floor(160 * s) + 'px';\n"
"    cv.style.height = Math.floor(144 * s) + 'px';\n"
"    ctrlLayout();\n"
"    drawCtrl();\n"
"  }\n"
"\n"
"  function setButtonState(bit, on) {\n"
"    const prev = btns;\n"
"    btns = on ? (btns | (1 << bit)) : (btns & ~(1 << bit));\n"
"    if (btns !== prev && backend) backend.sendJoypad(btns);\n"
"  }\n"
"\n"
"  function setupBackend() {\n"
"    const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';\n"
"    backend = new RemoteBackend(proto + location.host + '/ws');\n"
"    backend.onframe = renderFrame;\n"
"    backend.onstatus = updateStatus;\n"
"    resize();\n"
"  }\n"
"\n"
"  window.addEventListener('resize', resize);\n"
"  window.addEventListener('load', resize);\n"
"\n"
"  document.addEventListener('keydown', (e) => {\n"
"    if (e.key === 'F5') {\n"
"      if (backend) backend.sendSave();\n"
"      e.preventDefault();\n"
"      return;\n"
"    }\n"
"    if (e.key === 'F8') {\n"
"      if (backend) backend.sendLoad();\n"
"      e.preventDefault();\n"
"      return;\n"
"    }\n"
"    if (e.key === 'f' || e.key === 'F') {\n"
"      if (backend) backend.setFast(!fast);\n"
"      setFastUi(!fast);\n"
"      e.preventDefault();\n"
"      return;\n"
"    }\n"
"    const b = km[e.key];\n"
"    if (b !== undefined) {\n"
"      setButtonState(b, true);\n"
"      e.preventDefault();\n"
"    }\n"
"  });\n"
"\n"
"  document.addEventListener('keyup', (e) => {\n"
"    const b = km[e.key];\n"
"    if (b !== undefined) {\n"
"      setButtonState(b, false);\n"
"      e.preventDefault();\n"
"    }\n"
"  });\n"
"\n"
"  document.getElementById('bsv').onclick = () => { if (backend) backend.sendSave(); };\n"
"  document.getElementById('bld').onclick = () => { if (backend) backend.sendLoad(); };\n"
"  document.getElementById('brs').onclick = () => { if (backend) backend.sendReset(); };\n"
"  document.getElementById('bft').onclick = () => {\n"
"    if (backend) backend.setFast(!fast);\n"
"    setFastUi(!fast);\n"
"  };\n"
"  ['touchstart','touchmove'].forEach((ev) => {\n"
"    cc.addEventListener(ev, (e) => {\n"
"      e.preventDefault();\n"
"      for (let i = 0; i < e.changedTouches.length; i++) {\n"
"        const t = e.changedTouches[i];\n"
"        const p = ctrlPt(t);\n"
"        cTouches[t.identifier] = bitsAt(p.x, p.y);\n"
"      }\n"
"      ctrlUpdate();\n"
"    }, {passive:false});\n"
"  });\n"
"\n"
"  ['touchend','touchcancel'].forEach((ev) => {\n"
"    cc.addEventListener(ev, (e) => {\n"
"      e.preventDefault();\n"
"      for (let i = 0; i < e.changedTouches.length; i++) delete cTouches[e.changedTouches[i].identifier];\n"
"      ctrlUpdate();\n"
"    }, {passive:false});\n"
"  });\n"
"\n"
"  setFastUi(false);\n"
"  resize();\n"
"  setupBackend();\n"
"})();\n"
"</script>\n"
"</body>\n"
"</html>\n"
;

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
