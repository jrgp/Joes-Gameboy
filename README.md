# cgb

A Game Boy DMG emulator written in C. Passes 31/31 Blargg tests and 75/75 Mooneye tests.

## Frontends

| Frontend | Description |
|---|---|
| **SDL** | Native desktop window (default) |
| **Server** | Headless + WebSocket; stream gameplay to a browser |
| **WASM** | Runs entirely in the browser via WebAssembly |

All three frontends share the same emulator core (`gb.c`) and save-state format.

## Dependencies

### Linux (apt)
```bash
sudo apt-get install gcc libsdl2-dev libcbor-dev libwebsockets-dev
# WASM only:
sudo apt-get install emscripten
```

### macOS (brew)
```bash
brew install sdl2 libcbor libwebsockets
# WASM only:
brew install emscripten
```

## Building

### SDL (desktop)
```bash
make
./gb game.gb
```

Controls: Arrow keys = D-Pad, `A` = A, `S` = B, `Enter` = Start, `Shift` = Select,
`F5` = save state, `F` = toggle fast mode (~4× speed).

### Server (browser remote)
```bash
make
./gb --server game.gb           # listens on :8080
./gb --server --port 9000 --bind 127.0.0.1 game.gb
```

Open `http://localhost:8080` in a browser. The page streams the framebuffer over
WebSocket. Save states are stored server-side adjacent to the ROM.

### WASM (local browser)
```bash
make wasm-deps   # download and compile libcbor with emcc (one-time)
make wasm        # produces dist/gb.js + dist/gb.wasm

cp frontend/index.html dist/
cd dist && python3 -m http.server 8000
```

Open `http://localhost:8000`, click **Load ROM**, pick a `.gb` file.
Save/load state downloads and uploads `.cbor` files locally.

## Headless / testing
```bash
# Blargg serial output:
./gb --headless --cycles 100000000 rom.gb

# Cycle-limited with pass/fail register:
./gb --headless --gbmicrotest --cycles 500000 rom.gb
```

## Save states

Save states use CBOR and are compatible across all three frontends.

| Frontend | Save | Load |
|---|---|---|
| SDL | `F5` | auto-loaded on next launch |
| Server | toolbar button / `F5` key in browser | toolbar button |
| WASM | downloads `.cbor` file | uploads `.cbor` file |

Battery-backed cartridge RAM (`.sav`) is loaded and saved automatically.

## Options

```
./gb [options] <rom.gb | save.cbor>

  --headless          Run without display
  --server            Start WebSocket server (implies --headless)
  --port <n>          WebSocket server port (default: 8080)
  --bind <addr>       Bind address (default: all interfaces)
  --cycles <n>        Stop after n cycles
  --model <name>      Hardware model: dmg (default), dmg0, mgb, sgb, sgb2, cgb
  --gbmicrotest       Read 0xFF82 pass/fail after cycle limit
  --ppm <file>        Dump current frame to PPM file
```
