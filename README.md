# cgb

A Game Boy DMG emulator written in C

I started this project in 2021 and got as far as getting TETRIS playable, then I left the project for a while. Starting in 2026, I've been guiding Claude Sonnet 4.6 to gradually complete the emulator and add many more features.

### Features

- Multiple UIs - SDL/WASM/headless
- A headless server mode that allows `screen -x` or `tmux attach` functionality, for gaming on the Go.
- Game saves/snapshots
- Toggleable Color Palettes
- Toggleable Fast Mode (4x) to make grinding Pokemon easier
- Correctness: Blargg/Mooneye e2e tests pass.

### Upcoming features

- Gameboy Color
- Audio
- Native mac app
- Auto builds/releases using GHA

### Screenshots



Pokemon red with red palette on iphone/websockets

<img src="screenshots/iphone.png" width="400">


Pokemon red with blue palette on safari/websockets (same game streamed)

<img src="screenshots/pokemon in browser.png" width="400">

Donkey Kong Land with gold palette in SDL

<img src="screenshots/dkland in SDL.png" width="400">

Pokemon red with stock palette in SDL

<img src="screenshots/pokemon red.png" width="400">


## Frontends

| Frontend | Description |
|---|---|
| **SDL** | Native desktop window (default) |
| **Server** | Headless + WebSocket; stream gameplay to a browser |
| **WASM** | Runs entirely in the browser via WebAssembly |

All three frontends share the same emulator core (`gb.c`) and save-state format.

## Dependencies

### Linux (apt — Debian/Ubuntu)
```bash
sudo apt-get install gcc libsdl2-dev libcbor-dev libwebsockets-dev
# WASM only:
sudo apt-get install emscripten
```

### Linux (dnf — RHEL / AlmaLinux 9)
```bash
sudo dnf install epel-release
sudo dnf install gcc SDL2-devel libcbor-devel libwebsockets-devel
# WASM only (emscripten is not in EPEL — install via emsdk):
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

> **AlmaLinux 8 / RHEL 8:** SDL2-devel is in the PowerTools repo.
> Enable it first with `sudo dnf config-manager --set-enabled powertools`.

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

Controls:

- Arrow keys = D-Pad
- `A` = A
- `S` = B
- `Enter` = Start
- `Shift` = Select
- `F5` = Save state
- `F6` / `shift + F6` = Change color palette
- `F` = Toggle fast mode (~4× speed).

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

Open `http://localhost:8000/?wasm=1`, click **Load ROM**, pick a `.gb` file.
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
