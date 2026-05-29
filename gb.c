#include <SDL.h>

#include<unistd.h>
#include<inttypes.h>
#include<stdio.h>
#include<stdlib.h>
#include<stdbool.h>
#include<string.h>
#include"bios.h"
#include"bits.h"
#include"constants.h"
#include"opnames.h"

typedef uint8_t byte;
typedef uint16_t word;

//
// Forward declare some prototypes
//
void request_interrupt(byte interrupt);
byte mem_read(int pos);
void mem_write(int pos, byte data);
FILE *debug_logfile;
extern byte RAM[0xffff + 1];

//
// GPU
//
int gpu_cycles;
int exec_next_start_cycles; // cycles count at start of exec_next (after do_interrupts)
int cycles; // CPU cycle counter (forward-declared here for gpu_write access)
byte VRAM[0x2000];  // VRAM is 8KB, not 64KB
byte LY_REG = 0;    // LY register (0xFF44)
byte SCX_REG = 0;   // SCX register (0xFF43)
byte SCY_REG = 0;   // SCY register (0xFF42)
byte LCDC_REG = 0;  // LCDC register (0xFF40)
byte BGP_REG = 0;   // BGP register (0xFF47)

uint32_t *pixels;
char serial_buf[256];
int serial_buf_len = 0;
byte serial_sb = 0;
bool headless = false;
long long max_cycles = 0;

// Blargg A000-based result detection (used by halt_bug, interrupt_time, mem_timing-2, oam_bug)
// Magic bytes at A001-A003 = {0xDE, 0xB0, 0x61}; result at A000 (0x80=running, 0=pass, other=fail)
bool blargg_done = false;

typedef struct {
    bool enabled, bg, window, sprite;
    int windowtilemap, BgWindowTileData;
    bool BgTileDataSigned;
    int bgtilemap;
} GPUCONTROL;

GPUCONTROL gpu_control;

void gpu_parse_control(byte control){
    gpu_control.enabled = bit_check(control, 7);
    gpu_control.window = bit_check(control, 5);
    gpu_control.sprite = bit_check(control, 1);
    gpu_control.bg = bit_check(control, 0);

    if (bit_check(control, 4)) {
        gpu_control.BgWindowTileData = 0x8000;
        gpu_control.BgTileDataSigned = false;
    } else {
        gpu_control.BgWindowTileData = 0x8800;
        gpu_control.BgTileDataSigned = true;
    }

    if(bit_check(control, 3)){
      gpu_control.bgtilemap = 0x9C00;
    } else {
      gpu_control.bgtilemap = 0x9800;
    }
}

#define set_pixel(x,y,c) pixels[(y * VIEWPORT_WIDTH) + x] = c;

void gpu_init(void){
    memset(VRAM, 0, 0x2000);
    gpu_cycles = 0;
    LY_REG = 0;

    // Initialize GPU control with default values
    gpu_control.enabled = false;
    gpu_control.bg = false;
    gpu_control.window = false;
    gpu_control.sprite = false;
    gpu_control.BgWindowTileData = 0x8800;
    gpu_control.BgTileDataSigned = true;
    gpu_control.bgtilemap = 0x9800;
    gpu_control.windowtilemap = 0x9800;

    // Set default LCDC register value (display enabled, background enabled)
    mem_write(LCDC, 0x91);  // 10010001 - LCD enabled, BG enabled, BG tile data at 0x8000, BG tile map at 0x9800

    // Set default BGP register (white, light gray, dark gray, black)
    mem_write(BGP, 0xFC);   // 11111100 - white, light gray, dark gray, black

    // printf("GPU initialized: LCDC=0x%02X, BGP=0x%02X\n", mem_read(LCDC), mem_read(BGP));

    // Initialize scroll registers
    mem_write(SCX, 0);
    mem_write(SCY, 0);

    // Test tile data initialization removed - let the ROM provide its own data
}

byte gpu_get_mode(void) {
    if (!gpu_control.enabled) return 0;
    if (LY_REG >= 144) return 1;   // VBlank
    if (gpu_cycles < 80) return 2;  // OAM scan
    if (gpu_cycles < 252) return 3; // LCD transfer
    return 0;                        // HBlank
}

byte gpu_read(int pos){
    //printf("reading from gpu %x\n", pos);
    if (pos >= 0x8000 && pos <= 0x9FFF) {
        return VRAM[pos - 0x8000];
    }
    if (pos == STAT) {
        byte lyc_flag = (LY_REG == RAM[LYC]) ? 0x04 : 0x00;
        return 0x80 | (RAM[STAT] & 0x78) | lyc_flag | gpu_get_mode();
    }
    if (pos == LY) {
        // On DMG hardware, LY is a live hardware counter. Reading LY during an
        // instruction reflects the state ~4T (one M-cycle) into the instruction.
        // Our gpu_step only runs after full instructions, so compensate here.
        if (gpu_control.enabled && gpu_cycles + 4 >= 456) {
            byte next_ly = LY_REG + 1;
            if (next_ly > 153) next_ly = 0;
            return next_ly;
        }
        return LY_REG;
    }
    if (pos == SCX) {
        return SCX_REG;
    }
    if (pos == SCY) {
        return SCY_REG;
    }
    if (pos == LCDC) {
        return LCDC_REG;
    }
    if (pos == BGP) {
        return BGP_REG;
    }
    // For other registers like LYC, etc., we need to handle them separately
    return 0;
}

void gpu_write(int pos, byte data){
    if (pos >= 0x8000 && pos <= 0x9FFF) {
        VRAM[pos - 0x8000] = data;
    }
    if (pos == STAT) {
        // Bits 3-6 are writable; bits 0-2 are read-only
        RAM[STAT] = (RAM[STAT] & 0x87) | (data & 0x78);
    }
    if (pos == LCDC) {
        bool was_enabled = bit_check(LCDC_REG, 7);
        bool now_enabled = bit_check(data, 7);
        LCDC_REG = data;
        gpu_parse_control(data);
        if (!was_enabled && now_enabled) {
            // LCD just turned on: start from beginning of frame.
            LY_REG = 0;
            gpu_cycles = 0;
        }
        if (was_enabled && !now_enabled) {
            // LCD turned off: reset state
            LY_REG = 0;
            gpu_cycles = 0;
        }
    }
    if (pos == LY) {
        // LY register is read-only in the Game Boy - ignore writes
        // LY_REG = data;  // REMOVED: LY is read-only
    }
    if (pos == SCX) {
        SCX_REG = data;
    }
    if (pos == SCY) {
        SCY_REG = data;
    }
    if (pos == BGP) {
        BGP_REG = data;
    }
    if (data > 0) {
 //       printf("wrote %x to %x\n", data, pos);
    }
}

uint32_t gpu_pallete_color(byte number, int paletteIndex) {
    const byte config = mem_read(paletteIndex);
    byte resultindex = 0;

    const byte b1 = number * 2;
    const byte b2 = (number * 2) + 1;

    if(bit_check(config, b1)){
        resultindex = bit_set(resultindex, 1);
    }

    if(bit_check(config, b2)){
        resultindex = bit_set(resultindex, 0);
    }

    const uint32_t color = pallette[resultindex];

    return color;
}

void gpu_render_tile(byte ly, int xprefix, int tileIndex, int paletteIndex, bool flipx, bool flipy){

    // Start of tile data
    int actualTileIndex = tileIndex;
    if (gpu_control.BgTileDataSigned) {
        // Convert signed tile index to unsigned
        actualTileIndex = (tileIndex < 128) ? tileIndex : tileIndex - 256;
    }
    const int start = gpu_control.BgWindowTileData + (16 * actualTileIndex);

    const byte high = mem_read(start + ((ly % 8)*2));
    const byte low = mem_read(start + ((ly % 8)*2)+1);
    for (byte i = 0; i < 8; i++) {
      byte wat = 0;
      if(bit_check(low, (7-i))){
        wat |= (1 << 1);
      }
      if(bit_check(high, (7-i))){
        wat |= (1 << 0);
      }

      // Only render non-transparent pixels (color 0 is transparent for sprites)
      if (wat != 0) {
        set_pixel(xprefix+i, ly, gpu_pallete_color(wat, paletteIndex));
      }
    }

}

void gpu_draw_bg(byte ly){
    if (gpu_control.bg) {
        // Debug output removed
        for (int tile = 0; tile < 32; tile++) {
            // Calculate tile position with scrolling
            int tile_x = tile + (SCX_REG / 8);
            int tile_y = (ly / 8) + (SCY_REG / 8);

            // Wrap around for tile map
            tile_x = tile_x % 32;
            tile_y = tile_y % 32;

            // Index of tile from sprite map
            const int tileIndex = mem_read(gpu_control.bgtilemap + (tile_y * 32) + tile_x);

            // Debug output removed

            gpu_render_tile(ly, (tile*8), tileIndex, BGP, false, false);
        }

    } else {
        // Debug output removed
        for (int i = 0; i<VIEWPORT_WIDTH; i++) {
            set_pixel(i, ly, 0xffffff);
        }
    }
    
    // Test line removed
}

void gpu_draw_sprites(byte ly){
    if (gpu_control.sprite) {
        // Iterate through all 40 sprites, looking for
        // ones that overlap within the current LY value
        // Render sprites in reverse order (higher indices first) for proper priority
        for (int i = 39; i >= 0; i--) {
            const int start = 0xFE00 + (i * 4);
            const byte y = mem_read(start + 1);
            const byte x = mem_read(start);

            // Game Boy sprites are positioned 16 pixels above their actual position
            const int actual_y = y - 16;
            
            // does not overlap with our scanline; skip.
            if (actual_y > ly || (actual_y + 8) <= ly) {
                if (y != 0 || x != 0){
                    //printf("SKIPPING sprite at %d %d (LY: %d)\n", y, x, ly);
                }
                continue;
            }

           // printf("not skpping sprite at %d %d (LY: %d)", y, x, ly);
            //printf("NOT SKIPPING sprite at %d %d (LY: %d)\n", y, x, ly);

            const byte tileIndex = mem_read(start + 2);
            const byte flags = mem_read(start + 3);
            const bool flipy = bit_check(flags, 6);
            const bool flipx = bit_check(flags, 5);
            int paletteIndex;
            if (bit_check(flags, 4)) {
                paletteIndex = OBP1;
            } else {
                paletteIndex = OBP0;
            }

            // Game Boy sprites are positioned 8 pixels to the left of their actual position
            const int actual_x = x - 8;
            
            // Only render if sprite is within visible bounds
            if (actual_x >= -8 && actual_x < VIEWPORT_WIDTH) {
                //printf("drawing tile at %d %d\n", ly, actual_x);
                gpu_render_tile(ly, actual_x, tileIndex, paletteIndex, flipx, flipy);
            }
        }
    }
}

void gpu_drawline(byte ly){
    gpu_draw_bg(ly);
    gpu_draw_sprites(ly);
}

void gpu_step(int _cycles){
    if (gpu_control.enabled) {
        byte prev_mode = gpu_get_mode();
        int prev_ly = LY_REG;
        gpu_cycles += _cycles;
        
        if (gpu_cycles >= 456) {
            gpu_cycles -= 456; // preserve remainder for accurate sub-scanline timing

            const byte ly = ++LY_REG;

            if (ly == 144) {
                // VBLANK period starts
                request_interrupt(INTERRUPT_VBLANK);
                // STAT mode 1 interrupt (bit 4)
                if (RAM[STAT] & 0x10) request_interrupt(INTERRUPT_STAT);
            } else if (ly > 153) {
                LY_REG = 0;
            } else if (ly < 144) {
                // draw scanline
                gpu_drawline(ly);
            }

            // STAT mode 2 interrupt: fires at start of each new OAM scan (LY 0-143)
            if (LY_REG < 144 && (RAM[STAT] & 0x20)) {
                request_interrupt(INTERRUPT_STAT);
            }
        }

        // Check for LY=LYC coincidence interrupt (STAT bit 6)
        if ((RAM[STAT] & 0x40) && LY_REG == RAM[LYC] && LY_REG != prev_ly) {
            request_interrupt(INTERRUPT_STAT);
        }

        // STAT mode 0 (H-blank) interrupt: fires when entering H-blank
        byte new_mode = gpu_get_mode();
        if ((RAM[STAT] & 0x08) && new_mode == 0 && prev_mode != 0 && LY_REG < 144) {
            request_interrupt(INTERRUPT_STAT);
        }
    }
}

// OAM corruption helpers. OAM = 20 rows x 8 bytes = 160 bytes at 0xFE00.
// Objects 0-1 (row 0, bytes 0-7) are immune to corruption.
//
// The corruption happens during a specific M-cycle inside the current CPU instruction.
// gpu_step() only runs after the whole instruction, so project gpu_cycles/LY forward by the
// requested T-cycle offset before deciding which OAM row the PPU is scanning.

static int oam_accessed_row_at(int t_offset) {
    if (!gpu_control.enabled) return -1;

    int projected_cycles = gpu_cycles + t_offset;
    int projected_ly = LY_REG;

    // Handle negative projected_cycles (wrap back to previous scanline)
    while (projected_cycles < 0) {
        projected_ly--;
        if (projected_ly < 0) projected_ly = 153;
        projected_cycles += 456;
    }

    while (projected_cycles >= 456) {
        projected_cycles -= 456;
        projected_ly++;
        if (projected_ly > 153) projected_ly = 0;
    }

    if (projected_ly >= 144) return -1;
    if (projected_cycles >= 80) return -1;
    return (projected_cycles / 4) * 8;
}

static word oam_read_word(byte *oam, int offset) {
    return (word)oam[offset] | ((word)oam[offset + 1] << 8);
}
static void oam_write_word(byte *oam, int offset, word val) {
    oam[offset]     = val & 0xFF;
    oam[offset + 1] = (val >> 8) & 0xFF;
}

// Write corruption: triggered by INC/DEC rr, PUSH, CALL, RST with reg in OAM range.
// Formula: new_word0 = ((a ^ c) & (b ^ c)) ^ c; bytes[2..7] = copy from prev row.
static int oam_corrupt_debug = 0; // set to 1 to enable debug logging
void oam_write_corrupt_at(word reg_val, int t_offset) {
    if (reg_val < 0xFE00 || reg_val > 0xFEFF) return;
    int off_n = oam_accessed_row_at(t_offset);
    if (oam_corrupt_debug) {
        extern word PC;
        fprintf(stderr, "[OAM_CORRUPT] PC=%04X reg=%04X t_offset=%d gpu_cycles=%d LY=%d off_n=%d\n",
                PC, reg_val, t_offset, gpu_cycles, LY_REG, off_n);
    }
    if (off_n < 8) return; // -1 = not Mode 2, 0 = immune row

    byte *oam = &RAM[0xFE00];
    int off_prev = off_n - 8;

    word a = oam_read_word(oam, off_n);
    word b = oam_read_word(oam, off_prev);
    word c = oam_read_word(oam, off_prev + 4);
    word result = ((a ^ c) & (b ^ c)) ^ c;
    oam_write_word(oam, off_n, result);
    for (int i = 2; i < 8; i++) oam[off_n + i] = oam[off_prev + i];
}

void oam_write_corrupt(word reg_val) {
    oam_write_corrupt_at(reg_val, 0);
}

// Read corruption: triggered by POP, RET when stack read hits OAM range, or LD A,(HL±).
// Dispatches to the correct formula based on accessed row, matching SameBoy behavior.
static void oam_read_corrupt_at(word addr, int t_offset) {
    if (addr < 0xFE00 || addr > 0xFEFF) return;
    int row = oam_accessed_row_at(t_offset);
    if (row < 8 || row >= 0x98) return;

    byte *oam = &RAM[0xFE00];

    if ((row & 0x18) == 0x10) {
        // Secondary case: rows 0x10, 0x30, 0x50, 0x70, 0x90
        // Formula: (b & (a|c|d)) | (a&c&d), writes only to prev_word0
        // Inner copy: oam[row-16..row-9] = oam[row-8..row-1]
        word a = oam_read_word(oam, row - 16); // base[-8]
        word b = oam_read_word(oam, row - 8);  // base[-4] = prev_word0
        word c = oam_read_word(oam, row);      // base[0]  = curr_word0
        word d = oam_read_word(oam, row - 4);  // base[-2] = prev_word2
        word glitch = (b & (a | c | d)) | (a & c & d);
        oam_write_word(oam, row - 8, glitch);
        for (int i = 0; i < 8; i++) oam[row - 16 + i] = oam[row - 8 + i];
    } else if ((row & 0x18) == 0x00) {
        // Tertiary/Quaternary case: rows 0x20, 0x40, 0x60, 0x80...
        if (row == 0x40) {
            // Quaternary (DMG-B: emulate zero-output for non-deterministic cases)
            // params: a=oam[0:1], b=oam[row], c=oam[row-4], d=oam[row-6],
            //         e=oam[row-8], f=oam[row-14], g=oam[row-16], h=oam[row-32]
            word b = oam_read_word(oam, row);      // base[0]
            word c = oam_read_word(oam, row - 4);  // base[-2]
            word d = oam_read_word(oam, row - 6);  // base[-3]
            word e = oam_read_word(oam, row - 8);  // base[-4]
            word f = oam_read_word(oam, row - 14); // base[-7]
            word g = oam_read_word(oam, row - 16); // base[-8]
            word h = oam_read_word(oam, row - 32); // base[-16]
            word glitch = (e & (h | g | ((word)(~d) & f) | c | b)) | (c & g & h);
            oam_write_word(oam, row - 8, glitch);
            for (int i = 0; i < 8; i++) oam[row - 16 + i] = oam[row - 32 + i] = oam[row - 8 + i];
        } else {
            // Tertiary case: rows 0x20, 0x60, 0x80, etc.
            // params: a=oam[row], b=oam[row-4], c=oam[row-8], d=oam[row-16], e=oam[row-32]
            word a = oam_read_word(oam, row);      // base[0]
            word b = oam_read_word(oam, row - 4);  // base[-2]
            word c = oam_read_word(oam, row - 8);  // base[-4]
            word d = oam_read_word(oam, row - 16); // base[-8]
            word e = oam_read_word(oam, row - 32); // base[-16]
            word glitch;
            if (row == 0x20)
                glitch = (c & (a | b | d | e)) | (a & b & d & e); // tertiary_read_2
            else if (row == 0x60)
                glitch = (c & (a | b | d | e)) | (b & d & e);     // tertiary_read_3
            else
                glitch = c | (a & b & d & e);                      // tertiary_read_1
            oam_write_word(oam, row - 8, glitch);
            for (int i = 0; i < 8; i++) oam[row - 16 + i] = oam[row - 32 + i] = oam[row - 8 + i];
        }
    } else {
        // Standard case: rows with (row & 0x18) == 0x08 or 0x18
        // Formula: b | (a & c), writes glitch to BOTH curr and prev word0
        word a = oam_read_word(oam, row);     // base[0]  = curr_word0
        word b = oam_read_word(oam, row - 8); // base[-4] = prev_word0
        word c = oam_read_word(oam, row - 4); // base[-2] = prev_word2
        word glitch = b | (a & c);
        oam_write_word(oam, row,     glitch);
        oam_write_word(oam, row - 8, glitch);
    }

    // Global copy always: oam[row..row+7] = oam[row-8..row-1]
    for (int i = 0; i < 8; i++) oam[row + i] = oam[row - 8 + i];
    if (row == 0x80) // special: row 0x80 also copies to row 0x00
        for (int i = 0; i < 8; i++) oam[i] = oam[row + i];
}

void oam_read_corrupt(word addr) {
    oam_read_corrupt_at(addr, 4); // standard: read happens at M2+ (1 M-cycle offset)
}

//
// CART
//

char cart_name[30];
byte cart_type;
byte *cart_data;
int cart_rom_banks;   // total number of 16KB ROM banks
int cart_rom_bank = 1; // current switchable ROM bank (for $4000-$7FFF)
byte ext_ram[0x2000]; // 8KB external RAM for MBC1+RAM (cart types 0x02, 0x03)

byte cart_read(int pos) {
    if (pos < 0x4000) {
        return cart_data[pos]; // bank 0 is always fixed
    }
    // $4000-$7FFF: switchable ROM bank
    int bank_offset = cart_rom_bank * 0x4000 + (pos - 0x4000);
    return cart_data[bank_offset];
}

void cart_write(int pos, byte data) {
    if (pos <= 0x1FFF) {
        // MBC1 RAM enable: ignored (we always allow ext_ram access)
        return;
    }
    if (pos <= 0x3FFF) {
        // MBC1 ROM bank number (lower 5 bits)
        int bank = data & 0x1F;
        if (bank == 0) bank = 1; // MBC1: bank 0 maps to bank 1
        // Mask to valid bank range
        int mask = cart_rom_banks - 1;
        cart_rom_bank = bank & mask;
        if (cart_rom_bank == 0) cart_rom_bank = 1;
        return;
    }
    if (pos <= 0x5FFF) {
        // MBC1 RAM bank or upper ROM bank bits: ignore for now
        return;
    }
    if (pos <= 0x7FFF) {
        // MBC1 mode select: ignore
        return;
    }
    // other cart writes: silently ignore
}

void cart_load(char *path) {
    FILE *fileptr;
    long filelen;
    fileptr = fopen(path, "rb");
    fseek(fileptr, 0, SEEK_END);
    filelen = ftell(fileptr);
    rewind(fileptr);
    cart_data = (byte *)malloc(filelen * sizeof(byte));
    if (cart_data == NULL) {
        perror("malloc");
        exit(1);
    }
    fread(cart_data, filelen, 1, fileptr);
    fclose(fileptr);

    int i = 0;

    for (int pos = 0x0134; pos <= 0x0144; pos++, i++) {
        const byte c = cart_data[pos];
        if (c == 0 || (char) c == ' ') {
            break;
        }
        cart_name[i] = c;
    }
    cart_name[i+1] = '\0';
    cart_type = cart_data[0x147];
    cart_rom_banks = (int)filelen / 0x4000;
    cart_rom_bank = 1; // reset to default bank

    printf("Loaded %s\n", cart_name);

    if (cart_type > 3) {
        printf("Invalid romtype %x: MBC not yet supported\n", cart_type);
        exit(1);
    }
}

//
// Joypad
//

byte joypad;

typedef struct {
  bool A, B, SELECT, START,
    UP, DOWN, LEFT, RIGHT;
} JOYPADBUTTONS;

JOYPADBUTTONS joypad_buttons;

void joypad_init(void){
  joypad_buttons.START = false;
  joypad_buttons.SELECT = false;
  joypad_buttons.A = false;
  joypad_buttons.B = false;
  joypad_buttons.UP = false;
  joypad_buttons.DOWN = false;
  joypad_buttons.LEFT = false;
  joypad_buttons.RIGHT = false;
}

byte joypad_read(void){
  // Want button keys
  byte result = 0;
  if (!bit_check(joypad, 5)){
      result = bit_def(result, 3, !joypad_buttons.START);
      result = bit_def(result, 2, !joypad_buttons.SELECT);
      result = bit_def(result, 1, !joypad_buttons.B);
      result = bit_def(result, 0, !joypad_buttons.A);
  }

  // Want arrows
  if (!bit_check(joypad, 4)){
      result = bit_def(result, 3, !joypad_buttons.DOWN);
      result = bit_def(result, 2, !joypad_buttons.UP);
      result = bit_def(result, 1, !joypad_buttons.LEFT);
      result = bit_def(result, 0, !joypad_buttons.RIGHT);
  }

  return result;
}

void joypad_write(byte data){
  // Only allow setting the RW bits
  if (bit_check(data, 5)) {
    joypad = bit_set(joypad, 5);
  } else {
    joypad = bit_clear(joypad, 5);
  }
  if (bit_check(data, 4)) {
    joypad = bit_set(joypad, 4);
  } else {
    joypad = bit_clear(joypad, 4);
  }
}


//
// RAM
//
byte RAM[0xffff + 1];
bool inBios;
bool bailAfterBios;

void request_interrupt(byte interrupt){
    RAM[INTERRUPT_FLAGS] |= interrupt;
}

//
// Timer
//
#define REG_DIV  0xFF04
#define REG_TIMA 0xFF05
#define REG_TMA  0xFF06
#define REG_TAC  0xFF07

int timer_div_cycles  = 0;
int timer_tima_cycles = 0;

// Clock dividers for TAC bits 1-0
static const int timer_clocks[4] = { 1024, 16, 64, 256 };

void timer_step(int cpu_cycles) {
    // Divider: increments at CPU/256
    timer_div_cycles += cpu_cycles;
    while (timer_div_cycles >= 256) {
        timer_div_cycles -= 256;
        RAM[REG_DIV]++;
    }

    // TIMA: only counts when bit 2 of TAC is set
    if (RAM[REG_TAC] & 0x04) {
        int threshold = timer_clocks[RAM[REG_TAC] & 0x03];
        timer_tima_cycles += cpu_cycles;
        while (timer_tima_cycles >= threshold) {
            timer_tima_cycles -= threshold;
            RAM[REG_TIMA]++;
            if (RAM[REG_TIMA] == 0) {
                RAM[REG_TIMA] = RAM[REG_TMA];
                request_interrupt(INTERRUPT_TIMER);
            }
        }
    }
}

void mem_dma(byte data) {
	const int start = (int)data * 100;
	for (int i = 0; i <= 0x9f; i++) {
    mem_write(0xFE00+i, mem_read(start+i));
	}
}

byte mem_read(int pos) {
    if (pos < 256 && inBios) {
        return bios[pos];
    } else if (pos <= 0x7FFF) {
        return cart_read(pos);
    } else if (pos >= 0x8000 && pos <= 0x9FFF) {
        return gpu_read(pos);
    } else if (pos >= 0xA000 && pos <= 0xBFFF) {
        return ext_ram[pos - 0xA000];
    }
    // FIXME: lots missing here
    else {
        switch (pos) {
            case BGP:
            case LCDC:
            case STAT:
            case LY:
                return gpu_read(pos);
            case LYC:
            case SCY:
            case WX:
            case WY:
                return gpu_read(pos);
            case JOYPAD:
                return joypad_read();
            case INTERRUPT_FLAGS:
                // DMG: upper 3 bits of IF are always 1
                return RAM[pos] | 0xE0;
            case 0xFF01:
                return serial_sb;
            case 0xFF02:
                return RAM[pos];
            default:
                return RAM[pos];
        }
    }
}

void mem_write(int pos, byte data) {
    if (pos <= 0x7FFF) {
        cart_write(pos, data);
        return;
    }
    switch (pos) {
        case BGP:
        case LCDC:
        case STAT:
        case LY:
        case LYC:
        case SCY:
        case WX:
        case WY:
            gpu_write(pos, data);
            break;
        case JOYPAD:
            joypad_write(data);
            break;
        case DMA:
            mem_dma(data);
        break;
        case 0xFF01:
            serial_sb = data;
            break;
        case 0xFF02:
            if (bit_check(data, 7)) {
                if (serial_buf_len < (int)(sizeof(serial_buf) - 1)) {
                    serial_buf[serial_buf_len++] = (char)serial_sb;
                    serial_buf[serial_buf_len] = '\0';
                }
                fputc(serial_sb, stdout);
                fflush(stdout);
                // Clear bit 7 immediately (transfer complete) so ROM doesn't busy-wait
                RAM[pos] = data & ~0x80;
                // Trigger serial interrupt (INT 3 = $08)
                RAM[INTERRUPT_FLAGS] |= 0x08;
            } else {
                RAM[pos] = data;
            }
            break;
        case REG_DIV:
            // Writing any value to DIV resets it to 0
            RAM[REG_DIV] = 0;
            timer_div_cycles = 0;
            break;
        case REG_TIMA:
        case REG_TMA:
        case REG_TAC:
            RAM[pos] = data;
            break;
        default:
            if (pos == 0xFF50 && inBios) {
                inBios = false;
                printf("bios disabled\n");
            } else if (pos >= 0x8000 && pos <= 0x9FFF) {
                gpu_write(pos, data);
            } else if (pos >= 0xA000 && pos <= 0xBFFF) {
                ext_ram[pos - 0xA000] = data;
                // Detect blargg test completion: A000 written with non-0x80 value
                // when magic bytes DE B0 61 are present at A001-A003
                if (pos == 0xA000 && data != 0x80 &&
                    ext_ram[1] == 0xDE && ext_ram[2] == 0xB0 && ext_ram[3] == 0x61) {
                    blargg_done = true;
                }
            } else {
                RAM[pos] = data;
            }
    }
}

char *serial_get_output(void) {
    serial_buf[serial_buf_len] = '\0';
    return serial_buf;
}

void mem_init(void){
    memset(RAM, 0, 0xffff + 1);
    memset(ext_ram, 0, sizeof(ext_ram));
    serial_buf_len = 0;
    serial_buf[0] = '\0';
    serial_sb = 0;
    blargg_done = false;
    inBios = true;
    bailAfterBios = true;
    bailAfterBios = false;
}

//
// CPU
//

#define FLAG_Z 7 // Zero
#define FLAG_N 6 // Subtraction
#define FLAG_H 5 // Half carry
#define FLAG_C 4 // Cary

byte F, A, C, B, E, D, L, H;

// cycles declared at top of file (before gpu_write) for accessibility.
// Tracks timer cycles pre-stepped mid-instruction via cpu_read/cpu_write.
// Reset before each instruction; used to avoid double-counting in the main loops.
int instr_timer_cycles;
word PC;
word SP;
bool interrupts;
int ei_delay;
bool halted;
bool halt_bug_active; // HALT bug: next instruction's PC doesn't increment on opcode fetch


void cpu_init(void){
    F = 0;
    A = 0;
    C = 0;
    B = 0;
    E = 0;
    D = 0;
    L = 0;
    H = 0;
    cycles = 0;
    instr_timer_cycles = 0;
    PC = 0;
    SP = 0;
    interrupts = false;
    ei_delay = 0;
    halted = false;
    halt_bug_active = false;
}

void cpu_init_debug_file(void){
  debug_logfile = fopen("debug_out.txt", "w");
  if (!debug_logfile) {
    perror("fopen");
    exit(1);
  }
}

void cpu_close_debug_file(void){
  if (debug_logfile != NULL) {
    fclose(debug_logfile);
  }
}

void cpu_fake_init(void){
    F = 0xB0;
    A = 0x01;
    C = 0x13;
    B = 0x00;
    E = 0xD8;
    D = 0x00;
    L = 0x4D;
    H = 0x01;
    cycles = 0;
    instr_timer_cycles = 0;
    PC = 0x0100;
    SP = 0xFFFE;
    interrupts = false;
    ei_delay = 0;
    halted = false;
    halt_bug_active = false;
    // We're skipping the BIOS, so mark it as done so ROM interrupt
    // vectors at 0x40-0x60 are read from cart, not BIOS.
    inBios = false;
}

void cpu_debug_log(void) {
  fprintf(
      debug_logfile,
      "A:%02X F:%02X B:%02X C:%02X D:%02X E:%02X H:%02X L:%02X SP:%04X PC:%04X PCMEM:%02X,%02X,%02X,%02X\n",
      A, F, B, C, D, E, H, L, SP, PC,
      mem_read(PC), mem_read(PC+1), mem_read(PC+2), mem_read(PC+3));
}

void push_stack(word data) {
    const byte f1 = data & 0x00ff;
    const byte f2 = (data & 0xff00) >> 8;

    SP -= 2;
    mem_write(SP, f1);
    mem_write(SP + 1, f2);
}

word pop_stack(void) {
    const word f1 = mem_read(SP);
    const word f2 = mem_read(SP + 1);
    SP += 2;
    return (f2 << 8) | f1;
}

word peek_stack(void) {
    const word f1 = mem_read(SP);
    const word f2 = mem_read(SP + 1);
    return (f2 << 8) | f1;
}

word AF(void) {
    return F | (A << 8);
}

word HL(void) {
    return L | (H << 8);
}

word DE(void) {
    return E | (D << 8);
}

word BC(void) {
    return C | (B << 8);
}

void dump_regs(void) {
  printf("REGS: AF: %04x BC: %04x DE: %04x HL: %04x SP: %04x PC: %04x\n",
    AF(),
    BC(),
    DE(),
    HL(),
    SP,
    PC
  );
}


void setAF(word data) {
    F = (data & 0x00ff) & 0xf0;
    A = (data & 0xff00) >> 8;
}

void setBC(word data) {
    B = (byte) (data >> 8);
    C = (byte) data;
}

void setDE(word data) {
    D = (byte) (data >> 8);
    E = (byte) (data);
}

void setHL(word data) {
    H = (byte) (data >> 8);
    L = (byte) data;
}

word HLDec(void) {
    const word hl = HL();
    setHL(hl - 1);
    // System.out.println("Old HL: 0x"+Integer.toHexString(hl));
    return hl;
}

word HLInc(void) {
    const word hl = HL();
    setHL(hl + 1);
    return hl;
}

byte cpu_read(int loc) {
    cycles += 4;
    timer_step(4);            // step timer before memory access for sub-instruction timing
    instr_timer_cycles += 4;
    return mem_read(loc);
}

void cpu_write(int loc, byte value) {
    cycles += 4;
    timer_step(4);            // step timer before memory access for sub-instruction timing
    instr_timer_cycles += 4;
    mem_write(loc, value);
}

byte cpu_read_next(void) {
  const byte data = cpu_read(PC);
  PC++;
  return data;
}

word cpu_read16(void) {
  const byte lo = cpu_read_next();
  const byte hi = cpu_read_next();
  return lo | (hi << 8);
}

void do_interrupts(void){
  // Any pending interrupt wakes CPU from HALT regardless of IME
  // Mask to bits 0-4 only; IF bits 5-7 are always 1 (hardware artifact)
  byte enabled_bits = mem_read(INTERRUPT_ENABLE) & 0x1F;
  byte flag_bits = mem_read(INTERRUPT_FLAGS) & 0x1F;
  if ((flag_bits & enabled_bits) > 0) {
      halted = false;
  }

  if (interrupts) {
    if(enabled_bits > 0){
      byte enabled = flag_bits & enabled_bits;

      // If we have any, disable them as we run them
      if(enabled > 0){
        interrupts = false;

        for (int i = 0; i<5; i++) {
          byte interrupt = INTERRUPT_PRIORITY[i];
          if ((enabled & interrupt) != 0) {
            flag_bits = flag_bits & ~interrupt;

            //printf("Doing interrupt %s (%x)\n", INTERRUPT_NAMES[i], INTERRUPT_OFFSETS[i]);
            // Interrupt dispatch takes 5 M-cycles (20 cycles):
            // 2 idle + 2 stack push writes + 1 vector load
            cycles += 20;
            push_stack(PC);

            PC = INTERRUPT_OFFSETS[i];
            break;
          }
        }

        RAM[INTERRUPT_FLAGS] = flag_bits;
      }
    }
  }
}

//
// ALGS
//
void setFlag(byte flag, bool value) {
    F = value ? bit_set(F, flag) : bit_clear(F, flag);
}

bool CheckFlag(byte flag) {
    return bit_check(F, flag);
}

void clearFlags(void) {
    F = 0;
}

byte Inc(byte reg) {
    reg++;
    setFlag(FLAG_Z, reg == 0);
    setFlag(FLAG_N, false);
    setFlag(FLAG_H, (reg & 0xf)==0);
    return reg;
}

byte Dec(byte reg) {
    reg--;
    setFlag(FLAG_Z, reg == 0);
    setFlag(FLAG_N, true);
    setFlag(FLAG_H, (reg & 0xf)==0xf);
    return reg;
}

byte Sub(byte arg) {
    const word result = A - arg;
    setFlag(FLAG_Z,  (result&0x0ff) == 0);
    setFlag(FLAG_N, true);
    setFlag(FLAG_C, (result & 0xFF00)!=0);
    setFlag(FLAG_H,  (A & 0x0F) < (arg & 0x0F));
    return  result&0x0ff;
}

byte Add(byte arg) {
    const byte result = A + arg;
    setFlag(FLAG_Z, result == 0);
    setFlag(FLAG_N, false);
    setFlag(FLAG_H, (A & 0x0F) + (arg & 0x0F) > 0x0F);
    setFlag(FLAG_C, 0xFF - A < arg);
    return result;
}

byte Adc(byte arg) {
    int carry = CheckFlag(FLAG_C) ? 1 : 0;
    int result = A + arg + carry;
    setFlag(FLAG_N, false);
    setFlag(FLAG_C, result > 0xff);
    setFlag(FLAG_H, (A & 0xf) + (arg & 0xf) + carry > 0xf);
    setFlag(FLAG_Z, (byte)result == 0);
    return (byte)result;
}

byte Sbc(byte arg) {
    int carry = CheckFlag(FLAG_C) ? 1 : 0;
    int result = A - arg - carry;
    setFlag(FLAG_N, true);
    setFlag(FLAG_C, result < 0);
    setFlag(FLAG_H, (A & 0xf) - (arg & 0xf) - carry < 0);
    setFlag(FLAG_Z, (byte)result == 0);
    return (byte)result;
}

byte And(byte arg) {
    const byte v = A & arg;
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    setFlag(FLAG_H, true);
    return v;
}

byte Or(byte arg) {
    const byte v = A | arg;
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte Xor(byte arg) {
    const byte v = A ^ arg;
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte Swap(byte arg) {
    const byte v = ((arg & 0x0F)<<4 | (arg & 0xF0)>>4);
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte RL(byte arg) {
    // XXX: may be correct per https://github.com/daveallie/rustyboy/blob/master/src/register/alu.rs
    bool oldC = (arg & 0x80) == 0x80;
    arg <<= 1;
    if (CheckFlag(FLAG_C)) {
        arg |= 1;
    }
    clearFlags();
    setFlag(FLAG_C, oldC);
    setFlag(FLAG_Z, arg == 0); // For RLA this is later set to false.
    return arg;
}

byte Rr(byte arg, bool isA) {
    bool lsb = (arg & 1) > 0;
    byte result;
    if (CheckFlag(FLAG_C)){
        result = (arg >> 1) | 0x80;
    } else {
        result = (arg >> 1);
    }
    setFlag(FLAG_Z, result == 0);
    setFlag(FLAG_C, lsb);
    setFlag(FLAG_H, false);
    setFlag(FLAG_N, false);
    return result;
}


byte Rlc(byte arg) {
    // XXX: may be correct per https://github.com/daveallie/rustyboy/blob/master/src/register/alu.rs
    bool oldC = (arg & 0x80) == 0x80;
    arg <<= 1;
    if(oldC){
        arg |= 0x01;
    }
    clearFlags();
    setFlag(FLAG_C, oldC);
    setFlag(FLAG_Z, arg == 0);
    return arg;
}

byte Sla(byte arg) {
    // XXX: may be correct per https://github.com/daveallie/rustyboy/blob/master/src/register/alu.rs
    clearFlags();
    const byte v = arg << 1;
    setFlag(FLAG_C, (arg & 0x80) == 0x80);
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte Srl(byte arg) {
    clearFlags();
    const byte v = arg >> 1;
    setFlag(FLAG_C, (arg & 0x1) == 0x1);
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte Rrc(byte arg) {
    bool lsb = (arg & 0x01) != 0;
    arg = (arg >> 1) | (lsb ? 0x80 : 0);
    clearFlags();
    setFlag(FLAG_C, lsb);
    setFlag(FLAG_Z, arg == 0);
    return arg;
}

byte Sra(byte arg) {
    clearFlags();
    const byte v = (arg & 0x80) | (arg >> 1);
    setFlag(FLAG_C, (arg & 0x01) != 0);
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte DAA(byte reg) {
    //
    // Ripped from https://forums.nesdev.com/viewtopic.php?t=15944#p196282
    //
    if (!CheckFlag(FLAG_N)) { // after an addition, adjust if (half-)carry occurred or if result is out of bounds
        if (CheckFlag(FLAG_C) || reg > 0x99) {
            reg += 0x60;
            setFlag(FLAG_C, true);
        }
        if (CheckFlag(FLAG_H) || (reg&0x0f) > 0x09) {
            reg += 0x6;
        }
    } else { // after a subtraction, only adjust if (half-)carry occurred
        if (CheckFlag(FLAG_C)) {
            reg -= 0x60;
        }
        if (CheckFlag(FLAG_H)) {
            reg -= 0x6;
        }
    }
    // these flags are always updated
    setFlag(FLAG_Z, reg == 0);
    setFlag(FLAG_H, false);
    return reg;
}

void Bit(byte target, int bit) {
    setFlag(FLAG_Z, !bit_check(target, (byte) bit));
    setFlag(FLAG_N, false);
    setFlag(FLAG_H, true);
}

void CP(byte to) {
    byte tmp = A;
    Sub(to);
    A = tmp;
}

//
// CPU helpers
//

void exec_ext_op(byte opcode);

void exec_op(byte opcode){
    byte offset;
    int pos;
    switch (opcode){
        // NOP
        case 0:
            break;

        // Extended opcodes
        case 0xcb:
            exec_ext_op(cpu_read_next());
            break;

// START GENERATED

    // LD BC <- d16
    case 0x01:
      setBC(cpu_read16());
      break;

    // LD (BC) <- A
    case 0x02:
      cpu_write(BC(), A);
      break;

    // INC BC
    case 0x03:
      { word v = BC(); oam_write_corrupt(v); setBC(v + 1); }
      cycles += 4;
      break;

    // INC B
    case 0x04:
      B = Inc(B);
      break;

    // DEC B
    case 0x05:
      B = Dec(B);
      break;

    // LD B <- d8
    case 0x06:
      B = cpu_read_next();
      break;

    // RLCA
    case 0x07:
      A = Rlc(A);
      setFlag(FLAG_Z, false);
      break;

    // LD (a16) <- SP
    case 0x08:
      pos = cpu_read16();
      cpu_write(pos, SP&0x00ff);
      cpu_write(pos+1, (SP&0xff00)>>8);
      break;

    // ADD HL += BC
    case 0x09:
        {
            const word target = HL();
            const word source = BC();
            const word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x0FFF)+(source&0x0FFF) > 0x0FFF);
            cycles += 4;
        }
      break;

    // LD A <- (BC)
    case 0x0a:
      A = cpu_read(BC());
      break;

    // DEC BC
    case 0x0b:
      { word v = BC(); oam_write_corrupt(v); setBC(v - 1); }
      cycles += 4;
      break;

    // INC C
    case 0x0c:
      C = Inc(C);
      break;

    // DEC C
    case 0x0d:
      C = Dec(C);
      break;

    // LD C <- d8
    case 0x0e:
      C = cpu_read_next();
      break;

    // RRCA
    case 0x0f:
      {
        bool lsb = (A & 1) != 0;
        A = (A >> 1) | (lsb ? 0x80 : 0);
        clearFlags();
        setFlag(FLAG_C, lsb);
      }
      break;

    // STOP
    case 0x10:
      cpu_read_next(); // consume 0x00 operand
      break;

    // LD DE <- d16
    case 0x11:
      setDE(cpu_read16());
      break;

    // LD (DE) <- A
    case 0x12:
      cpu_write(DE(), A);
      break;

    // INC DE
    case 0x13:
      { word v = DE(); oam_write_corrupt(v); setDE(v + 1); }
      cycles += 4;
      break;

    // INC D
    case 0x14:
      D = Inc(D);
      break;

    // DEC D
    case 0x15:
      D = Dec(D);
      break;

    // LD D <- d8
    case 0x16:
      D = cpu_read_next();
      break;

    // RLA
    case 0x17:
      A = RL(A);
      setFlag(FLAG_Z, false);
      break;

    // JR r8
    case 0x18:
      offset = cpu_read_next();
      PC += (int8_t) offset;
      cycles += 4;
      break;

    // ADD HL += DE
    case 0x19:
        {
            const word target = HL();
            const word source = DE();
            const word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x0FFF)+(source&0x0FFF) > 0x0FFF);
            cycles += 4;
        }
      break;

    // LD A <- (DE)
    case 0x1a:
      A = cpu_read(DE());
      break;

    // DEC DE
    case 0x1b:
      { word v = DE(); oam_write_corrupt(v); setDE(v - 1); }
      cycles += 4;
      break;

    // INC E
    case 0x1c:
      E = Inc(E);
      break;

    // DEC E
    case 0x1d:
      E = Dec(E);
      break;

    // LD E <- d8
    case 0x1e:
      E = cpu_read_next();
      break;

    // RRA
    case 0x1f:
      A = Rr(A, true);
      setFlag(FLAG_Z, false);
      break;

    // JR NZ
    case 0x20:
      offset = cpu_read_next();
      if (!CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // LD HL <- d16
    case 0x21:
      setHL(cpu_read16());
      break;

    // LD (HL+) <- A
    case 0x22:
      cpu_write(HLInc(), A);
      break;

    // INC HL
    case 0x23:
      { word v = HL(); oam_write_corrupt(v); setHL(v + 1); }
      cycles += 4;
      break;

    // INC H
    case 0x24:
      H = Inc(H);
      break;

    // DEC H
    case 0x25:
      H = Dec(H);
      break;

    // LD H <- d8
    case 0x26:
      H = cpu_read_next();
      break;

    // DAA
    case 0x27:
      A = DAA(A);
      break;

    // JR Z
    case 0x28:
      offset = cpu_read_next();
      if (CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // ADD HL += HL
    case 0x29:
        {
            const word target = HL();
            const word source = HL();
            const word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x0FFF)+(source&0x0FFF) > 0x0FFF);
            cycles += 4;
        }
      break;

    // LD A <- (HL+)
    case 0x2a:
      {
        word hl = HL();
        oam_read_corrupt_at(hl, 0);
        A = cpu_read(hl);
        setHL(hl + 1);
      }
      break;

    // DEC HL
    case 0x2b:
      { word v = HL(); oam_write_corrupt(v); setHL(v - 1); }
      cycles += 4;
      break;

    // INC L
    case 0x2c:
      L = Inc(L);
      break;

    // DEC L
    case 0x2d:
      L = Dec(L);
      break;

    // LD L <- d8
    case 0x2e:
      L = cpu_read_next();
      break;

    // CPL
    case 0x2f:
      A = ~A;
      setFlag(FLAG_N, true);
      setFlag(FLAG_H, true);
      break;

    // JR NC
    case 0x30:
      offset = cpu_read_next();
      if (!CheckFlag(FLAG_C)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // LD SP <- d16
    case 0x31:
      SP = cpu_read16();
      break;

    // LD (HL-) <- A
    case 0x32:
      cpu_write(HLDec(), A);
      break;

    // INC SP
    case 0x33:
      oam_write_corrupt(SP); SP++;
      cycles += 4;
      break;

    // INC (HL)
    case 0x34:
      {
          const byte source = cpu_read(HL());
          const byte result = Inc(source);
          cpu_write(HL(), result);
      }
      break;

    // DEC (HL)
    case 0x35:
      {
          const byte source = cpu_read(HL());
          const byte result = Dec(source);
          cpu_write(HL(), result);
      }
      break;

    // LD (HL) <- d8
    case 0x36:
      cpu_write(HL(), cpu_read_next());
      break;

    // SCF
    case 0x37:
      setFlag(FLAG_N, false);
      setFlag(FLAG_H, false);
      setFlag(FLAG_C, true);
      break;

    // JR C
    case 0x38:
      offset = cpu_read_next();
      if (CheckFlag(FLAG_C)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // ADD HL += SP
    case 0x39:
        {
            const word target = HL();
            const word source = SP;
            const word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x0FFF)+(source&0x0FFF) > 0x0FFF);
            cycles += 4;
        }
      break;

    // LD A <- (HL-)
    case 0x3a:
      {
        word hl = HL();
        oam_read_corrupt_at(hl, 0);
        A = cpu_read(hl);
        setHL(hl - 1);
      }
      break;

    // DEC SP
    case 0x3b:
      oam_write_corrupt(SP); SP--;
      cycles += 4;
      break;

    // INC A
    case 0x3c:
      A = Inc(A);
      break;

    // DEC A
    case 0x3d:
      A = Dec(A);
      break;

    // LD A <- d8
    case 0x3e:
      A = cpu_read_next();
      break;

    // CCF
    case 0x3f:
      setFlag(FLAG_N, false);
      setFlag(FLAG_H, false);
      setFlag(FLAG_C, !CheckFlag(FLAG_C));
    // LD B <- B (nop)
    case 0x40:
      break;

    // LD B <- C
    case 0x41:
      B = C;
      break;

    // LD B <- D
    case 0x42:
      B = D;
      break;

    // LD B <- E
    case 0x43:
      B = E;
      break;

    // LD B <- H
    case 0x44:
      B = H;
      break;

    // LD B <- L
    case 0x45:
      B = L;
      break;

    // LD B <- (HL)
    case 0x46:
      B = cpu_read(HL());
      break;

    // LD B <- A
    case 0x47:
      B = A;
      break;

    // LD C <- B
    case 0x48:
      C = B;
      break;

    // LD C <- C
    case 0x49:
      break;

    // LD C <- D
    case 0x4a:
      C = D;
      break;

    // LD C <- E
    case 0x4b:
      C = E;
      break;

    // LD C <- H
    case 0x4c:
      C = H;
      break;

    // LD C <- L
    case 0x4d:
      C = L;
      break;

    // LD C <- (HL)
    case 0x4e:
      C = cpu_read(HL());
      break;

    // LD C <- A
    case 0x4f:
      C = A;
      break;

    // LD D <- B
    case 0x50:
      D = B;
      break;

    // LD D <- C
    case 0x51:
      D = C;
      break;

    // LD D <- D
    case 0x52:
      break;

    // LD D <- E
    case 0x53:
      D = E;
      break;

    // LD D <- H
    case 0x54:
      D = H;
      break;

    // LD D <- L
    case 0x55:
      D = L;
      break;

    // LD D <- (HL)
    case 0x56:
      D = cpu_read(HL());
      break;

    // LD D <- A
    case 0x57:
      D = A;
      break;

    // LD E <- B
    case 0x58:
      E = B;
      break;

    // LD E <- C
    case 0x59:
      E = C;
      break;

    // LD E <- D
    case 0x5a:
      E = D;
      break;

    // LD E <- E
    case 0x5b:
      break;

    // LD E <- H
    case 0x5c:
      E = H;
      break;

    // LD E <- L
    case 0x5d:
      E = L;
      break;

    // LD E <- (HL)
    case 0x5e:
      E = cpu_read(HL());
      break;

    // LD E <- A
    case 0x5f:
      E = A;
      break;

    // LD H <- B
    case 0x60:
      H = B;
      break;

    // LD H <- C
    case 0x61:
      H = C;
      break;

    // LD H <- D
    case 0x62:
      H = D;
      break;

    // LD H <- E
    case 0x63:
      H = E;
      break;

    // LD H <- H
    case 0x64:
      break;

    // LD H <- L
    case 0x65:
      H = L;
      break;

    // LD H <- (HL)
    case 0x66:
      H = cpu_read(HL());
      break;

    // LD H <- A
    case 0x67:
      H = A;
      break;

    // LD L <- B
    case 0x68:
      L = B;
      break;

    // LD L <- C
    case 0x69:
      L = C;
      break;

    // LD L <- D
    case 0x6a:
      L = D;
      break;

    // LD L <- E
    case 0x6b:
      L = E;
      break;

    // LD L <- H
    case 0x6c:
      L = H;
      break;

    // LD L <- L
    case 0x6d:
      break;

    // LD L <- (HL)
    case 0x6e:
      L = cpu_read(HL());
      break;

    // LD L <- A
    case 0x6f:
      L = A;
      break;

    // LD (HL) <- B
    case 0x70:
      cpu_write(HL(), B);
      break;

    // LD (HL) <- C
    case 0x71:
      cpu_write(HL(), C);
      break;

    // LD (HL) <- D
    case 0x72:
      cpu_write(HL(), D);
      break;

    // LD (HL) <- E
    case 0x73:
      cpu_write(HL(), E);
      break;

    // LD (HL) <- H
    case 0x74:
      cpu_write(HL(), H);
      break;

    // LD (HL) <- L
    case 0x75:
      cpu_write(HL(), L);
      break;

    // HALT
    case 0x76:
      {
        byte ie_reg = mem_read(INTERRUPT_ENABLE);
        byte if_reg = mem_read(INTERRUPT_FLAGS);
        if (!interrupts && (ie_reg & if_reg & 0x1F) != 0) {
            // HALT bug: IME=0 and pending interrupt — CPU skips HALT but
            // the next instruction's opcode byte is read without PC advancing.
            halt_bug_active = true;
        } else {
            halted = true;
        }
      }
      break;

    // LD (HL) <- A
    case 0x77:
      cpu_write(HL(), A);
      break;

    // LD A <- B
    case 0x78:
      A = B;
      break;

    // LD A <- C
    case 0x79:
      A = C;
      break;

    // LD A <- D
    case 0x7a:
      A = D;
      break;

    // LD A <- E
    case 0x7b:
      A = E;
      break;

    // LD A <- H
    case 0x7c:
      A = H;
      break;

    // LD A <- L
    case 0x7d:
      A = L;
      break;

    // LD A <- (HL)
    case 0x7e:
      A = cpu_read(HL());
      break;

    // LD A <- A
    case 0x7f:
      break;

    // ADD A += B
    case 0x80:
      A = Add(B);
      break;

    // ADD A += C
    case 0x81:
      A = Add(C);
      break;

    // ADD A += D
    case 0x82:
      A = Add(D);
      break;

    // ADD A += E
    case 0x83:
      A = Add(E);
      break;

    // ADD A += H
    case 0x84:
      A = Add(H);
      break;

    // ADD A += L
    case 0x85:
      A = Add(L);
      break;

    // ADD A += (HL)
    case 0x86:
      A = Add(cpu_read(HL()));
      break;

    // ADD A += A
    case 0x87:
      A = Add(A);
      break;

    // ADC A += B
    case 0x88:
      A = Adc(B);
      break;

    // ADC A += C
    case 0x89:
      A = Adc(C);
      break;

    // ADC A += D
    case 0x8a:
      A = Adc(D);
      break;

    // ADC A += E
    case 0x8b:
      A = Adc(E);
      break;

    // ADC A += H
    case 0x8c:
      A = Adc(H);
      break;

    // ADC A += L
    case 0x8d:
      A = Adc(L);
      break;

    // ADC A += (HL)
    case 0x8e:
      A = Adc(cpu_read(HL()));
      break;

    // ADC A += A
    case 0x8f:
      A = Adc(A);
      break;

    // SUB A -= B
    case 0x90:
      A = Sub(B);
      break;

    // SUB A -= C
    case 0x91:
      A = Sub(C);
      break;

    // SUB A -= D
    case 0x92:
      A = Sub(D);
      break;

    // SUB A -= E
    case 0x93:
      A = Sub(E);
      break;

    // SUB A -= H
    case 0x94:
      A = Sub(H);
      break;

    // SUB A -= L
    case 0x95:
      A = Sub(L);
      break;

    // SUB A -= (HL)
    case 0x96:
      A = Sub(cpu_read(HL()));
      break;

    // SUB A -= A
    case 0x97:
      A = Sub(A);
      break;

    // SBC A -= B
    case 0x98:
      A = Sbc(B);
      break;

    // SBC A -= C
    case 0x99:
      A = Sbc(C);
      break;

    // SBC A -= D
    case 0x9a:
      A = Sbc(D);
      break;

    // SBC A -= E
    case 0x9b:
      A = Sbc(E);
      break;

    // SBC A -= H
    case 0x9c:
      A = Sbc(H);
      break;

    // SBC A -= L
    case 0x9d:
      A = Sbc(L);
      break;

    // SBC A -= (HL)
    case 0x9e:
      A = Sbc(cpu_read(HL()));
      break;

    // SBC A -= A
    case 0x9f:
      A = Sbc(A);
      break;

    // AND A & B
    case 0xa0:
      A = And(B);
      break;

    // AND A & C
    case 0xa1:
      A = And(C);
      break;

    // AND A & D
    case 0xa2:
      A = And(D);
      break;

    // AND A & E
    case 0xa3:
      A = And(E);
      break;

    // AND A & H
    case 0xa4:
      A = And(H);
      break;

    // AND A & L
    case 0xa5:
      A = And(L);
      break;

    // AND A & (HL)
    case 0xa6:
      A = And(cpu_read(HL()));
      break;

    // AND A & A
    case 0xa7:
      A = And(A);
      break;

    // XOR A ^ B
    case 0xa8:
      A = Xor(B);
      break;

    // XOR A ^ C
    case 0xa9:
      A = Xor(C);
      break;

    // XOR A ^ D
    case 0xaa:
      A = Xor(D);
      break;

    // XOR A ^ E
    case 0xab:
      A = Xor(E);
      break;

    // XOR A ^ H
    case 0xac:
      A = Xor(H);
      break;

    // XOR A ^ L
    case 0xad:
      A = Xor(L);
      break;

    // XOR A ^ (HL)
    case 0xae:
      A = Xor(cpu_read(HL()));
      break;

    // XOR A ^ A
    case 0xaf:
      A = Xor(A);
      break;

    // OR A | B
    case 0xb0:
      A = Or(B);
      break;

    // OR A | C
    case 0xb1:
      A = Or(C);
      break;

    // OR A | D
    case 0xb2:
      A = Or(D);
      break;

    // OR A | E
    case 0xb3:
      A = Or(E);
      break;

    // OR A | H
    case 0xb4:
      A = Or(H);
      break;

    // OR A | L
    case 0xb5:
      A = Or(L);
      break;

    // OR A | (HL)
    case 0xb6:
      A = Or(cpu_read(HL()));
      break;

    // OR A | A
    case 0xb7:
      A = Or(A);
      break;

    // CP B
    case 0xb8:
      CP(B);
      break;

    // CP C
    case 0xb9:
      CP(C);
      break;

    // CP D
    case 0xba:
      CP(D);
      break;

    // CP E
    case 0xbb:
      CP(E);
      break;

    // CP H
    case 0xbc:
      CP(H);
      break;

    // CP L
    case 0xbd:
      CP(L);
      break;

    // CP (HL)
    case 0xbe:
      CP(cpu_read(HL()));
      break;

    // CP A
    case 0xbf:
      CP(A);
      break;

    // RET NZ
    case 0xc0:
      cycles += 4;
      if (!CheckFlag(FLAG_Z)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // POP BC
    case 0xc1:
      oam_read_corrupt_at(SP, 0);    // M2: POP first read (row 0x30, secondary case)
      oam_read_corrupt_at(SP + 1, 4); // M3: POP second read (row 0x38, standard case)
      setBC(pop_stack());
      cycles += 8;
      break;

    // JP NZ
    case 0xc2:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // JP a16
    case 0xc3:
      PC = cpu_read16();
      cycles += 4;
      break;

    // CALL NZ
    case 0xc4:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_Z)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // PUSH BC
    case 0xc5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      push_stack(BC());
      cycles += 12;
      break;

    // ADD A += d8
    case 0xc6:
      A = Add(cpu_read_next());
      break;

    // RST 00H
    case 0xc7:
      push_stack(PC);
      cycles += 12;
      PC = 0x00;
      break;

    // RET Z
    case 0xc8:
      cycles += 4;
      if (CheckFlag(FLAG_Z)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // RET
    case 0xc9:
      cycles += 12;
      PC = pop_stack();
      break;

    // JP Z
    case 0xca:
      pos = cpu_read16();
      if (CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // CALL Z
    case 0xcc:
      pos = cpu_read16();
      if (CheckFlag(FLAG_Z)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // CALL a16
    case 0xcd:
      pos = cpu_read16();
      push_stack(PC);
      PC = pos;
      cycles += 12;
      break;

    // ADC A += d8
    case 0xce:
      A = Adc(cpu_read_next());
      break;

    // RST 08H
    case 0xcf:
      push_stack(PC);
      cycles += 12;
      PC = 0x08;
      break;

    // RET NC
    case 0xd0:
      cycles += 4;
      if (!CheckFlag(FLAG_C)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // POP DE
    case 0xd1:
      oam_read_corrupt_at(SP, 0);
      oam_read_corrupt_at(SP + 1, 4);
      setDE(pop_stack());
      cycles += 8;
      break;

    // JP NC
    case 0xd2:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_C)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // CALL NC
    case 0xd4:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_C)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // PUSH DE
    case 0xd5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      push_stack(DE());
      cycles += 12;
      break;

    // SUB A -= d8
    case 0xd6:
      A = Sub(cpu_read_next());
      break;

    // RST 10H
    case 0xd7:
      push_stack(PC);
      cycles += 12;
      PC = 0x10;
      break;

    // RET C
    case 0xd8:
      cycles += 4;
      if (CheckFlag(FLAG_C)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // RETI
    case 0xd9:
      cycles += 12;
      PC = pop_stack();
      interrupts = true;
      break;

    // JP C
    case 0xda:
      pos = cpu_read16();
      if (CheckFlag(FLAG_C)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // CALL C
    case 0xdc:
      pos = cpu_read16();
      if (CheckFlag(FLAG_C)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // RST 18H
    case 0xdf:
      push_stack(PC);
      cycles += 12;
      PC = 0x18;
      break;

    // SBC A, d8
    case 0xde:
      A = Sbc(cpu_read_next());
      break;

    // LDH (a8) <- A
    case 0xe0:
      {
          const byte immediate = cpu_read_next();
          const word addr = 0xFF00 | immediate;
          cpu_write(addr, A);
      }
      break;

    // POP HL
    case 0xe1:
      oam_read_corrupt_at(SP, 0);
      oam_read_corrupt_at(SP + 1, 4);
      setHL(pop_stack());
      cycles += 8;
      break;

    // LD (C) <- A
    case 0xe2:
          {
          const word addr = 0xFF00 | C;
          cpu_write(addr, A);
          }
      break;

    // ADD SP, r8
    case 0xe8:
      {
        int8_t val = (int8_t)cpu_read_next();
        int result = SP + val;
        setFlag(FLAG_Z, false);
        setFlag(FLAG_N, false);
        setFlag(FLAG_H, (SP & 0xf) + (val & 0xf) > 0xf);
        setFlag(FLAG_C, (SP & 0xff) + (unsigned)(val & 0xff) > 0xff);
        SP = (word)result;
        cycles += 8;
      }
      break;

    // PUSH HL
    case 0xe5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      push_stack(HL());
      cycles += 12;
      break;

    // AND A & d8
    case 0xe6:
      A = And(cpu_read_next());
      break;

    // RST 20H
    case 0xe7:
      push_stack(PC);
      cycles += 12;
      PC = 0x20;
      break;

    // JP (HL)
    case 0xe9:
      PC = HL();
      break;

    // LD (a16) <- A
    case 0xea:
      cpu_write(cpu_read16(), A);
      break;

    // XOR A ^ d8
    case 0xee:
      A = Xor(cpu_read_next());
      break;

    // RST 28H
    case 0xef:
      push_stack(PC);
      cycles += 12;
      PC = 0x28;
      break;

    // LDH A <- (a8)
    case 0xf0:
       {
           const byte immediate = cpu_read_next();
           const word addr = 0xFF00 | immediate;
            A = cpu_read(addr);
       }
      break;

    // POP AF
    case 0xf1:
      oam_read_corrupt_at(SP, 0);
      oam_read_corrupt_at(SP + 1, 4);
      setAF(pop_stack());
      cycles += 8;
      break;

    // LD A <- (C)
    case 0xf2:
         {
                   const word addr = 0xFF00 | C;
                   A = cpu_read(addr);
         }
      break;

    // DI
    case 0xf3:
      interrupts = false;
      ei_delay = 0;
      break;

    // PUSH AF
    case 0xf5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      push_stack(AF());
      cycles += 12;
      break;

    // OR A | d8
    case 0xf6:
      A = Or(cpu_read_next());
      break;

    // RST 30H
    case 0xf7:
      push_stack(PC);
      cycles += 12;
      PC = 0x30;
      break;

    // LD HL, SP+r8
    case 0xf8:
      {
        int8_t val = (int8_t)cpu_read_next();
        int result = SP + val;
        setFlag(FLAG_Z, false);
        setFlag(FLAG_N, false);
        setFlag(FLAG_H, (SP & 0xf) + (val & 0xf) > 0xf);
        setFlag(FLAG_C, (SP & 0xff) + (unsigned)(val & 0xff) > 0xff);
        setHL((word)result);
        cycles += 4;
      }
      break;

    // LD SP, HL
    case 0xf9:
      SP = HL();
      cycles += 4;
      break;

    // LD A <- (a16)
    case 0xfa:
      A = cpu_read(cpu_read16());
      break;

    // EI
    case 0xfb:
      ei_delay = 2;
      break;

    // CP d8
    case 0xfe:
      CP(cpu_read_next());
      break;

    // RST 38H
    case 0xff:
      push_stack(PC);
      cycles += 12;
      PC = 0x38;
      break;

// END GENERATED
        default:
            printf("Unimplemented opcode %x at %x\n", opcode, PC - 1);
            break;
    }
}

void exec_ext_op(byte opcode){
    switch (opcode){
// START EX GENERATED

    // RLC B
    case 0x00: B = Rlc(B); break;
    // RLC C
    case 0x01: C = Rlc(C); break;
    // RLC D
    case 0x02: D = Rlc(D); break;
    // RLC E
    case 0x03: E = Rlc(E); break;
    // RLC H
    case 0x04: H = Rlc(H); break;
    // RLC L
    case 0x05: L = Rlc(L); break;
    // RLC (HL)
    case 0x06: cpu_write(HL(), Rlc(cpu_read(HL()))); break;
    // RLC A
    case 0x07: A = Rlc(A); break;

    // RRC B
    case 0x08: B = Rrc(B); break;
    // RRC C
    case 0x09: C = Rrc(C); break;
    // RRC D
    case 0x0a: D = Rrc(D); break;
    // RRC E
    case 0x0b: E = Rrc(E); break;
    // RRC H
    case 0x0c: H = Rrc(H); break;
    // RRC L
    case 0x0d: L = Rrc(L); break;
    // RRC (HL)
    case 0x0e: cpu_write(HL(), Rrc(cpu_read(HL()))); break;
    // RRC A
    case 0x0f: A = Rrc(A); break;

    // RL B
    case 0x10:
      B = RL(B);
      break;

    // RL C
    case 0x11:
      C = RL(C);
      break;

    // RL D
    case 0x12:
      D = RL(D);
      break;

    // RL E
    case 0x13:
      E = RL(E);
      break;

    // RL H
    case 0x14:
      H = RL(H);
      break;

    // RL L
    case 0x15:
      L = RL(L);
      break;

    // RL (HL)
    case 0x16:
      cpu_write(HL(), RL(cpu_read(HL())));
      break;

    // RL A
    case 0x17:
      A = RL(A);
      break;

    // RR B
    case 0x18:
      B = Rr(B, false);
      break;

    // RR C
    case 0x19:
      C = Rr(C, false);
      break;

    // RR D
    case 0x1a:
      D = Rr(D, false);
      break;

    // RR E
    case 0x1b:
      E = Rr(E, false);
      break;

    // RR H
    case 0x1c:
      H = Rr(H, false);
      break;

    // RR L
    case 0x1d:
      L = Rr(L, false);
      break;

    // RR (HL)
    case 0x1e:
      cpu_write(HL(), Rr(cpu_read(HL()), false));
      break;

    // RR A
    case 0x1f:
      A = Rr(A, true);
      break;

    // SLA B
    case 0x20:
      B = Sla(B);
      break;

    // SLA C
    case 0x21:
      C = Sla(C);
      break;

    // SLA D
    case 0x22:
      D = Sla(D);
      break;

    // SLA E
    case 0x23:
      E = Sla(E);
      break;

    // SLA H
    case 0x24:
      H = Sla(H);
      break;

    // SLA L
    case 0x25:
      L = Sla(L);
      break;

    // SLA (HL)
    case 0x26:
      cpu_write(HL(), Sla(cpu_read(HL())));
      break;

    // SLA A
    case 0x27:
      A = Sla(A);
      break;

    // SRA B
    case 0x28: B = Sra(B); break;
    // SRA C
    case 0x29: C = Sra(C); break;
    // SRA D
    case 0x2a: D = Sra(D); break;
    // SRA E
    case 0x2b: E = Sra(E); break;
    // SRA H
    case 0x2c: H = Sra(H); break;
    // SRA L
    case 0x2d: L = Sra(L); break;
    // SRA (HL)
    case 0x2e: cpu_write(HL(), Sra(cpu_read(HL()))); break;
    // SRA A
    case 0x2f: A = Sra(A); break;

    // SWAP B
    case 0x30:
      B = Swap(B);
      break;

    // SWAP C
    case 0x31:
      C = Swap(C);
      break;

    // SWAP D
    case 0x32:
      D = Swap(D);
      break;

    // SWAP E
    case 0x33:
      E = Swap(E);
      break;

    // SWAP H
    case 0x34:
      H = Swap(H);
      break;

    // SWAP L
    case 0x35:
      L = Swap(L);
      break;

    // SWAP (HL)
    case 0x36:
      cpu_write(HL(), Swap(cpu_read(HL())));
      break;

    // SWAP A
    case 0x37:
      A = Swap(A);
      break;

    // SRL B
    case 0x38:
      B = Srl(B);
      break;

    // SRL C
    case 0x39:
      C = Srl(C);
      break;

    // SRL D
    case 0x3a:
      D = Srl(D);
      break;

    // SRL E
    case 0x3b:
      E = Srl(E);
      break;

    // SRL H
    case 0x3c:
      H = Srl(H);
      break;

    // SRL L
    case 0x3d:
      L = Srl(L);
      break;

    // SRL (HL)
    case 0x3e:
      cpu_write(HL(), Srl(cpu_read(HL())));
      break;

    // SRL A
    case 0x3f:
      A = Srl(A);
      break;

    // BIT 0 of B
    case 0x40:
      Bit(B, 0);
      break;

    // BIT 0 of C
    case 0x41:
      Bit(C, 0);
      break;

    // BIT 0 of D
    case 0x42:
      Bit(D, 0);
      break;

    // BIT 0 of E
    case 0x43:
      Bit(E, 0);
      break;

    // BIT 0 of H
    case 0x44:
      Bit(H, 0);
      break;

    // BIT 0 of L
    case 0x45:
      Bit(L, 0);
      break;

    // BIT 0 of (HL)
    case 0x46:
      Bit(cpu_read(HL()), 0);
      break;

    // BIT 0 of A
    case 0x47:
      Bit(A, 0);
      break;

    // BIT 1 of B
    case 0x48:
      Bit(B, 1);
      break;

    // BIT 1 of C
    case 0x49:
      Bit(C, 1);
      break;

    // BIT 1 of D
    case 0x4a:
      Bit(D, 1);
      break;

    // BIT 1 of E
    case 0x4b:
      Bit(E, 1);
      break;

    // BIT 1 of H
    case 0x4c:
      Bit(H, 1);
      break;

    // BIT 1 of L
    case 0x4d:
      Bit(L, 1);
      break;

    // BIT 1 of (HL)
    case 0x4e:
      Bit(cpu_read(HL()), 1);
      break;

    // BIT 1 of A
    case 0x4f:
      Bit(A, 1);
      break;

    // BIT 2 of B
    case 0x50:
      Bit(B, 2);
      break;

    // BIT 2 of C
    case 0x51:
      Bit(C, 2);
      break;

    // BIT 2 of D
    case 0x52:
      Bit(D, 2);
      break;

    // BIT 2 of E
    case 0x53:
      Bit(E, 2);
      break;

    // BIT 2 of H
    case 0x54:
      Bit(H, 2);
      break;

    // BIT 2 of L
    case 0x55:
      Bit(L, 2);
      break;

    // BIT 2 of (HL)
    case 0x56:
      Bit(cpu_read(HL()), 2);
      break;

    // BIT 2 of A
    case 0x57:
      Bit(A, 2);
      break;

    // BIT 3 of B
    case 0x58:
      Bit(B, 3);
      break;

    // BIT 3 of C
    case 0x59:
      Bit(C, 3);
      break;

    // BIT 3 of D
    case 0x5a:
      Bit(D, 3);
      break;

    // BIT 3 of E
    case 0x5b:
      Bit(E, 3);
      break;

    // BIT 3 of H
    case 0x5c:
      Bit(H, 3);
      break;

    // BIT 3 of L
    case 0x5d:
      Bit(L, 3);
      break;

    // BIT 3 of (HL)
    case 0x5e:
      Bit(cpu_read(HL()), 3);
      break;

    // BIT 3 of A
    case 0x5f:
      Bit(A, 3);
      break;

    // BIT 4 of B
    case 0x60:
      Bit(B, 4);
      break;

    // BIT 4 of C
    case 0x61:
      Bit(C, 4);
      break;

    // BIT 4 of D
    case 0x62:
      Bit(D, 4);
      break;

    // BIT 4 of E
    case 0x63:
      Bit(E, 4);
      break;

    // BIT 4 of H
    case 0x64:
      Bit(H, 4);
      break;

    // BIT 4 of L
    case 0x65:
      Bit(L, 4);
      break;

    // BIT 4 of (HL)
    case 0x66:
      Bit(cpu_read(HL()), 4);
      break;

    // BIT 4 of A
    case 0x67:
      Bit(A, 4);
      break;

    // BIT 5 of B
    case 0x68:
      Bit(B, 5);
      break;

    // BIT 5 of C
    case 0x69:
      Bit(C, 5);
      break;

    // BIT 5 of D
    case 0x6a:
      Bit(D, 5);
      break;

    // BIT 5 of E
    case 0x6b:
      Bit(E, 5);
      break;

    // BIT 5 of H
    case 0x6c:
      Bit(H, 5);
      break;

    // BIT 5 of L
    case 0x6d:
      Bit(L, 5);
      break;

    // BIT 5 of (HL)
    case 0x6e:
      Bit(cpu_read(HL()), 5);
      break;

    // BIT 5 of A
    case 0x6f:
      Bit(A, 5);
      break;

    // BIT 6 of B
    case 0x70:
      Bit(B, 6);
      break;

    // BIT 6 of C
    case 0x71:
      Bit(C, 6);
      break;

    // BIT 6 of D
    case 0x72:
      Bit(D, 6);
      break;

    // BIT 6 of E
    case 0x73:
      Bit(E, 6);
      break;

    // BIT 6 of H
    case 0x74:
      Bit(H, 6);
      break;

    // BIT 6 of L
    case 0x75:
      Bit(L, 6);
      break;

    // BIT 6 of (HL)
    case 0x76:
      Bit(cpu_read(HL()), 6);
      break;

    // BIT 6 of A
    case 0x77:
      Bit(A, 6);
      break;

    // BIT 7 of B
    case 0x78:
      Bit(B, 7);
      break;

    // BIT 7 of C
    case 0x79:
      Bit(C, 7);
      break;

    // BIT 7 of D
    case 0x7a:
      Bit(D, 7);
      break;

    // BIT 7 of E
    case 0x7b:
      Bit(E, 7);
      break;

    // BIT 7 of H
    case 0x7c:
      Bit(H, 7);
      break;

    // BIT 7 of L
    case 0x7d:
      Bit(L, 7);
      break;

    // BIT 7 of (HL)
    case 0x7e:
      Bit(cpu_read(HL()), 7);
      break;

    // BIT 7 of A
    case 0x7f:
      Bit(A, 7);
      break;

    // RES 0 of B
    case 0x80:
      B = bit_clear(B, 0);
      break;

    // RES 0 of C
    case 0x81:
      C = bit_clear(C, 0);
      break;

    // RES 0 of D
    case 0x82:
      D = bit_clear(D, 0);
      break;

    // RES 0 of E
    case 0x83:
      E = bit_clear(E, 0);
      break;

    // RES 0 of H
    case 0x84:
      H = bit_clear(H, 0);
      break;

    // RES 0 of L
    case 0x85:
      L = bit_clear(L, 0);
      break;

    // RES 0 of (HL)
    case 0x86:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 0));
      break;

    // RES 0 of A
    case 0x87:
      A = bit_clear(A, 0);
      break;

    // RES 1 of B
    case 0x88:
      B = bit_clear(B, 1);
      break;

    // RES 1 of C
    case 0x89:
      C = bit_clear(C, 1);
      break;

    // RES 1 of D
    case 0x8a:
      D = bit_clear(D, 1);
      break;

    // RES 1 of E
    case 0x8b:
      E = bit_clear(E, 1);
      break;

    // RES 1 of H
    case 0x8c:
      H = bit_clear(H, 1);
      break;

    // RES 1 of L
    case 0x8d:
      L = bit_clear(L, 1);
      break;

    // RES 1 of (HL)
    case 0x8e:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 1));
      break;

    // RES 1 of A
    case 0x8f:
      A = bit_clear(A, 1);
      break;

    // RES 2 of B
    case 0x90:
      B = bit_clear(B, 2);
      break;

    // RES 2 of C
    case 0x91:
      C = bit_clear(C, 2);
      break;

    // RES 2 of D
    case 0x92:
      D = bit_clear(D, 2);
      break;

    // RES 2 of E
    case 0x93:
      E = bit_clear(E, 2);
      break;

    // RES 2 of H
    case 0x94:
      H = bit_clear(H, 2);
      break;

    // RES 2 of L
    case 0x95:
      L = bit_clear(L, 2);
      break;

    // RES 2 of (HL)
    case 0x96:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 2));
      break;

    // RES 2 of A
    case 0x97:
      A = bit_clear(A, 2);
      break;

    // RES 3 of B
    case 0x98:
      B = bit_clear(B, 3);
      break;

    // RES 3 of C
    case 0x99:
      C = bit_clear(C, 3);
      break;

    // RES 3 of D
    case 0x9a:
      D = bit_clear(D, 3);
      break;

    // RES 3 of E
    case 0x9b:
      E = bit_clear(E, 3);
      break;

    // RES 3 of H
    case 0x9c:
      H = bit_clear(H, 3);
      break;

    // RES 3 of L
    case 0x9d:
      L = bit_clear(L, 3);
      break;

    // RES 3 of (HL)
    case 0x9e:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 3));
      break;

    // RES 3 of A
    case 0x9f:
      A = bit_clear(A, 3);
      break;

    // RES 4 of B
    case 0xa0:
      B = bit_clear(B, 4);
      break;

    // RES 4 of C
    case 0xa1:
      C = bit_clear(C, 4);
      break;

    // RES 4 of D
    case 0xa2:
      D = bit_clear(D, 4);
      break;

    // RES 4 of E
    case 0xa3:
      E = bit_clear(E, 4);
      break;

    // RES 4 of H
    case 0xa4:
      H = bit_clear(H, 4);
      break;

    // RES 4 of L
    case 0xa5:
      L = bit_clear(L, 4);
      break;

    // RES 4 of (HL)
    case 0xa6:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 4));
      break;

    // RES 4 of A
    case 0xa7:
      A = bit_clear(A, 4);
      break;

    // RES 5 of B
    case 0xa8:
      B = bit_clear(B, 5);
      break;

    // RES 5 of C
    case 0xa9:
      C = bit_clear(C, 5);
      break;

    // RES 5 of D
    case 0xaa:
      D = bit_clear(D, 5);
      break;

    // RES 5 of E
    case 0xab:
      E = bit_clear(E, 5);
      break;

    // RES 5 of H
    case 0xac:
      H = bit_clear(H, 5);
      break;

    // RES 5 of L
    case 0xad:
      L = bit_clear(L, 5);
      break;

    // RES 5 of (HL)
    case 0xae:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 5));
      break;

    // RES 5 of A
    case 0xaf:
      A = bit_clear(A, 5);
      break;

    // RES 6 of B
    case 0xb0:
      B = bit_clear(B, 6);
      break;

    // RES 6 of C
    case 0xb1:
      C = bit_clear(C, 6);
      break;

    // RES 6 of D
    case 0xb2:
      D = bit_clear(D, 6);
      break;

    // RES 6 of E
    case 0xb3:
      E = bit_clear(E, 6);
      break;

    // RES 6 of H
    case 0xb4:
      H = bit_clear(H, 6);
      break;

    // RES 6 of L
    case 0xb5:
      L = bit_clear(L, 6);
      break;

    // RES 6 of (HL)
    case 0xb6:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 6));
      break;

    // RES 6 of A
    case 0xb7:
      A = bit_clear(A, 6);
      break;

    // RES 7 of B
    case 0xb8:
      B = bit_clear(B, 7);
      break;

    // RES 7 of C
    case 0xb9:
      C = bit_clear(C, 7);
      break;

    // RES 7 of D
    case 0xba:
      D = bit_clear(D, 7);
      break;

    // RES 7 of E
    case 0xbb:
      E = bit_clear(E, 7);
      break;

    // RES 7 of H
    case 0xbc:
      H = bit_clear(H, 7);
      break;

    // RES 7 of L
    case 0xbd:
      L = bit_clear(L, 7);
      break;

    // RES 7 of (HL)
    case 0xbe:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 7));
      break;

    // RES 7 of A
    case 0xbf:
      A = bit_clear(A, 7);
      break;

    // SET 0 of B
    case 0xc0:
      B = bit_set(B, 0);
      break;

    // SET 0 of C
    case 0xc1:
      C = bit_set(C, 0);
      break;

    // SET 0 of D
    case 0xc2:
      D = bit_set(D, 0);
      break;

    // SET 0 of E
    case 0xc3:
      E = bit_set(E, 0);
      break;

    // SET 0 of H
    case 0xc4:
      H = bit_set(H, 0);
      break;

    // SET 0 of L
    case 0xc5:
      L = bit_set(L, 0);
      break;

    // SET 0 of (HL)
    case 0xc6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 0));
      break;

    // SET 0 of A
    case 0xc7:
      A = bit_set(A, 0);
      break;

    // SET 1 of B
    case 0xc8:
      B = bit_set(B, 1);
      break;

    // SET 1 of C
    case 0xc9:
      C = bit_set(C, 1);
      break;

    // SET 1 of D
    case 0xca:
      D = bit_set(D, 1);
      break;

    // SET 1 of E
    case 0xcb:
      E = bit_set(E, 1);
      break;

    // SET 1 of H
    case 0xcc:
      H = bit_set(H, 1);
      break;

    // SET 1 of L
    case 0xcd:
      L = bit_set(L, 1);
      break;

    // SET 1 of (HL)
    case 0xce:
      cpu_write(HL(), bit_set(cpu_read(HL()), 1));
      break;

    // SET 1 of A
    case 0xcf:
      A = bit_set(A, 1);
      break;

    // SET 2 of B
    case 0xd0:
      B = bit_set(B, 2);
      break;

    // SET 2 of C
    case 0xd1:
      C = bit_set(C, 2);
      break;

    // SET 2 of D
    case 0xd2:
      D = bit_set(D, 2);
      break;

    // SET 2 of E
    case 0xd3:
      E = bit_set(E, 2);
      break;

    // SET 2 of H
    case 0xd4:
      H = bit_set(H, 2);
      break;

    // SET 2 of L
    case 0xd5:
      L = bit_set(L, 2);
      break;

    // SET 2 of (HL)
    case 0xd6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 2));
      break;

    // SET 2 of A
    case 0xd7:
      A = bit_set(A, 2);
      break;

    // SET 3 of B
    case 0xd8:
      B = bit_set(B, 3);
      break;

    // SET 3 of C
    case 0xd9:
      C = bit_set(C, 3);
      break;

    // SET 3 of D
    case 0xda:
      D = bit_set(D, 3);
      break;

    // SET 3 of E
    case 0xdb:
      E = bit_set(E, 3);
      break;

    // SET 3 of H
    case 0xdc:
      H = bit_set(H, 3);
      break;

    // SET 3 of L
    case 0xdd:
      L = bit_set(L, 3);
      break;

    // SET 3 of (HL)
    case 0xde:
      cpu_write(HL(), bit_set(cpu_read(HL()), 3));
      break;

    // SET 3 of A
    case 0xdf:
      A = bit_set(A, 3);
      break;

    // SET 4 of B
    case 0xe0:
      B = bit_set(B, 4);
      break;

    // SET 4 of C
    case 0xe1:
      C = bit_set(C, 4);
      break;

    // SET 4 of D
    case 0xe2:
      D = bit_set(D, 4);
      break;

    // SET 4 of E
    case 0xe3:
      E = bit_set(E, 4);
      break;

    // SET 4 of H
    case 0xe4:
      H = bit_set(H, 4);
      break;

    // SET 4 of L
    case 0xe5:
      L = bit_set(L, 4);
      break;

    // SET 4 of (HL)
    case 0xe6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 4));
      break;

    // SET 4 of A
    case 0xe7:
      A = bit_set(A, 4);
      break;

    // SET 5 of B
    case 0xe8:
      B = bit_set(B, 5);
      break;

    // SET 5 of C
    case 0xe9:
      C = bit_set(C, 5);
      break;

    // SET 5 of D
    case 0xea:
      D = bit_set(D, 5);
      break;

    // SET 5 of E
    case 0xeb:
      E = bit_set(E, 5);
      break;

    // SET 5 of H
    case 0xec:
      H = bit_set(H, 5);
      break;

    // SET 5 of L
    case 0xed:
      L = bit_set(L, 5);
      break;

    // SET 5 of (HL)
    case 0xee:
      cpu_write(HL(), bit_set(cpu_read(HL()), 5));
      break;

    // SET 5 of A
    case 0xef:
      A = bit_set(A, 5);
      break;

    // SET 6 of B
    case 0xf0:
      B = bit_set(B, 6);
      break;

    // SET 6 of C
    case 0xf1:
      C = bit_set(C, 6);
      break;

    // SET 6 of D
    case 0xf2:
      D = bit_set(D, 6);
      break;

    // SET 6 of E
    case 0xf3:
      E = bit_set(E, 6);
      break;

    // SET 6 of H
    case 0xf4:
      H = bit_set(H, 6);
      break;

    // SET 6 of L
    case 0xf5:
      L = bit_set(L, 6);
      break;

    // SET 6 of (HL)
    case 0xf6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 6));
      break;

    // SET 6 of A
    case 0xf7:
      A = bit_set(A, 6);
      break;

    // SET 7 of B
    case 0xf8:
      B = bit_set(B, 7);
      break;

    // SET 7 of C
    case 0xf9:
      C = bit_set(C, 7);
      break;

    // SET 7 of D
    case 0xfa:
      D = bit_set(D, 7);
      break;

    // SET 7 of E
    case 0xfb:
      E = bit_set(E, 7);
      break;

    // SET 7 of H
    case 0xfc:
      H = bit_set(H, 7);
      break;

    // SET 7 of L
    case 0xfd:
      L = bit_set(L, 7);
      break;

    // SET 7 of (HL)
    case 0xfe:
      cpu_write(HL(), bit_set(cpu_read(HL()), 7));
      break;

    // SET 7 of A
    case 0xff:
      A = bit_set(A, 7);
      break;

// END EX GENERATED

        default:
            printf("Unimplemented EX opcode %x at %x\n", opcode, PC - 2);
            exit(1);
            break;
    }
}

bool debug = false;

void exec_next(void){
    if (halted) {
        cycles += 4;
        return;
    }
    if (debug_logfile != NULL) {
        cpu_debug_log();
    }
    if(PC == 0x02dd) {
      //  debug = true;
    }
    if (debug){
        dump_regs();
    }
    // HALT bug: opcode is fetched without PC advancing (same byte read twice)
    if (halt_bug_active) {
        halt_bug_active = false;
        byte op = cpu_read(PC); // read without incrementing PC
        exec_op(op);
    } else {
        byte op = cpu_read_next();
        if(debug) {
            printf("executing %x (%s) at $%x\n", op, opnames[op], PC-1);
        }
        exec_op(op);
    }
    if (ei_delay > 0) {
        ei_delay--;
        if (ei_delay == 0) {
            interrupts = true;
        }
    }
}

//
// SDL boilerplate
//

SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* texture;

void pixels_init(void){
    pixels = malloc(sizeof(uint32_t) * VIEWPORT_WIDTH * VIEWPORT_HEIGHT);
    if (pixels == NULL) {
          printf("Could not malloc pixels");
          exit(1);
    }

    memset(pixels, 0, sizeof(uint32_t) * VIEWPORT_WIDTH * VIEWPORT_HEIGHT);
}

void sdl_init(void){
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        exit(1);
    }
    
    window = SDL_CreateWindow("Joe's GB", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VIEWPORT_WIDTH*2, VIEWPORT_HEIGHT*2, SDL_WINDOW_SHOWN);

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

void sdl_display(void){
    // Diagnostic output removed
    
    int result = SDL_UpdateTexture(texture, NULL, pixels, VIEWPORT_WIDTH*sizeof(uint32_t));

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


bool frame_headless(void){
    static int frame_count = 0;
    frame_count++;

    if (bailAfterBios && !inBios) {

    } else {
        cycles = 0;
        int instruction_count = 0;
        while (cycles < 69905) {
            if (bailAfterBios && !inBios) {
                printf("bailing after bios\n");
                break;
        //        return false;
            }
            instr_timer_cycles = 0;
            int prevcycles = cycles;
            do_interrupts();
            int dispatch_cycles = cycles - prevcycles;
            if (dispatch_cycles > 0) {
                gpu_step(dispatch_cycles);
                timer_step(dispatch_cycles - instr_timer_cycles);
                instr_timer_cycles = 0;
            }
            exec_next_start_cycles = cycles;
            exec_next();
            int cpu_cycles = cycles - prevcycles - dispatch_cycles;

            gpu_step(cpu_cycles);
            timer_step(cpu_cycles - instr_timer_cycles);
            instruction_count++;

            // Safety check for infinite loops
            if (instruction_count > 100000) {
                printf("WARNING: Infinite loop detected at frame %d, PC=0x%04X\n", frame_count, PC);
                break;
            }
        }

        // Diagnostic output removed
    }

    return true;
}

bool frame(void){
    const uint32_t start_ticks = SDL_GetTicks();

    frame_headless();
    sdl_display();

    const uint32_t diff = SDL_GetTicks() - start_ticks;

    // More accurate frame timing - target 16.67ms per frame (60 FPS)
    if (diff < 16) {
        const uint32_t nap_time = 16 - diff;
        SDL_Delay(nap_time);
    }

    return true;
}

void sdl_main_impl(void){
  bool run = true;
  bool *joypad_key;
  bool joypad_last;

  // SDL main loop starting
  SDL_Event event;
  printf("SDL window created, starting main loop...\n");
  while(run) {
      while (SDL_PollEvent(&event)) {
          switch (event.type) {
              case SDL_QUIT:
                  printf("got quit event - window closed\n");
                  run = false;
                  return;
              case SDL_KEYDOWN:
              case SDL_KEYUP:
                  joypad_key = NULL;
                  switch (event.key.keysym.sym) {
                      case SDLK_RIGHT: // RIGHT
                        joypad_key = &joypad_buttons.RIGHT;
                      break;
                      case SDLK_LEFT: // LEFT
                        joypad_key = &joypad_buttons.LEFT;
                      break;
                      case SDLK_UP: // UP
                        joypad_key = &joypad_buttons.UP;
                      break;
                      case SDLK_DOWN: // DOWN
                        joypad_key = &joypad_buttons.DOWN;
                      break;
                      case SDLK_a: // A
                        joypad_key = &joypad_buttons.A;
                      break;
                      case SDLK_s: // B
                        joypad_key = &joypad_buttons.B;
                      break;
                      case SDLK_RSHIFT: // SELECT
                        joypad_key = &joypad_buttons.SELECT;
                      break;
                      case SDLK_RETURN: // START
                        joypad_key = &joypad_buttons.START;
                      break;
                  }
                  if (joypad_key != NULL) {
                      joypad_last = *joypad_key;
                      *joypad_key = event.key.type == SDL_KEYDOWN;
                      if (joypad_last != *joypad_key) {
                          request_interrupt(INTERRUPT_JOYPAD);
                      }
                  }
                  break;
          }
      }

      static int frame_count = 0;
      frame_count++;
      if (frame_count <= 5) {
          printf("Processing frame %d...\n", frame_count);
      }
      run = frame();
  }

}

void headless_print_blargg_a000(void) {
    // Print blargg A000-format test output to stdout.
    // Format: A000=status, A001-A003=magic bytes DE B0 61, A004+=text
    if (ext_ram[1] == 0xDE && ext_ram[2] == 0xB0 && ext_ram[3] == 0x61) {
        for (int i = 4; i < 0x2000; i++) {
            if (ext_ram[i] == 0) break;
            fputc(ext_ram[i], stdout);
        }
        fflush(stdout);
    }
}

void headless_main_impl(void) {
    long long total_cycles = 0;
    while (1) {
        if (max_cycles > 0 && total_cycles >= max_cycles) {
            printf("\n[headless] cycle limit %lld reached, stopping\n", max_cycles);
            break;
        }
        if (blargg_done) {
            headless_print_blargg_a000();
            break;
        }
        instr_timer_cycles = 0;
        int prevcycles = cycles;
        do_interrupts();
        int dispatch_cycles = cycles - prevcycles;
        if (dispatch_cycles > 0) {
            gpu_step(dispatch_cycles);
            timer_step(dispatch_cycles - instr_timer_cycles);
            instr_timer_cycles = 0;
        }
        exec_next_start_cycles = cycles;
        exec_next();
        int cpu_cycles = cycles - prevcycles - dispatch_cycles;
        gpu_step(cpu_cycles);
        timer_step(cpu_cycles - instr_timer_cycles);
        total_cycles += cpu_cycles;
    }
    // If cycle limit hit but A000 output present, still print it
    if (!blargg_done) {
        headless_print_blargg_a000();
    }
}

int main(int argc, char **argv){

  char *rom = "tetris.gb";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--headless") == 0) {
      headless = true;
    } else if (strcmp(argv[i], "--cycles") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --cycles\n");
        return 1;
      }
      max_cycles = strtoll(argv[++i], NULL, 10);
    } else if (strncmp(argv[i], "--", 2) == 0) {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    } else {
      rom = argv[i];
    }
  }
// cpu_init_debug_file(); // Disabled to prevent interference

  cart_load(rom);

  mem_init();
  //cpu_init();
  cpu_fake_init();
  gpu_init();
  pixels_init();
  if (!headless) {
    sdl_init();
  }
  joypad_init();

  if (headless) {
    headless_main_impl();
  } else {
    sdl_main_impl();
  }

cpu_close_debug_file();

  return 0;
}
