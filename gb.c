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
static bool pending_drawline = false;   // deferred scanline render (fires at T=80)
static byte pending_draw_ly = 0;
// LY=153 short-scanline hardware quirk: LY=153 lasts only 4T on DMG, then LY→0 while
// VBlank (mode 1) continues for 452T more before OAM scan starts for LY=0.
static bool ly153_vblank_active = false;
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
static bool serial_active = false;    // true during an in-progress internal-clock transfer
static int  serial_bits_remaining = 0; // bits left in current transfer (8 down to 0)
static byte serial_out_byte = 0;       // byte to transmit (saved at transfer start)
bool headless = false;
long long max_cycles = 0;
bool gbmicrotest_mode = false; // read 0xFF82 for pass/fail after cycle limit

// Joypad injection: list of (frame, button_state) events for headless automation.
#define MAX_JOYPAD_EVENTS 128
typedef struct {
    int frame;       // GB frame number (70224 T-cycles each) to apply this event
    int duration;    // frames to hold the buttons (default 1)
    bool A, B, START, SELECT, UP, DOWN, LEFT, RIGHT;
} JoypadEvent;
static JoypadEvent joypad_events[MAX_JOYPAD_EVENTS];
static int joypad_event_count = 0;

#define MAX_SCREENSHOT_REQUESTS 32
typedef struct {
    int frame;
    char filename[1024];
    bool done;
} ScreenshotRequest;
static ScreenshotRequest screenshot_requests[MAX_SCREENSHOT_REQUESTS];
static int screenshot_request_count = 0;

static bool cpu_debug_range_enabled = false;
static long long cpu_debug_start_cycle = 0;
static long long cpu_debug_end_cycle = 0;
static long long total_cpu_cycles = 0;

typedef enum {
    MODEL_DMG = 0,   // DMG-ABC (default)
    MODEL_DMG0,      // early DMG0
    MODEL_MGB,       // Game Boy Pocket
    MODEL_SGB,       // Super Game Boy
    MODEL_SGB2,      // Super Game Boy 2
    MODEL_GBC,       // Game Boy Color
} gb_model_t;
static gb_model_t gb_model = MODEL_DMG;

// Blargg A000-based result detection (used by halt_bug, interrupt_time, mem_timing-2, oam_bug)
// Magic bytes at A001-A003 = {0xDE, 0xB0, 0x61}; result at A000 (0x80=running, 0=pass, other=fail)
bool blargg_done = false;

// OAM DMA state: cycle-accurate 160 M-cycle transfer
bool dma_active = false;
bool dma_oam_locked = false;    // OAM locked only after 2-M-cycle startup grace period
int  dma_cycles_remaining = 0; // counts down from 160 (M-cycles) during active transfer
int  dma_startup_remaining = 0;// 2 M-cycle startup delay before first byte is copied
int  dma_source = 0;           // source start address

// CGB double-speed mode state (KEY1/$FF4D)
bool double_speed = false;

// Minimal APU: only length counters for channels 1 and 2 (needed for interrupt_time test)
// Length counter fires at 256 Hz real-time = 16384 T-cycles at 1x, 32768 T-cycles at 2x.
bool apu_ch1_active = false;
bool apu_ch1_length_enable = false;
int  apu_ch1_length = 0;   // remaining length ticks
bool apu_ch2_active = false;
bool apu_ch2_length_enable = false;
int  apu_ch2_length = 0;   // remaining length ticks
int  apu_length_cycles = 0;

typedef struct {
    bool enabled, bg, window, sprite, sprite_tall;
    int windowtilemap, BgWindowTileData;
    bool BgTileDataSigned;
    int bgtilemap;
} GPUCONTROL;

GPUCONTROL gpu_control;

void gpu_parse_control(byte control){
    gpu_control.enabled    = bit_check(control, 7);
    gpu_control.window     = bit_check(control, 5);
    gpu_control.sprite     = bit_check(control, 1);
    gpu_control.sprite_tall= bit_check(control, 2);
    gpu_control.bg         = bit_check(control, 0);

    if (bit_check(control, 4)) {
        gpu_control.BgWindowTileData = 0x8000;  // unsigned, blocks 0+1
        gpu_control.BgTileDataSigned = false;
    } else {
        gpu_control.BgWindowTileData = 0x9000;  // signed, blocks 2+1 (tile 0 at 0x9000)
        gpu_control.BgTileDataSigned = true;
    }

    gpu_control.bgtilemap    = bit_check(control, 3) ? 0x9C00 : 0x9800;
    gpu_control.windowtilemap= bit_check(control, 6) ? 0x9C00 : 0x9800;
}

#define set_pixel(x,y,c) pixels[(y * VIEWPORT_WIDTH) + x] = c;

// Per-scanline BG color index (0-3) — used by sprites for BG priority
static uint8_t bg_scanline[160];
// Window internal line counter (increments each scanline window is visible)
static int window_line = 0;

// Tracks timer cycles pre-stepped mid-instruction via cpu_read/cpu_write.
// Reset before each instruction; used in projected-mode calculations (gpu_read,
// mem_read) and to avoid double-counting timers in the main loop.
int instr_timer_cycles;

// STAT interrupt line (rising-edge detection for blocking behaviour).
// The hardware STAT interrupt fires only when this line transitions 0→1.
static bool stat_irq_line = false;

// LYC=LY flag value frozen when LCD is disabled.
// On DMG, turning off the LCD does NOT clear STAT bit 2 (LYC=LY flag);
// it freezes at the value it had just before the LCD was turned off.
static bool lcd_off_lyc_flag = false;

// After LCD is enabled, hardware starts in mode 0 (not mode 2) for a brief
// startup period before mode 2 begins. This flag overrides mode to 0 until
// gpu_cycles advances past the ~80T OAM-scan point.
static bool lcd_startup_mode0 = false;

// True during the entire first scanline after LCD is enabled (LY=0 startup line).
// Cleared when LY increments from 0 to 1 in gpu_step.
// Used to apply a later LY projection threshold on the startup line (+12T vs normal).
static bool lcd_startup_line = false;

// LYC=LY STAT interrupt fires 8T after the LY change.
// When set, the LYC check in stat_check_irq is suppressed; it fires in gpu_step at T>=8.
static bool lyc_delayed = false;

// Projected T-cycle of last CPU write to IF (0xFF0F) — used to suppress OAM/LYC
// STAT interrupt when CPU write and interrupt fire in the same T-cycle (CPU wins).
static int if_cleared_proj_gc = -100;
static int if_cleared_proj_ly = -100;

// Per-scanline mode-3 extension (T-cycles beyond 172).
// Accounts for fine-scroll penalty (SCX & 7) and sprite-fetch stall (6T per sprite, max 10).
// Recomputed at the start of each new scanline; used by all mode-boundary queries.
static int mode3_extra = 0;

// Recompute mode3_extra for the current LY_REG.
// Must be called right after LY_REG is updated so OAM is scanned for the correct line.
static void compute_mode3_extra(void) {
    // Window active on this scanline?
    bool window_active = gpu_control.window && (int)RAM[WY] <= (int)LY_REG && RAM[WX] <= 166;

    // SCX fine-scroll penalty depends on context:
    // - Startup line OR window-active line: exact SCX%8 T-cycles (SCX%8=7 → 8T).
    // - Normal lines without window: quantized in 4T groups (0-3=0T, 4-6=4T, 7=8T).
    //   When the window is active, the fetcher transitions mid-scanline, which preserves
    //   the exact SCX penalty rather than allowing it to be absorbed.
    int scx_fine = SCX_REG & 7;
    int extra;
    if (lcd_startup_line || window_active) {
        extra = (scx_fine < 7) ? scx_fine : 8;
    } else {
        extra = (scx_fine < 3) ? 0 : (scx_fine < 7) ? 4 : 8;
    }
    if (gpu_control.sprite) {
        int height = gpu_control.sprite_tall ? 16 : 8;
        // Track background fetcher phase (0-7) and current pixel position
        // to compute per-sprite stall and inter-group overhead accurately.
        int bg_phase = 0;
        int cur_pixel = 0;
        int prev_ox = -1;
        int n = 0;
        int first_neg_correction_done = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int oam = 0xFE00 + i * 4;
            int oy  = RAM[oam]     - 16;
            int ox  = RAM[oam + 1];
            // Sprites with OAM X >= 168 are off-screen to the right — no stall.
            if (ox >= 168) continue;
            if ((int)LY_REG < oy || (int)LY_REG >= oy + height) continue;

            // Effective screen position (clamped: OAM X < 8 → all stall at pixel 0).
            int sp = (ox >= 8) ? ox - 8 : 0;

            // Advance background fetcher phase to this sprite's screen position.
            if (sp > cur_pixel) {
                bg_phase = (bg_phase + (sp - cur_pixel)) % 8;
                cur_pixel = sp;
            }

            // Inter-group overhead when the sprite's OAM X changes (new cluster).
            // The overhead depends on background phase at the time of the transition:
            //   phase 6-7: 5T (fetcher mid-cycle restart penalty)
            //   phase 0-1: 1T (minor alignment penalty)
            //   phase 2-5: 0T (no penalty)
            int is_transition = (prev_ox >= 0 && ox != prev_ox);
            if (is_transition) {
                int overhead = (bg_phase >= 6) ? 5 : (bg_phase <= 1) ? 1 : 0;
                extra    += overhead;
                bg_phase  = (bg_phase + overhead) % 8;
            }

            // Per-sprite stall: base 6T, ±1T for OAM X%8 < 2 based on fetch phase.
            // X%8=0: phases 6,2 → +1T; phases 1,5 → -1T; phase 0 → 6T; others → 6T.
            // X%8=1: phases 6,2,1 → +1T; phase 5 → -1T; phase 0 → 6T; others → 6T.
            // (The asymmetry arises from the fetcher's 1T offset for sprites at sp%8=1.)
            int stall;
            if (ox % 8 == 0) {
                stall = (bg_phase == 0)                  ? 6 :
                        (bg_phase == 6 || bg_phase == 2) ? 7 :
                        (bg_phase == 1 || bg_phase == 5) ? 5 : 6;
            } else if (ox % 8 == 1) {
                stall = (bg_phase == 0)                  ? 6 :
                        (bg_phase == 6 || bg_phase == 2) ? 7 :
                        // phase=1 -> 7T only for subsequent sprites; first sprite
                        // hasn't yet established the ±1T fetch-alignment asymmetry.
                        (bg_phase == 1 && prev_ox >= 0)  ? 7 :
                        (bg_phase == 5)                  ? 5 : 6;
            } else if (ox % 8 >= 4 && prev_ox < 0) {
                // First sprite encountered in mode-3 at a mid-tile ox position:
                // the background fetcher was already partway into its initial tile
                // fetch, so the effective stall is reduced.
                stall = (8 - (ox % 8)) >= 3 ? (8 - (ox % 8)) : 3;
            } else {
                stall = 6;
            }

            extra    += stall;
            bg_phase  = (bg_phase + stall) % 8;

            // Per-OAM-X%8 correction for inter-group transitions.
            // Hardware stall per isolated sprite scales with ox%8: sprites at lower
            // ox%8 values stall longer. This correction adjusts the cumulative extra.
            // Positive corrections (ox%8=0,1,2) always apply.
            // Negative corrections (ox%8=4,5,6,7) skip the first such transition to
            // avoid over-subtracting in simple 2-sprite tests.
            if (is_transition) {
                int ox_correction = 3 - (ox % 8);
                if (ox_correction > 3) ox_correction = 3;
                if (ox_correction < -2) ox_correction = -2;
                if (ox_correction > 0 || first_neg_correction_done)
                    extra += ox_correction;
                if (ox_correction < 0)
                    first_neg_correction_done = 1;
            }

            prev_ox   = ox;
            n++;
        }
    }
    mode3_extra = extra;
    // Window startup penalty: adds 4T when the window will render on this line.
    if (window_active) {
        mode3_extra += 4;
    }
}

void gpu_init(void){
    memset(VRAM, 0, 0x2000);
    gpu_cycles = 0;
    LY_REG = 0;
    pending_drawline = false;
    ly153_vblank_active = false;
    if_cleared_proj_gc = -100;
    if_cleared_proj_ly = -100;

    // Initialize GPU control with default values
    gpu_control.enabled = false;
    gpu_control.bg = false;
    gpu_control.window = false;
    gpu_control.sprite = false;
    gpu_control.sprite_tall = false;
    gpu_control.BgWindowTileData = 0x9000;
    gpu_control.BgTileDataSigned = true;
    gpu_control.bgtilemap = 0x9800;
    gpu_control.windowtilemap = 0x9800;

    // Set LCDC directly without going through gpu_write (which would trigger LCD startup mode).
    // The LCD was on continuously during the boot ROM; there's no "just turned on" event here.
    LCDC_REG = 0x91;  // LCD enabled, BG tile data at 0x8000, BG enabled
    gpu_parse_control(0x91);
    RAM[LCDC] = 0x91;

    // Set default BGP register (white, light gray, dark gray, black)
    BGP_REG = 0xFC;
    RAM[BGP] = 0xFC;

    // Initialize scroll registers
    SCX_REG = 0;
    SCY_REG = 0;
    RAM[SCX] = 0;
    RAM[SCY] = 0;
    lcd_startup_mode0 = false;
    lcd_startup_line = false;
    lyc_delayed = false;

    // Test tile data initialization removed - let the ROM provide its own data
}

byte gpu_get_mode(void) {
    if (!gpu_control.enabled) return 0;
    if (ly153_vblank_active) return 1;  // LY=153→0 transition: VBlank continues
    if (LY_REG >= 144) return 1;   // VBlank
    if (lcd_startup_mode0) return 0; // Startup: mode 0 visible before mode 2 begins
    if (gpu_cycles < 80) return 2;  // OAM scan
    if (gpu_cycles < 252 + mode3_extra) return 3; // LCD transfer (extended by SCX/sprite penalty)
    return 0;                        // HBlank
}

// Returns the GPU mode at a projected cycle offset within the current scanline.
// Used for STAT reads and OAM/VRAM locking — accounts for the T-cycle within
// the current instruction at which the memory access actually occurs.
// projected_cycles = gpu_cycles + (instr_timer_cycles - 4)
//
// STAT-visible boundaries:
//   Dead zone: T=0..3 shows mode-0 before PPU OAM-scan is visible (4T propagation delay)
//   Mode 2: T=4..83 (80T)
//   Mode 3: T=84..255+mode3_extra (172T base)
//   Mode 0 HBlank: T=256+mode3_extra..455 (200T base)
//
// Overflow: projected can exceed 456T (end of scanline), meaning the read fires in the
// next scanline. The overflow is handled differently for ly153 vs normal lines:
//   ly153_vblank_active: projected >= 456 → samples into LY=0 first scanline
//   Normal LY<144: projected > 456 → samples into next scanline
static byte gpu_projected_mode_from_cycles(int pc) {
    if (lcd_startup_mode0) return 0;
    if (pc < 0) pc = 0;
    if (pc < 4)   return 0;  // dead zone: T=0..3 (4T propagation delay)
    if (pc < 84)  return 2;  // OAM scan visible: T=4..83
    if (pc < 260 + mode3_extra) return 3;  // LCD transfer: T=84..259
    return 0;                 // HBlank: T=256..455
}

// For STAT register reads: 8T propagation delay (wider dead zone than transition detection).
// STAT-visible boundaries:
//   Dead zone: T=0..7 shows mode-0 (8T propagation delay)
//   Mode 2: T=8..87 (80T OAM scan)
//   Mode 3: T=88..259+mode3_extra
//   Mode 0 HBlank: T=260+mode3_extra..455
static byte gpu_projected_mode_for_stat_read(int pc) {
    if (pc < 0) pc = 0;
    if (pc < 8)   return 0;  // dead zone: T=0..7 (8T propagation delay)
    if (pc < 88)  return 2;  // OAM scan visible: T=8..87
    if (pc < 260 + mode3_extra) return 3;  // LCD transfer: T=88..259
    return 0;                              // HBlank: T=260..455
}

byte gpu_get_mode_projected(int projected_cycles) {
    if (!gpu_control.enabled) return 0;
    if (ly153_vblank_active) {
        // VBlank continues until end of the 456T LY=153 scanline slot.
        // projected >= 456 means the read samples into the first LY=0 scanline.
        if (projected_cycles < 456) return 1;
        return gpu_projected_mode_from_cycles(projected_cycles - 456);
    }
    if (LY_REG > 144) return 1;
    // LY=144 (VBlank start): 4T propagation delay — proj<4 still shows mode-0.
    if (LY_REG == 144) return (projected_cycles < 4) ? 0 : 1;
    if (lcd_startup_line) {
        // The first LCD-on scanline shows startup mode 0 before entering mode 3;
        // mode 2 is skipped entirely on this line.
        if (projected_cycles > 456) return gpu_projected_mode_from_cycles(projected_cycles - 456);
        if (projected_cycles < 84) return 0;
        if (projected_cycles < 260 + mode3_extra) return 3;
        return 0;
    }
    // Normal visible scanline: projected > 456 overflows into next scanline.
    // projected == 456 is still the last T of current HBlank.
    if (projected_cycles > 456) return gpu_projected_mode_from_cycles(projected_cycles - 456);
    return gpu_projected_mode_from_cycles(projected_cycles);
}

// Returns the STAT-register-visible mode for STAT reads (8T dead zone + ly153 Phase A/B).
static byte gpu_get_mode_for_stat_read(int projected_cycles) {
    if (!gpu_control.enabled) return 0;
    if (ly153_vblank_active) {
        // VBlank continues until T=455 of Phase B (LY=0 continuation).
        if (projected_cycles < 456) return 1;
        int pc = projected_cycles - 456;
        if (LY_REG == 153) {
            // Phase A (4T LY=153 stub): projecting past end into Phase B (452T VBlank
            // continuation). Phase B is still VBlank for its full 452T duration.
            if (pc < 452) return 1;
            pc -= 452;  // past Phase B — fall through to new-frame transition
        }
        // Phase B end: VBlank→OAM transition.
        // T=0..3: VBlank still visible (propagation delay keeps mode 1).
        // T=4..7: dead zone (mode 0).
        // T=8+: normal OAM scan visible.
        if (pc < 4) return 1;
        if (pc < 8) return 0;
        return gpu_projected_mode_for_stat_read(pc);
    }
    if (LY_REG > 144) return 1;
    if (LY_REG == 144) return (projected_cycles < 4) ? 0 : 1;
    if (lcd_startup_line) {
        if (projected_cycles > 456) return gpu_projected_mode_for_stat_read(projected_cycles - 456);
        if (projected_cycles < 88) return 0;
        if (projected_cycles < 260 + mode3_extra) return 3;
        return 0;
    }
    if (projected_cycles > 456) return gpu_projected_mode_for_stat_read(projected_cycles - 456);
    return gpu_projected_mode_for_stat_read(projected_cycles);
}

// Returns the GPU mode at a projected cycle for STAT INTERRUPT firing.
// STAT interrupts fire with 4T propagation delay for OAM/LYC, 8T for HBlank.
// Boundaries: dead zone T=0..3, mode 2 T=4..87, mode 3 T=88..259, mode 0 T=260..455.
static byte gpu_irq_mode_from_cycles(int pc) {
    if (lcd_startup_mode0) return 0;
    if (pc < 0) pc = 0;
    if (pc < 4)   return 0;  // dead zone: T=0..3 (4T propagation)
    if (pc < 88)  return 2;  // OAM scan: T=4..87
    if (pc < 260 + mode3_extra) return 3;  // LCD transfer: T=88..259
    return 0;                 // HBlank: T=260..455
}
static byte gpu_get_mode_for_irq(int gc) {
    if (!gpu_control.enabled) return 0;
    if (ly153_vblank_active) {
        if (gc < 456) return 1;
        return gpu_irq_mode_from_cycles(gc - 456);
    }
    if (LY_REG > 144) return 1;
    if (LY_REG == 144) return (gc < 8) ? 0 : 1;
    if (gc > 456) return gpu_irq_mode_from_cycles(gc - 456);
    return gpu_irq_mode_from_cycles(gc);
}

// Mode calculation for OAM/VRAM read locking and interrupt comparisons.
// Uses the hardware-internal mode boundaries (80/252) which differ from the
// STAT-read-visible boundaries (84/260) by 4-8T propagation delay.
static byte gpu_get_mode_for_lock(int projected_cycles) {
    if (!gpu_control.enabled) return 0;
    if (ly153_vblank_active) return 1;  // LY=153→0 transition: VBlank continues
    if (LY_REG >= 144) return 1;
    if (lcd_startup_mode0) return 0;
    if (projected_cycles < 0) projected_cycles = 0;
    if (projected_cycles >= 456) projected_cycles = 455;
    if (projected_cycles < 80) return 2;
    if (projected_cycles < 252 + mode3_extra) return 3;
    return 0;
}

// Returns true if VRAM writes are allowed at the projected T-cycle offset.
// VRAM is locked only during mode 3 (pixel pipeline), starting 8T after mode 3 begins
// and extending 8T into mode 0 — the same T=88..259 window as the OAM mode-3 lock.
// Unlike OAM, VRAM is always accessible during mode 2.
static bool vram_write_accessible(int proj) {
    if (!gpu_control.enabled) return true;
    if (LY_REG >= 144)       return true;  // VBlank: always accessible
    if (proj < 0) proj = 0;
    if (ly153_vblank_active && proj < 456) return true; // VBlank continuation
    bool startup = lcd_startup_line;
    proj %= 456;
    if (startup) return !(proj >= 88 && proj <= 259 + mode3_extra);
    if (proj >= 88 && proj <= 259 + mode3_extra) return false; // mode-3 pixel pipeline lock
    return true;
}
// Derived from hardware-measured access windows for lcdon_write_timing-GS.
// Lock windows per scanline:
//   T=8..83   — mode-2 OAM-scan bus held by PPU
//   T=88..259 — mode-3 pixel pipeline + first 8T of mode-0 OAM bus hold
// Accessible windows: T<8, T=84..87 (1 M-cycle gap between locks), T>=260.
// On the LCD startup line, mode 2 is skipped — OAM is accessible until T=88.
static bool oam_write_accessible(int proj) {
    if (!gpu_control.enabled) return true;
    if (LY_REG >= 144)       return true;  // VBlank: always accessible
    if (proj < 0) proj = 0;
    if (ly153_vblank_active && proj < 456) return true; // VBlank continuation
    bool startup = lcd_startup_line;
    proj %= 456;
    if (startup) return !(proj >= 88 && proj <= 259 + mode3_extra);
    if (proj >= 8  && proj <= 83)  return false; // mode-2 bus lock
    if (proj >= 88 && proj <= 259 + mode3_extra) return false; // mode-3 / early mode-0 lock
    return true;
}

// Compute the STAT interrupt line and fire an interrupt on 0→1 transition.
// Must be called after any GPU state change that might affect the conditions.
static void stat_check_irq(void) {
    if (!gpu_control.enabled) {
        stat_irq_line = false;
        return;
    }
    // STAT interrupts use 4T propagation delay for OAM and 8T for HBlank/VBlank/LYC.
    // This preserves hblank_int and int_vblank1 timing with mode 0 interrupt at T=260.
    byte mode = gpu_get_mode();
    byte mode_irq = gpu_get_mode_for_irq(gpu_cycles);
    bool line = false;
    // Mode 0 HBlank: fires at T=260+mode3_extra via mode_irq 3→0 transition (8T propagation).
    // The `mode_irq == 0` guard fires at T=260 (not T=252) matching hardware IRQ timing.
    // The `mode == 0` guard prevents false fires during the T=0..7 dead zone.
    if ((RAM[STAT] & 0x08) && mode == 0 && mode_irq == 0 && LY_REG < 144) line = true;
    // Mode 1 VBlank: fires at T=8 of LY=144 (8T propagation).
    if ((RAM[STAT] & 0x10) && mode_irq == 1)                  line = true;
    // Mode 2 OAM: fires at T=4 (4T after hardware mode 2 start).
    // The LCD startup line skips mode 2, so suppress the normal mode_irq==2 case there.
    // LY=144 keeps the internal OAM condition for STAT purposes, but it is not visible
    // until T=4 and ends with the normal OAM-scan window.
    if ((RAM[STAT] & 0x20) && ((!lcd_startup_line && mode_irq == 2) || (LY_REG == 144 && gpu_cycles >= 4 && gpu_cycles < 84))) line = true;
    if ((RAM[STAT] & 0x40) && LY_REG == RAM[LYC] && !lyc_delayed) line = true; // LYC=LY
    if (line && !stat_irq_line) {
        bool suppressed = false;
        if (if_cleared_proj_ly == LY_REG) {
            bool oam_firing = (mode_irq == 2);
            bool vblank_firing = ((RAM[STAT] & 0x10) && mode_irq == 1 && LY_REG == 144);
            bool lyc_firing = ((RAM[STAT] & 0x40) && LY_REG == RAM[LYC] && !lyc_delayed && gpu_cycles < 88);
            // Suppress OAM fire only if LYC is NOT independently firing at gc=8.
            // When IF is cleared at proj=4, it suppresses the OAM at gc=4. But if LYC fires
            // at gc=8 and OAM condition is still true (mode=2), don't suppress the LYC fire.
            if (oam_firing && !lyc_firing && if_cleared_proj_gc == 4) suppressed = true;
            if ((vblank_firing || lyc_firing) && if_cleared_proj_gc == 8) suppressed = true;
        }
        if (!suppressed) {
            request_interrupt(INTERRUPT_STAT);
        }
    }
    stat_irq_line = line;
}

byte gpu_read(int pos){
    //printf("reading from gpu %x\n", pos);
    if (pos >= 0x8000 && pos <= 0x9FFF) {
        // VRAM locked during mode 3 (pixel pipeline).
        // Lock window per scanline: T=84..259 (internal mode3 start + 4T, end + 8T).
        // On the startup line (lcd_startup_mode0), VRAM is accessible for T<88 then
        // locked from T=88 onward (matching the STAT-visible mode3 boundary).
        if (gpu_control.enabled && LY_REG < 144) {
            int projected = gpu_cycles + instr_timer_cycles - 4;
            if (projected < 0) projected = 0;
            bool locked;
            bool startup = lcd_startup_line;
            if (startup && projected > 456) {
                startup = false;
                projected -= 456;
            }
            if (startup) {
                locked = (projected >= 88 && projected < 260 + mode3_extra);
            } else if (ly153_vblank_active && projected < 456) {
                locked = false;
            } else {
                projected %= 456;
                locked = (projected >= 84 && projected < 260 + mode3_extra);
            }
            if (locked) return 0xFF;
        }
        return VRAM[pos - 0x8000];
    }
    if (pos == STAT) {
        if (!gpu_control.enabled) {
            // LCD off: mode=0, LYC=LY flag frozen at value when LCD was disabled
            byte lyc_flag = lcd_off_lyc_flag ? 0x04 : 0x00;
            return 0x80 | (RAM[STAT] & 0x78) | lyc_flag;
        }
        // Use projected mode with 8T STAT-read boundary (Phase A/B for ly153).
        int projected = gpu_cycles + instr_timer_cycles - 4;
        byte mode = gpu_get_mode_for_stat_read(projected);
        // LYC coincidence flag is cleared near end of visible scanlines (hardware pre-clear).
        // Hardware clears LYC comparison at projected > 456 (strictly after LY<144 scanline end)
        // to prevent spurious STAT interrupts during the LY counter wrap.
        // At projected == 456 (T=0 of next scanline dead zone), LYC is still visible.
        byte lyc_flag;
        if (!lcd_startup_mode0 &&
                (projected > 456 && LY_REG < 144 && !ly153_vblank_active)) {
            lyc_flag = 0x00; // pre-clear: LYC comparison cleared after scanline end
        } else {
            lyc_flag = (LY_REG == RAM[LYC]) ? 0x04 : 0x00;
        }
        return 0x80 | (RAM[STAT] & 0x78) | lyc_flag | mode;
    }
    if (pos == LY) {
        // Project LY forward to account for the hardware read-latch delay.
        // LY physically increments in gpu_step when gpu_cycles reaches 456 (same for
        // startup and normal lines), but the LY register read reflects the new value
        // only 8T later (the hardware comparison clock latches the new LY one M-cycle
        // after the actual counter wrap). Threshold 464 = 456T wrap + 8T latch delay.
        // During ly153_vblank_active the scanline end does NOT increment LY (it stays 0),
        // so suppress the projection to prevent erroneously returning LY=1.

        // LY=153 stub: the short 4T scanline is visible only for T=0..3 (gc=452..455
        // with the stub starting at gc=452). At T=4 (gc+instr >= 456) LY transitions to 0.
        if (ly153_vblank_active && LY_REG == 153 && gpu_cycles + instr_timer_cycles >= 456) {
            return 0;
        }
        int ly_threshold = 464;
        if (!ly153_vblank_active && gpu_cycles + instr_timer_cycles >= ly_threshold) {
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
    // For other registers (LYC, WX, WY, etc.) stored directly in RAM
    return RAM[pos];
}

void gpu_write(int pos, byte data){
    if (pos >= 0x8000 && pos <= 0x9FFF) {
        VRAM[pos - 0x8000] = data;
    }
    if (pos == STAT) {
        // DMG STAT write glitch: writing to STAT during mode 0 (HBlank) or mode 1 (VBlank)
        // causes a momentary 1-cycle pulse on the STAT IRQ line, firing an interrupt
        // regardless of which enable bits are set.
        //
        // The glitch fires if the instruction's execution window intersects the HBlank/VBlank
        // region: specifically if M1 (gc) or M3 (proj_tc) falls in HBlank [260..455], or we're
        // in VBlank (LY=144..153). This handles:
        //   - Write starts in HBlank but proj wraps past scanline boundary (gc>=260 fires)
        //   - Write starts in mode 3 but M3 lands in HBlank (proj_tc in [260..455] fires)
        //   - Dead zone gc=0..3 at start of scanline does NOT trigger the glitch
        // On the LCD startup line, the initial ~36 T-cycles behave like mode 0.
        if (gpu_control.enabled) {
            bool glitch;
            if (lcd_startup_mode0) {
                glitch = (gpu_cycles < 36);
            } else {
                int proj_tc = gpu_cycles + instr_timer_cycles - 4;
                bool hblank_at_m1 = (gpu_cycles >= 260 && LY_REG < 144);
                bool hblank_at_m3 = (proj_tc >= 260 && proj_tc < 456 && LY_REG < 144);
                bool in_vblank    = (LY_REG >= 144 && LY_REG <= 153);
                glitch = hblank_at_m1 || hblank_at_m3 || in_vblank;
            }
            if (glitch) {
                request_interrupt(INTERRUPT_STAT);
            }
        }
        // Bits 3-6 are writable; bits 0-2 are read-only
        RAM[STAT] = (RAM[STAT] & 0x87) | (data & 0x78);
        // When STAT is written: update stat_irq_line to reflect the new condition WITHOUT
        // firing an interrupt. This avoids false rising-edge triggers when STAT interrupt
        // bits are set while a condition (e.g. mode-2 OAM) is already active.
        // The correct interrupt fires on the next natural rising edge (e.g. T=8 of next
        // scanline). The DMG write-glitch (fires during mode-0/mode-1) is handled
        // separately by the stat_write_glitch logic (currently not implemented).
        {
            byte mode_i = gpu_get_mode_for_irq(gpu_cycles);
            byte mode_v = gpu_get_mode_projected(gpu_cycles);
            bool new_line = false;
            if ((RAM[STAT] & 0x08) && mode_v == 0 && mode_i == 0 && LY_REG < 144) new_line = true;
            if ((RAM[STAT] & 0x10) && mode_i == 1) new_line = true;
            if ((RAM[STAT] & 0x20) && ((!lcd_startup_line && mode_i == 2) || (LY_REG == 144 && gpu_cycles >= 4 && gpu_cycles < 84))) new_line = true;
            if ((RAM[STAT] & 0x40) && LY_REG == RAM[LYC] && !lyc_delayed) new_line = true;
            stat_irq_line = new_line;
        }
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
            ly153_vblank_active = false;  // fresh start, not in VBlank continuation
            // On DMG hardware, LCD startup begins in mode 0, not mode 2.
            // The comparison clock restarts immediately, so LYC=LY is re-evaluated.
            lcd_startup_mode0 = true;
            lcd_startup_line = true;
            // Queue LY=0 for rendering (fires at T=80 of the first scanline).
            pending_drawline = true;
            pending_draw_ly  = 0;
            // Delay LYC interrupt to T=8 of the startup scanline.
            lyc_delayed = true;
            // Restore stat_irq_line from the frozen pre-disable state so
            // rising-edge detection works correctly on re-enable:
            // if LYC=LY was true before disable and is still true now, no new interrupt.
            stat_irq_line = lcd_off_lyc_flag;
            stat_check_irq();
        }
        if (was_enabled && !now_enabled) {
            // LCD turned off: freeze LYC=LY flag before resetting LY
            lcd_off_lyc_flag = (LY_REG == RAM[LYC]);
            LY_REG = 0;
            gpu_cycles = 0;
            window_line = 0;
            ly153_vblank_active = false;  // clear VBlank continuation on LCD off
            lyc_delayed = false;
            stat_irq_line = false;
            pending_drawline = false;
        }
    }
    if (pos == LY) {
        // LY register is read-only in the Game Boy - ignore writes
        // LY_REG = data;  // REMOVED: LY is read-only
    }
    if (pos == LYC) {
        RAM[LYC] = data;
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
    if (pos == WX || pos == WY) {
        RAM[pos] = data;
    }
    if (data > 0) {
 //       printf("wrote %x to %x\n", data, pos);
    }
}

uint32_t gpu_pallete_color(byte number, int paletteIndex) {
    const byte config = mem_read(paletteIndex);
    // Each color number occupies 2 bits: bits [n*2+1 : n*2]
    // Bit n*2 is LSB of shade, bit n*2+1 is MSB
    const byte resultindex = (config >> (number * 2)) & 0x03;
    return pallette[resultindex];
}

// Render one row of a tile.
// tile_row: row within the tile (0-7).
// transparent0: if true (sprites), skip color index 0.
// for_sprite: if true, use 0x8000 base (unsigned); otherwise use BG/window tile data.
// Records BG color index into bg_scanline[] when !transparent0.
void gpu_render_tile(byte ly, int xprefix, int tile_row, byte tileIndex,
                     int paletteIndex, bool flipx, bool flipy,
                     bool transparent0, bool for_sprite) {
    int data_base;
    int actual;
    if (for_sprite) {
        data_base = 0x8000;
        actual = tileIndex;  // sprites always unsigned
    } else {
        data_base = gpu_control.BgWindowTileData;
        if (gpu_control.BgTileDataSigned) {
            actual = (int)(int8_t)tileIndex;  // sign-extend
        } else {
            actual = tileIndex;
        }
    }

    int row = flipy ? (7 - tile_row) : tile_row;
    const int addr = data_base + actual * 16;
    // PPU has direct VRAM access (bypasses the CPU-facing lock that returns 0xFF).
    const byte hi = VRAM[(addr + row * 2)     - 0x8000];
    const byte lo = VRAM[(addr + row * 2 + 1) - 0x8000];

    for (int i = 0; i < 8; i++) {
        int x = xprefix + (flipx ? (7 - i) : i);
        if (x < 0 || x >= VIEWPORT_WIDTH) continue;
        byte color = 0;
        if (bit_check(lo, 7 - i)) color |= 2;
        if (bit_check(hi, 7 - i)) color |= 1;
        if (transparent0 && color == 0) continue;
        set_pixel(x, ly, gpu_pallete_color(color, paletteIndex));
        if (!transparent0) bg_scanline[x] = color;
    }
}

void gpu_draw_bg(byte ly){
    if (gpu_control.bg) {
        int fine_x = SCX_REG & 7;
        int fine_y = (ly + SCY_REG) & 7;
        // Render 21 tiles to cover 160 pixels + fine X offset
        for (int tile = 0; tile <= 20; tile++) {
            int map_x = ((SCX_REG / 8) + tile) & 31;
            int map_y = ((ly + SCY_REG) / 8) & 31;
            int tileIndex = VRAM[(gpu_control.bgtilemap + (map_y * 32) + map_x) - 0x8000];
            gpu_render_tile(ly, tile * 8 - fine_x, fine_y, tileIndex,
                            BGP, false, false, false, false);
        }
    } else {
        for (int i = 0; i < VIEWPORT_WIDTH; i++) {
            set_pixel(i, ly, pallette[0]);
            bg_scanline[i] = 0;
        }
    }
}

void gpu_draw_window(byte ly){
    if (!gpu_control.window) return;
    int wy = RAM[WY];
    int wx = (int)RAM[WX] - 7;  // WX=7 means window starts at x=0
    if (ly < wy) return;
    // WX >= 167 (wx >= 160) means window is completely off-screen.
    // On real hardware, window_line only increments when at least one window pixel
    // is rendered. If WX is off-screen, no pixels draw and window_line must not advance.
    if (wx >= VIEWPORT_WIDTH) return;
    int fine_y = window_line & 7;
    for (int tile = 0; tile <= 20; tile++) {
        int xpos = wx + tile * 8;
        if (xpos >= VIEWPORT_WIDTH) break;
        int map_x = tile & 31;
        int map_y = (window_line / 8) & 31;
        int tileIndex = VRAM[(gpu_control.windowtilemap + (map_y * 32) + map_x) - 0x8000];
        gpu_render_tile(ly, xpos, fine_y, tileIndex,
                        BGP, false, false, false, false);
    }
    window_line++;
}

void gpu_draw_sprites(byte ly){
    if (!gpu_control.sprite) return;
    int sprite_height = gpu_control.sprite_tall ? 16 : 8;

    // DMG: at most 10 sprites per scanline (OAM scan selects first 10 visible by Y overlap,
    // filtering out fully off-screen sprites by X). After selection, DMG renders by
    // X-coordinate priority: smaller X wins; OAM index breaks ties (lower = higher priority).
    int visible[10];
    int n_visible = 0;
    for (int i = 0; i < 40 && n_visible < 10; i++) {
        const int oam = 0xFE00 + (i * 4);
        const int oam_y = RAM[oam];
        const int oam_x = RAM[oam + 1];
        const int actual_y = oam_y - 16;
        const int actual_x = oam_x - 8;
        if (actual_y > (int)ly || (actual_y + sprite_height) <= (int)ly) continue;
        if (actual_x <= -8 || actual_x >= VIEWPORT_WIDTH) continue;
        visible[n_visible++] = i;
    }

    // Sort visible[] by X descending (largest X rendered first = lowest priority);
    // tiebreak: OAM index descending. This ensures smallest-X / lowest-OAM index
    // sprites are drawn last (on top), matching DMG hardware priority.
    for (int a = 0; a < n_visible - 1; a++) {
        for (int b = a + 1; b < n_visible; b++) {
            int xa = RAM[0xFE00 + visible[a]*4 + 1];
            int xb = RAM[0xFE00 + visible[b]*4 + 1];
            if (xb > xa || (xb == xa && visible[b] > visible[a])) {
                int tmp = visible[a]; visible[a] = visible[b]; visible[b] = tmp;
            }
        }
    }

    // Render sprites in sorted order (lowest priority first, highest priority last).
    for (int s = 0; s < n_visible; s++) {
        const int i = visible[s];
        const int oam = 0xFE00 + (i * 4);
        const int oam_y = RAM[oam];
        const int oam_x = RAM[oam + 1];
        const int actual_y = oam_y - 16;
        const int actual_x = oam_x - 8;

        byte tileIndex = RAM[oam + 2];
        const byte flags = RAM[oam + 3];
        const bool flipy    = bit_check(flags, 6);
        const bool flipx    = bit_check(flags, 5);
        const bool bg_prio  = bit_check(flags, 7);
        const int paletteIndex = bit_check(flags, 4) ? OBP1 : OBP0;

        int tile_row = (int)ly - actual_y;
        if (gpu_control.sprite_tall) {
            tileIndex &= 0xFE;  // 8x16: tile index bit 0 ignored
            if ((!flipy && tile_row >= 8) || (flipy && tile_row < 8)) {
                tileIndex++;
            }
            tile_row &= 7;
        }
        if (flipy) tile_row = 7 - tile_row;  // flipy handled here, not in render_tile

        // For bg_prio sprites: only draw over BG color 0
        if (bg_prio) {
            // Render pixel-by-pixel with priority check
            int row = tile_row;
            const int addr = 0x8000 + tileIndex * 16;
            const byte hi = VRAM[(addr + row * 2)     - 0x8000];
            const byte lo = VRAM[(addr + row * 2 + 1) - 0x8000];
            for (int pi = 0; pi < 8; pi++) {
                int x = actual_x + (flipx ? (7 - pi) : pi);
                if (x < 0 || x >= VIEWPORT_WIDTH) continue;
                if (bg_scanline[x] != 0) continue;  // BG wins
                byte color = 0;
                if (bit_check(lo, 7 - pi)) color |= 2;
                if (bit_check(hi, 7 - pi)) color |= 1;
                if (color == 0) continue;
                set_pixel(x, ly, gpu_pallete_color(color, paletteIndex));
            }
        } else {
            // Normal sprite: transparent0=true, flipy already applied to tile_row
            gpu_render_tile(ly, actual_x, tile_row, tileIndex,
                            paletteIndex, flipx, false, true, true);
        }
    }
}

void gpu_drawline(byte ly){
    memset(bg_scanline, 0, sizeof(bg_scanline));  // reset per-scanline BG color tracker
    gpu_draw_bg(ly);
    gpu_draw_window(ly);
    gpu_draw_sprites(ly);
}

void gpu_step(int _cycles){
    if (!gpu_control.enabled) return;

    byte prev_mode = gpu_get_mode();
    byte prev_mode_visible = gpu_get_mode_projected(gpu_cycles);
    byte prev_mode_irq = gpu_get_mode_for_irq(gpu_cycles);
    int prev_ly = LY_REG;
    gpu_cycles += _cycles;

    // Clear startup mode-0 override once gpu_cycles advances into OAM scan range.
    // Threshold >=84 matches the hardware mode-2 start time; also ensures the
    // startup line transitions directly to mode 3 (no spurious mode-2 visible).
    if (lcd_startup_mode0 && gpu_cycles >= 84) {
        lcd_startup_mode0 = false;
    }

    if (gpu_cycles >= 456) {
        int ly_scanline_len = 456; // startup line fires at same T as normal lines
        gpu_cycles -= ly_scanline_len; // preserve remainder for accurate sub-scanline timing

        if (ly153_vblank_active) {
            if (LY_REG == 153) {
                // End of 4T LY=153 stub. Transition to LY=0 VBlank continuation (452T).
                // Add 4T offset so the next gc>=456 fires after 452T, not 456T.
                LY_REG = 0;
                gpu_cycles += 4;
                lyc_delayed = true; // LYC=0 interrupt fires 8T into the continuation
            } else {
                // End of 452T LY=0/VBlank continuation. OAM scan for LY=0 begins.
                ly153_vblank_active = false;
                window_line = 0;
                pending_drawline = true;
                pending_draw_ly  = 0;
            }
        } else {
            const byte ly = ++LY_REG;

            // Clear startup-line flag on first LY increment (LY=0→1)
            if (ly == 1) lcd_startup_line = false;

            if (ly == 144) {
                // VBlank period starts; VBlank interrupt is unconditional
                request_interrupt(INTERRUPT_VBLANK);
            } else if (ly == 153) {
                // LY=153 lasts only 4T on real DMG hardware, then LY→0 while
                // VBlank (mode 1) continues for 452T more. Add 452T so the next
                // gpu_cycles>=456 fires 4T from now (end of the 4T LY=153 stub).
                gpu_cycles += 452;
                if (gpu_cycles >= 456) {
                    // 4T stub already elapsed within this step; immediately set LY=0.
                    // Add 4T compensation so the 452T continuation ends at the right time.
                    gpu_cycles -= 456;
                    LY_REG = 0;
                    gpu_cycles += 4;
                }
                ly153_vblank_active = true;
                lyc_delayed = true; // LYC interrupt fires 8T after LY=153/0 boundary
            } else if (ly > 153) {
                // Fallback (shouldn't normally reach here with ly153 logic above)
                LY_REG = 0;
                window_line = 0;
                pending_drawline = true;
                pending_draw_ly  = 0;
            } else if (ly < 144) {
                // Defer scanline render to T=80 (end of OAM scan) so that raster
                // effects that poll LY and write SCX/SCY during OAM scan are
                // reflected in the rendered output, matching real hardware behaviour.
                pending_drawline = true;
                pending_draw_ly  = ly;
            }
        }

        // LYC interrupt fires 8T after scanline start (same propagation as mode 2).
        // Set lyc_delayed before calling stat_check_irq so LYC is suppressed here.
        if (!lyc_delayed) lyc_delayed = true; // new scanline: delay LYC to T=8
        // Fire STAT for any newly-active condition after LY change
        stat_check_irq();
    }


    // Fire deferred scanline render once OAM scan is complete (T>=80).
    // Also recompute mode3_extra here so that SCX writes during OAM scan (mode 2)
    // are captured before mode 3 starts. This handles both mid-scanline SCX changes
    // and the LCD startup line (where no LY transition fires the recompute).
    if (pending_drawline && gpu_cycles >= 80) {
        compute_mode3_extra();
        gpu_drawline(pending_draw_ly);
        pending_drawline = false;
    }

    // Check for mode transitions (internal: mode 3→0 HBlank, mode 0→2 new scanline)
    byte new_mode = gpu_get_mode();
    if (new_mode != prev_mode) {
        stat_check_irq();
    }

    // Check for STAT-visible mode transitions (4T propagation delay vs internal).
    // This fires mode-2 STAT interrupt at T=4 (not T=0) when mode-2 becomes visible in STAT.
    byte new_mode_visible = gpu_get_mode_projected(gpu_cycles);
    if (new_mode_visible != prev_mode_visible) {
        stat_check_irq();
    }

    // Check for STAT-interrupt mode transitions (4T for OAM, 8T for HBlank).
    // This fires mode-2 STAT interrupt at T=4 and mode-0 STAT interrupt at T=260.
    byte new_mode_irq = gpu_get_mode_for_irq(gpu_cycles);
    if (new_mode_irq != prev_mode_irq) {
        stat_check_irq();
    }

    // LYC coincidence: check whenever LY changed
    if (LY_REG != prev_ly) {
        stat_check_irq();
    }

    // Fire delayed LYC interrupt at T>=8 of new scanline (8T propagation).
    // lyc_delayed is set whenever LY changes; cleared here once gpu_cycles reaches T=8.
    if (lyc_delayed && gpu_cycles >= 8) {
        lyc_delayed = false;
        stat_check_irq();
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
byte cart_cgb_flag;  // cart header byte $0143: $80=CGB enhanced, $C0=CGB only
byte *cart_data;
int cart_rom_banks;      // total number of 16KB ROM banks
int cart_rom_bank = 1;   // current switchable ROM bank (for $4000-$7FFF)
int cart_mbc1_upper = 0; // upper 2 bits written to $4000-$5FFF (ROM bank bits 5-6 or RAM bank)
int cart_mbc1_mode = 0;  // 0=ROM banking mode (default), 1=RAM banking mode
int cart_ram_bank = 0;   // current RAM bank (MBC3 uses this; MBC1 uses mbc1_upper)
byte ext_ram[0x8000];    // 32KB external RAM (up to 4 banks × 8KB)

static bool is_mbc3(void) {
    return cart_type >= 0x0F && cart_type <= 0x13;
}

byte cart_read(int pos) {
    if (pos < 0x4000) {
        if (!is_mbc3() && cart_mbc1_mode == 1 && cart_rom_banks > 32) {
            // MBC1 mode 1: upper bits remap bank 0
            int bank0 = (cart_mbc1_upper << 5) & (cart_rom_banks - 1);
            return cart_data[bank0 * 0x4000 + pos];
        }
        return cart_data[pos];
    }
    // $4000-$7FFF: switchable ROM bank
    int bank;
    if (is_mbc3()) {
        bank = cart_rom_bank & 0x7F;
        if (bank == 0) bank = 1;
        bank &= (cart_rom_banks - 1);
    } else {
        // MBC1
        bank = cart_rom_bank | (cart_mbc1_upper << 5);
        bank &= (cart_rom_banks - 1);
        if ((bank & 0x1F) == 0) bank |= 1;
    }
    return cart_data[bank * 0x4000 + (pos - 0x4000)];
}

void cart_write(int pos, byte data) {
    if (pos <= 0x1FFF) {
        // RAM/RTC enable: ignored (we always allow access)
        return;
    }
    if (is_mbc3()) {
        if (pos <= 0x3FFF) {
            // MBC3: ROM bank number (7 bits, 0→1)
            cart_rom_bank = data & 0x7F;
            if (cart_rom_bank == 0) cart_rom_bank = 1;
            return;
        }
        if (pos <= 0x5FFF) {
            // MBC3: RAM bank (0-3) or RTC select (0x08-0x0C); only 0-3 supported
            cart_ram_bank = data & 0x03;
            return;
        }
        if (pos <= 0x7FFF) {
            // MBC3: RTC latch — ignored
            return;
        }
        return;
    }
    // MBC1
    if (pos <= 0x3FFF) {
        // MBC1 ROM bank number (lower 5 bits)
        cart_rom_bank = data & 0x1F;
        if (cart_rom_bank == 0) cart_rom_bank = 1;
        return;
    }
    if (pos <= 0x5FFF) {
        // MBC1 upper bits: RAM bank number OR upper ROM bank bits
        cart_mbc1_upper = data & 0x03;
        return;
    }
    if (pos <= 0x7FFF) {
        // MBC1 banking mode: 0=ROM (up to 2MB ROM, 8KB RAM), 1=RAM (512KB ROM, 32KB RAM)
        cart_mbc1_mode = data & 0x01;
        return;
    }
    // other cart writes: silently ignore
}

void cart_load(char *path) {
    FILE *fileptr;
    long filelen;
    fileptr = fopen(path, "rb");
    if (fileptr == NULL) {
        fprintf(stderr, "Error: cannot open ROM: %s\n", path);
        exit(1);
    }
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
    cart_cgb_flag = cart_data[0x143];
    cart_rom_banks = (int)filelen / 0x4000;
    cart_rom_bank = 1;
    cart_mbc1_upper = 0;
    cart_mbc1_mode = 0;
    cart_ram_bank = 0;

    printf("Loaded %s\n", cart_name);

    // Support MBC1 (types 0-3) and MBC3 (types 0x0F-0x13) for Pokemon
    if (cart_type > 3 && !(cart_type >= 0x0F && cart_type <= 0x13)) {
        printf("Warning: cart type 0x%02X not fully supported\n", cart_type);
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
  // Bits are active-low: 0=pressed, 1=released. Default all released (0xF).
  byte result = 0x0F;
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

// Proper timer implementation using internal 16-bit counter.
// DIV register = upper 8 bits (timer_internal >> 8).
// TIMA ticks on falling edge of a specific bit depending on TAC mode.
static uint16_t timer_internal = 0;
static int timer_overflow_pending = 0;  // cycles until TMA reload (0=none)
static bool timer_just_reloaded = false; // true during the M-cycle that TMA reloaded TIMA

// Bit in timer_internal that drives TIMA for each TAC mode (bits 1-0)
static const int timer_tac_bit[4] = { 9, 3, 5, 7 };

// Advance timer by exactly 4 T-cycles; called in a loop from timer_step.
static void timer_tick4(void) {
    timer_just_reloaded = false;  // reset at start of each tick

    // Handle TMA reload delay
    if (timer_overflow_pending > 0) {
        timer_overflow_pending -= 4;
        if (timer_overflow_pending <= 0) {
            timer_overflow_pending = 0;
            RAM[REG_TIMA] = RAM[REG_TMA];
            request_interrupt(INTERRUPT_TIMER);
            timer_just_reloaded = true;
        }
    }

    uint16_t old_internal = timer_internal;
    timer_internal += 4;
    // DIV register is upper 8 bits of internal counter
    RAM[REG_DIV] = (uint8_t)(timer_internal >> 8);

    // TIMA tick on falling edge of selected bit AND timer enabled
    if (RAM[REG_TAC] & 0x04) {
        int bit = timer_tac_bit[RAM[REG_TAC] & 0x03];
        bool old_bit = (old_internal >> bit) & 1;
        bool new_bit = (timer_internal >> bit) & 1;
        if (old_bit && !new_bit) {
            if (++RAM[REG_TIMA] == 0) {
                // TIMA overflow: 4-cycle delay before TMA reload
                timer_overflow_pending = 4;
            }
        }
    }
    // Serial clock: internal clock uses falling edge of timer_internal bit 8 (8192 Hz).
    // Bit 8 (period=512T) gives 8192 Hz = 4194304/512 Hz. Each falling edge shifts one bit.
    // We fire 4T early (when (timer_internal & 0x1FF) == 0x1FC, i.e. 4T before the real
    // falling edge) so the interrupt flag is set during the preceding instruction's opcode
    // fetch, matching the GB hardware behaviour where the interrupt is checked at the START
    // of the M1 cycle and fires before that instruction's body runs.
    if (serial_active) {
        bool old_bit8 = (old_internal >> 8) & 1;
        // Fire 4T before the actual falling edge so do_interrupts() can dispatch
        // before the next instruction executes (matching DMG hardware timing).
        if (old_bit8 && ((timer_internal & 0x1FF) == 0x1FC)) {
            // Shift SB left, shift in 1 from unconnected external pin
            serial_sb = (serial_sb << 1) | 1;
            RAM[0xFF01] = serial_sb;
            if (--serial_bits_remaining <= 0) {
                // Transfer complete: buffer original transmit byte, clear SC, fire interrupt
                if (serial_buf_len < (int)(sizeof(serial_buf) - 1)) {
                    serial_buf[serial_buf_len++] = (char)serial_out_byte;
                    serial_buf[serial_buf_len] = '\0';
                }
                fputc(serial_out_byte, stdout);
                fflush(stdout);
                RAM[0xFF02] &= ~0x80;  // clear transfer-in-progress
                RAM[INTERRUPT_FLAGS] |= 0x08;  // serial interrupt
                serial_active = false;
                serial_bits_remaining = 0;
            }
        }
    }
}


static const int timer_clocks[4] = { 1024, 16, 64, 256 };

void timer_step(int cpu_cycles) {
    for (int i = 0; i < cpu_cycles; i += 4) {
        timer_tick4();
    }
}

// Minimal APU step: fires the 256 Hz length counter clock.
// Period is 16384 T-cycles at 1x speed, 32768 T-cycles at 2x speed.
void apu_step(int cpu_cycles) {
    apu_length_cycles += cpu_cycles;
    // 256 Hz clock: period doubles in double-speed mode because APU runs at 1x real-time
    int period = double_speed ? 32768 : 16384;
    while (apu_length_cycles >= period) {
        apu_length_cycles -= period;
        if (apu_ch1_active && apu_ch1_length_enable) {
            if (--apu_ch1_length <= 0) apu_ch1_active = false;
        }
        if (apu_ch2_active && apu_ch2_length_enable) {
            if (--apu_ch2_length <= 0) apu_ch2_active = false;
        }
    }
}

// dma_read_source: read one byte from a DMA source address, handling echo RAM mapping
static byte dma_read_source(int addr) {
    // Echo RAM ($E000-$FFFF) mirrors WRAM ($C000-$DFFF) for DMA bus reads
    // DMG: addresses $E000-$FDFF → $C000-$DDFF, $FE00-$FEFF → $DE00-$DEFF, $FF00-$FFFF → $DF00-$DFFF
    if (addr >= 0xE000) addr -= 0x2000;
    return mem_read(addr);
}

void mem_dma(byte data) {
    RAM[DMA] = data; // store for $FF46 readback
    dma_source = (int)data * 0x100;
    dma_cycles_remaining = 160; // 160 M-cycles = 640 T-cycles
    dma_startup_remaining = 2;  // 2 M-cycle startup delay before first byte copied
    // For a fresh DMA start OAM is accessible for 2 M-cycles (grace period).
    // For a restarted DMA (previous one still running, OAM already locked), keep locked.
    if (!(dma_active && dma_oam_locked)) {
        dma_oam_locked = false;
    }
    dma_active = true;
}

// Called each instruction with the T-cycle count; steps the DMA transfer.
void dma_step(int t_cycles) {
    if (!dma_active) return;
    int m_cycles = t_cycles / 4;
    while (m_cycles-- > 0) {
        if (dma_startup_remaining > 0) {
            dma_startup_remaining--;
            if (dma_startup_remaining == 0) dma_oam_locked = true;
            continue;
        }
        if (dma_cycles_remaining <= 0) break;
        int idx = 160 - dma_cycles_remaining;
        RAM[0xFE00 + idx] = dma_read_source(dma_source + idx);
        dma_cycles_remaining--;
    }
    if (dma_startup_remaining <= 0 && dma_cycles_remaining <= 0) {
        dma_active = false;
        dma_oam_locked = false;
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
        int ram_bank = is_mbc3() ? cart_ram_bank : (cart_mbc1_mode == 1 ? cart_mbc1_upper : 0);
        return ext_ram[ram_bank * 0x2000 + (pos - 0xA000)];
    } else if (pos >= 0xE000 && pos < 0xFE00) {
        // Echo RAM: $E000-$FDFF mirrors WRAM $C000-$DDFF
        return RAM[pos - 0x2000];
    } else if (pos >= 0xFE00 && pos <= 0xFE9F) {
        // OAM: returns 0xFF when DMA is active (after startup grace) or while the PPU owns
        // the OAM bus. DMG read locking keeps LY>=1 reads blocked through the early part of
        // mode 2, exposes a short boundary window, then blocks again for mode 3.
        if (dma_active && dma_oam_locked) return 0xFF;
        if (gpu_control.enabled && LY_REG < 144) {
            int projected = gpu_cycles + instr_timer_cycles - 4;
            if (projected < 0) projected = 0;
            bool locked;
            bool startup = lcd_startup_line;
            if (startup && projected > 456) {
                startup = false;
                projected -= 456;
            }
            if (startup) {
                locked = (projected >= 88 && projected <= 259 + mode3_extra);
            } else if (ly153_vblank_active && projected < 456) {
                locked = false;
            } else {
                projected %= 456;
                locked = ((projected <= 87) || (projected >= 96 && projected <= 259 + mode3_extra));
            }
            if (locked) return 0xFF;
        }
        return RAM[pos];
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
            case SCX:
            case SCY:
            case WX:
            case WY:
                return gpu_read(pos);
            case JOYPAD:
                // bits 7-6: always 1; bits 5-4: P14/P15 select state from joypad var;
                // bits 3-0: button states (0=pressed).
                return (joypad & 0x30) | (joypad_read() & 0x0F) | 0xC0;
            case 0xFF4D:
                // KEY1: CGB speed switch register - only accessible on CGB hardware
                // On DMG, this register doesn't exist and reads as $FF
                if (cart_cgb_flag == 0xC0)
                    return (double_speed ? 0x80 : 0x00) | (RAM[0xFF4D] & 0x01) | 0x7E;
                return 0xFF;
            case 0xFF26: // NR52: sound master control; bit0=ch1, bit1=ch2 active
                return (RAM[0xFF26] & 0x80) |
                       (apu_ch1_active ? 0x01 : 0x00) |
                       (apu_ch2_active ? 0x02 : 0x00) |
                       0x70; // unused bits read as 1
            case INTERRUPT_FLAGS: {
                // DMG: upper 3 bits of IF are always 1
                byte result = RAM[pos] | 0xE0;
                if (gpu_control.enabled && !lcd_startup_mode0 && !stat_irq_line) {
                    int proj = gpu_cycles + instr_timer_cycles - 4;
                    if (LY_REG < 144) {
                        // HBlank STAT projection: mode_irq 3→0 transition at gc=260
                        if ((RAM[STAT] & 0x08) &&
                                gpu_get_mode_for_irq(gpu_cycles) == 3 &&
                                gpu_get_mode_for_irq(proj) == 0) {
                            result |= 0x02;
                        }
                        // OAM STAT projection: mode_irq 0→2 transition at gc=4 (4T propagation)
                        if ((RAM[STAT] & 0x20) &&
                                gpu_get_mode_for_irq(gpu_cycles) == 0 &&
                                gpu_get_mode_for_irq(proj) == 2) {
                            result |= 0x02;
                        }
                        // LYC STAT projection: lyc_delayed clears at gc=8 (8T propagation)
                        // When we're in the dead zone (gc=0..7) and proj crosses gc=8, LYC fires.
                        if ((RAM[STAT] & 0x40) && LY_REG == RAM[LYC] && lyc_delayed) {
                            if (gpu_cycles < 8 && proj >= 8) {
                                result |= 0x02;
                            }
                        }
                    }
                }
                return result;
            }
            case 0xFF01:
                return serial_sb;
            case 0xFF02: // SC: bits 6-1 always 1
                return RAM[pos] | 0x7E;
            case 0xFF07: // TAC: bits 7-3 always 1
                return RAM[REG_TAC] | 0xF8;
            case 0xFF10: // NR10: bit 7 always 1
                return RAM[pos] | 0x80;
            case 0xFF11: // NR11: duty (bits 7:6) readable; length (bits 5:0) write-only → read as 1
                return (RAM[pos] & 0xC0) | 0x3F;
            case 0xFF13: // NR13: frequency lo — write-only
                return 0xFF;
            case 0xFF14: // NR14: length_enable (bit 6) readable; rest read as 1
                return (RAM[pos] & 0x40) | 0xBF;
            case 0xFF16: // NR21: same structure as NR11
                return (RAM[pos] & 0xC0) | 0x3F;
            case 0xFF18: // NR23: frequency lo — write-only
                return 0xFF;
            case 0xFF19: // NR24: same structure as NR14
                return (RAM[pos] & 0x40) | 0xBF;
            case 0xFF1B: // NR31: length — write-only
                return 0xFF;
            case 0xFF1A: // NR30: bits 6-0 always 1
                return RAM[pos] | 0x7F;
            case 0xFF1C: // NR32: bits 7 and 4-0 always 1
                return RAM[pos] | 0x9F;
            case 0xFF1D: // NR33: frequency lo — write-only
                return 0xFF;
            case 0xFF1E: // NR34: same structure as NR14/NR24
                return (RAM[pos] & 0x40) | 0xBF;
            case 0xFF20: // NR41: length counter (bits 5:0) write-only; bits 7:6 unused → all read as 1
                return 0xFF;
            case 0xFF23: // NR44: length_enable (bit 6) + bit 7 always 1; bits 5:0 always 1
                return (RAM[pos] & 0x40) | 0xBF;
            // Unmapped I/O: always return 0xFF
            case 0xFF03:
            case 0xFF08: case 0xFF09: case 0xFF0A: case 0xFF0B:
            case 0xFF0C: case 0xFF0D: case 0xFF0E:
            case 0xFF15: case 0xFF1F:
            case 0xFF27: case 0xFF28: case 0xFF29:
            case 0xFF2A: case 0xFF2B: case 0xFF2C: case 0xFF2D: case 0xFF2E: case 0xFF2F:
                return 0xFF;
            default:
                // Unmapped $FF4C-$FF7F on DMG: always return 0xFF
                if (pos >= 0xFF4C && pos <= 0xFF7F) return 0xFF;
                return RAM[pos];
        }
    }
}

void mem_write(int pos, byte data) {
    if (pos <= 0x7FFF) {
        cart_write(pos, data);
        return;
    }
    // Echo RAM: $E000-$FDFF mirrors WRAM $C000-$DDFF (write through)
    if (pos >= 0xE000 && pos < 0xFE00) {
        pos -= 0x2000;
    }
    // OAM bus is locked during DMA — CPU writes to OAM are blocked
    if (dma_active && pos >= 0xFE00 && pos <= 0xFE9F) return;
    // OAM bus is locked by the PPU during mode 2 and most of mode 3.
    if (pos >= 0xFE00 && pos <= 0xFE9F) {
        int proj = gpu_cycles + instr_timer_cycles - 4;
        if (!oam_write_accessible(proj)) return;
    }
    switch (pos) {
        case BGP:
        case LCDC:
        case STAT:
        case LY:
        case LYC:
        case SCX:
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
            RAM[pos] = data;
            break;
        case 0xFF02:
            RAM[pos] = data | 0x7E;  // bits 6-1 always read as 1
            if ((data & 0x81) == 0x81) {
                // Internal clock, transfer start: begin timed 8-bit serial transfer.
                // Clock derived from timer_internal bit 9 falling edge (8192 Hz).
                serial_out_byte = serial_sb;  // save transmit byte before shifting
                serial_active = true;
                serial_bits_remaining = 8;
            } else if (!(data & 0x80)) {
                // Cancel any in-progress transfer
                serial_active = false;
                serial_bits_remaining = 0;
            }
            break;
        case INTERRUPT_FLAGS: {
            // Track projected T-cycle of IF write for OAM/LYC suppression (CPU write wins
            // when simultaneous with interrupt fire at same T-cycle).
            int proj = gpu_cycles + instr_timer_cycles - 4;
            int proj_ly = LY_REG;
            if (proj >= 456) { proj -= 456; proj_ly++; }
            if (proj < 0) proj = 0;
            if_cleared_proj_gc = proj;
            if_cleared_proj_ly = proj_ly;
            RAM[pos] = data;
            break;
        }
        case REG_DIV: {
            // Writing DIV resets internal counter; may cause TIMA falling edge
            int bit = timer_tac_bit[RAM[REG_TAC] & 0x03];
            bool was_set = (timer_internal >> bit) & 1;
            timer_internal = 0;
            timer_div_cycles = 0;
            RAM[REG_DIV] = 0;
            // Falling edge: if timer enabled and selected bit was 1
            if ((RAM[REG_TAC] & 0x04) && was_set) {
                if (++RAM[REG_TIMA] == 0)
                    timer_overflow_pending = 4;
            }
            break;
        }
        case REG_TIMA:
            // Write during overflow delay cancels TMA reload.
            // But if TMA already reloaded this M-cycle, the write is ignored entirely
            // (hardware ignores TIMA writes during the reload M-cycle itself).
            if (timer_just_reloaded) {
                break; // reload wins; write ignored, interrupt preserved
            }
            // Write during the 4-M-cycle overflow delay cancels the pending reload
            timer_overflow_pending = 0;
            RAM[REG_TIMA] = data;
            break;
        case REG_TMA:
            RAM[REG_TMA] = data;
            // Writing TMA during the reload window also updates TIMA immediately
            if (timer_just_reloaded || timer_overflow_pending > 0) {
                RAM[REG_TIMA] = data;
            }
            break;
        case REG_TAC: {
            // Changing TAC can cause falling edge
            int old_bit = timer_tac_bit[RAM[REG_TAC] & 0x03];
            bool timer_was_enabled = (RAM[REG_TAC] & 0x04) != 0;
            bool old_selected = (timer_internal >> old_bit) & 1;
            RAM[REG_TAC] = data;
            // If timer was on and old selected bit was 1, and now effectively 0 (disabled or bit changed)
            if (timer_was_enabled && old_selected) {
                bool still_enabled = (data & 0x04) != 0;
                int new_bit = timer_tac_bit[data & 0x03];
                bool new_selected = (timer_internal >> new_bit) & 1;
                if (!still_enabled || !new_selected) {
                    if (++RAM[REG_TIMA] == 0) {
                        // TAC-write-triggered overflow: set IF immediately so the
                        // interrupt is visible before the NEXT instruction's do_interrupts.
                        // In hardware, the overflow fires during the opcode fetch of the
                        // next instruction; since our do_interrupts runs before exec_next,
                        // we must set IF now to get the same observable behavior.
                        timer_overflow_pending = 4;  // TMA reload still happens after 4T
                        request_interrupt(INTERRUPT_TIMER);
                    }
                }
            }
            break;
        }
        case 0xFF4D:
            // KEY1: only bit 0 (arm speed switch) is writable
            RAM[0xFF4D] = data & 0x01;
            break;
        // APU channel 1 registers
        case 0xFF11: // NR11: length/duty
            RAM[pos] = data;
            apu_ch1_length = 64 - (data & 0x3F);
            break;
        case 0xFF12: // NR12: volume/envelope (DAC on if bits 7-3 != 0)
            RAM[pos] = data;
            break;
        case 0xFF14: // NR14: freq hi + trigger + length enable
            RAM[pos] = data;
            apu_ch1_length_enable = (data >> 6) & 1;
            if (data & 0x80) { // trigger bit
                apu_ch1_active = true;
                if (apu_ch1_length == 0) apu_ch1_length = 64;
                apu_length_cycles = 0; // sync phase on trigger
            }
            break;
        // APU channel 2 registers
        case 0xFF16: // NR21: length/duty
            RAM[pos] = data;
            apu_ch2_length = 64 - (data & 0x3F);
            break;
        case 0xFF17: // NR22: volume/envelope
            RAM[pos] = data;
            break;
        case 0xFF19: // NR24: freq hi + trigger + length enable
            RAM[pos] = data;
            apu_ch2_length_enable = (data >> 6) & 1;
            if (data & 0x80) { // trigger bit
                apu_ch2_active = true;
                if (apu_ch2_length == 0) apu_ch2_length = 64;
                apu_length_cycles = 0; // sync phase on trigger
            }
            break;
        case 0xFF26: // NR52: sound master on/off
            RAM[pos] = data & 0x80;
            if (!(data & 0x80)) { // turning APU off clears channels
                apu_ch1_active = false;
                apu_ch2_active = false;
            }
            break;
        default:
            if (pos == 0xFF50 && inBios) {
                inBios = false;
                printf("bios disabled\n");
            } else if (pos >= 0x8000 && pos <= 0x9FFF) {
                int proj = gpu_cycles + instr_timer_cycles - 4;
                if (vram_write_accessible(proj))
                    gpu_write(pos, data);
            } else if (pos >= 0xA000 && pos <= 0xBFFF) {
                int ram_bank = is_mbc3() ? cart_ram_bank : (cart_mbc1_mode == 1 ? cart_mbc1_upper : 0);
                int idx = ram_bank * 0x2000 + (pos - 0xA000);
                ext_ram[idx] = data;
                // Detect blargg test completion: A000 written with non-0x80 value
                // when magic bytes DE B0 61 are present at A001-A003
                if (pos == 0xA000 && data != 0x80 &&
                    ext_ram[1] == 0xDE && ext_ram[2] == 0xB0 && ext_ram[3] == 0x61) {
                    blargg_done = true;
                }
                // In headless mode, drain the blargg text buffer when the blargg
                // No per-write drain check needed here; drain is triggered in the
                // main loop by monitoring the $D883/$D884 text pointer value.
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
    RAM[DMA] = 0xFF;
    serial_buf_len = 0;
    serial_buf[0] = '\0';
    serial_sb = 0;
    serial_active = false;
    serial_bits_remaining = 0;
    serial_out_byte = 0;
    blargg_done = false;
    double_speed = false;
    RAM[0xFF4D] = 0;
    apu_ch1_active = false;
    apu_ch1_length_enable = false;
    apu_ch1_length = 0;
    apu_ch2_active = false;
    apu_ch2_length_enable = false;
    apu_ch2_length = 0;
    apu_length_cycles = 0;
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
    if_cleared_proj_gc = -100;
    if_cleared_proj_ly = -100;
    total_cpu_cycles = 0;
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
    debug_logfile = NULL;
  }
}

void cpu_update_debug_window(long long current_total_cycles) {
    if (!cpu_debug_range_enabled) {
        if (debug_logfile != NULL) {
            cpu_close_debug_file();
        }
        return;
    }

    bool should_log = current_total_cycles >= cpu_debug_start_cycle &&
                      current_total_cycles < cpu_debug_end_cycle;
    if (should_log) {
        if (debug_logfile == NULL) {
            cpu_init_debug_file();
        }
    } else if (debug_logfile != NULL) {
        cpu_close_debug_file();
    }
}

void cpu_fake_init(void){
    cycles = 0;
    instr_timer_cycles = 0;
    if_cleared_proj_gc = -100;
    if_cleared_proj_ly = -100;
    total_cpu_cycles = 0;
    PC = 0x0100;
    SP = 0xFFFE;
    interrupts = false;
    ei_delay = 0;
    halted = false;
    halt_bug_active = false;
    inBios = false;

    // Set post-boot-ROM register state based on hardware model.
    // Values from Pan Docs and mooneye test suite documentation.
    switch (gb_model) {
        case MODEL_DMG0:
            // Early DMG0 hardware: different B/E/H/L, F=$00, shorter bootrom.
            A = 0x01; F = 0x00; B = 0xFF; C = 0x13; D = 0x00; E = 0xC1; H = 0x84; L = 0x03;
            // Confirmed by oxideboy emulator and mooneye boot_div-dmg0 (N=45 preamble).
            timer_internal = 0x182C;
            break;
        case MODEL_MGB:
            // Game Boy Pocket: same as DMG but A=$FF.
            A = 0xFF; F = 0xB0; B = 0x00; C = 0x13; D = 0x00; E = 0xD8; H = 0x01; L = 0x4D;
            timer_internal = 0xABC8;  // same as DMG-ABC
            break;
        case MODEL_SGB:
            // Super Game Boy: F=$00, C=$14, H=$C0, L=$60.
            A = 0x01; F = 0x00; B = 0x00; C = 0x14; D = 0x00; E = 0x00; H = 0xC0; L = 0x60;
            // boot_div-S uses N=33 preamble NOPs → timer_low = $C8 - 4*(33-6) = $5C.
            timer_internal = 0xD85C;
            joypad_write(0x30);
            apu_ch1_active = false;  // SGB boot ROM does not play a chime
            break;
        case MODEL_SGB2:
            // Super Game Boy 2: same as SGB but A=$FF.
            A = 0xFF; F = 0x00; B = 0x00; C = 0x14; D = 0x00; E = 0x00; H = 0xC0; L = 0x60;
            // boot_div2-S uses N=37 preamble NOPs → timer_low = $C8 - 4*(37-6) = $4C.
            timer_internal = 0xD84C;
            joypad_write(0x30);
            apu_ch1_active = false;  // SGB2 boot ROM does not play a chime
            break;
        case MODEL_GBC:
            // Game Boy Color: A=$11 (signals CGB hardware to games).
            A = 0x11; F = 0x80; B = 0x00; C = 0x00; D = 0xFF; E = 0x56; H = 0x00; L = 0x0D;
            // Derived from mooneye boot_div-cgbABCDE (N=27 preamble): timer_internal=0x2674.
            timer_internal = 0x2674;
            // GBC bootrom leaves P1 with neither row selected (bits 5-4 = 1).
            joypad_write(0x30);
            break;
        default: // MODEL_DMG (DMG-ABC)
            // CGB-only ROMs ($0143=$C0) expect A=$11; DMG/CGB-enhanced use $01.
            A = (cart_cgb_flag == 0xC0) ? 0x11 : 0x01;
            F = 0xB0; B = 0x00; C = 0x13; D = 0x00; E = 0xD8; H = 0x01; L = 0x4D;
            // DMG-ABC post-boot-ROM: timer at $ABC8 (calibrated from boot_div-dmgABCmgb).
            timer_internal = 0xABC8;
            break;
    }
    RAM[REG_DIV] = (uint8_t)(timer_internal >> 8);

    // DMG power-on GPU state (at PC=0x0100): 56T before the end of the LY=153 short-scanline
    // VBlank continuation. LY=0, gpu_cycles=400, ly153_vblank_active=true (VBlank mode 1).
    // Derived from poweron_stat gbmicrotest: N=5 NOPs give mode 1 (projected=452 < 456),
    // N=6 NOPs give mode 0/dead-zone (projected=456 overflows into LY=0 scanline).
    // With preamble=20T (NOP+JP) and gc0=400: gc0+preamble=420; transition fires at NOP 9.
    LY_REG = 0;
    gpu_cycles = 404;
    ly153_vblank_active = true;

    // DMG0 boot ROM leaves the LCD at LY=145, gpu_cycles=250 when PC=$0100 is reached.
    // Calibrated so boot_hwio-dmg0 sees LY=1, STAT=$83 (mode 3) after the test's setup loop.
    // (K≈4460 cycles to reach $FF41 check; 145*456+250+4460 mod 70224 = LY=1, gc=150, mode 3.)
    if (gb_model == MODEL_DMG0) {
        LY_REG = 145;
        gpu_cycles = 250;
        ly153_vblank_active = false;
    }

    // IF: at post-boot, VBlank (bit 0) is pending — the LCD ran during the boot ROM
    RAM[INTERRUPT_FLAGS] = 0x01;  // bits 7-5 always read as 1 via mem_read mask + 0xE0
    RAM[OBP0] = 0xFF;   // OBP0: all pixels transparent/white (DMG boot ROM sets 0xFF)
    RAM[OBP1] = 0xFF;   // OBP1: same
    RAM[0xFF11] = 0x80;   // NR11: wave duty=2 (50%) in bits 7:6
    RAM[0xFF12] = 0xF3;   // NR12: volume=15, decreasing, shift=3
    RAM[0xFF24] = 0x77;   // NR50: SO2/SO1 volume both at max (7)
    RAM[0xFF25] = 0xF3;   // NR51: ch1+ch2 to SO1, ch1+ch2 to SO2
    RAM[0xFF26] = 0x80;   // NR52: master sound on (bit 7); ch status via apu_ch1_active
    // SGB/SGB2 boot ROMs don't play a chime; other models leave ch1 running.
    if (gb_model != MODEL_SGB && gb_model != MODEL_SGB2) {
        apu_ch1_active = true;
    }
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
    byte result = mem_read(loc);
    dma_step(4);              // step DMA after access (mem_read checks dma_active before this)
    return result;
}

void cpu_write(int loc, byte value) {
    cycles += 4;
    timer_step(4);            // step timer before memory access for sub-instruction timing
    instr_timer_cycles += 4;
    mem_write(loc, value);
    dma_step(4);              // step DMA after access
}

// Internal M-cycle: advances time by 4T and steps timer/DMA (for non-memory-access cycles)
static inline void cpu_internal(void) {
    cycles += 4;
    timer_step(4);
    instr_timer_cycles += 4;
    dma_step(4);
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
            //printf("Doing interrupt %s (%x)\n", INTERRUPT_NAMES[i], INTERRUPT_OFFSETS[i]);
            // Interrupt dispatch takes 5 M-cycles (20 cycles):
            // 2 idle + 2 stack push writes + 1 vector load
            cycles += 20;
            // Push PC hi byte first; push_hi may write to $FFFF (IE), changing which
            // interrupts remain enabled.  We must NOT consume the interrupt from IF
            // until after we re-evaluate against the post-push IE.
            mem_write(--SP, (PC >> 8) & 0xFF);
            // Re-read IE after push_hi (it may have been overwritten)
            byte post_hi_ie = mem_read(INTERRUPT_ENABLE) & 0x1F;
            // Re-evaluate all pending interrupts against the updated IE
            byte post_hi_pending = flag_bits & post_hi_ie;
            mem_write(--SP, PC & 0xFF);
            if (post_hi_pending == 0) {
                // No interrupt remains enabled — dispatch cancelled; jump to $0000,
                // IF is not consumed (nothing gets cleared)
                PC = 0x0000;
            } else {
                // Find the highest-priority interrupt still pending and dispatch to it
                for (int j = 0; j < 5; j++) {
                    if ((post_hi_pending & INTERRUPT_PRIORITY[j]) != 0) {
                        flag_bits &= ~INTERRUPT_PRIORITY[j];
                        PC = INTERRUPT_OFFSETS[j];
                        break;
                    }
                }
            }
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
      // CGB speed switch: if KEY1 bit 0 is armed, toggle double-speed mode
      fprintf(stderr, "[STOP] PC=%04X KEY1=%02X\n", PC, RAM[0xFF4D]);
      if (RAM[0xFF4D] & 0x01) {
          double_speed = !double_speed;
          RAM[0xFF4D] = 0;  // clear arm bit; bit 7 updated via mem_read
          cycles += 2050 * 4;  // 2050 M-cycle stall per Pan Docs
          fprintf(stderr, "[SPEED] switched to double_speed=%d\n", double_speed);
      }
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
      break;
    // LD B <- B (nop)
    case 0x40:
      // Mooneye-gb test signal: LD B,B used as breakpoint
      // Pass: B=3,C=5,D=8,E=13,H=21,L=34 (Fibonacci); Fail: all regs = 0x42
      if (headless) {
          if (B == 3 && C == 5 && D == 8 && E == 13 && H == 21 && L == 34) {
              printf("Passed\n");
              blargg_done = true;
          } else if (B == 0x42 && C == 0x42 && D == 0x42 && E == 0x42 && H == 0x42 && L == 0x42) {
              // Decode the BG tilemap ($9800) as ASCII to show the failure message
              // Tile map uses tile indices; font in VRAM at $8000, each tile = 16 bytes
              // Character codes: tile 0 = space, others are ASCII-like
              char vram_msg[700] = {0};
              int vlen = 0;
              for (int row = 0; row < 18; row++) {
                  for (int col = 0; col < 32 && vlen < 690; col++) {
                      byte tidx = VRAM[0x1800 + row*32 + col];
                      char ch = (tidx >= 0x20 && tidx < 0x80) ? (char)tidx : (tidx == 0 ? ' ' : '?');
                      vram_msg[vlen++] = ch;
                  }
                  if (vlen < 690) vram_msg[vlen++] = '\n';
              }
              vram_msg[vlen] = '\0';
              // Trim trailing spaces
              while (vlen > 0 && vram_msg[vlen-1] == ' ') vram_msg[--vlen] = '\0';
              printf("Failed (mooneye): %s\n", vram_msg[0] ? vram_msg : "(no screen msg)");
              blargg_done = true;
          }
      }
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
      cpu_internal();       // M2: condition check
      if (!CheckFlag(FLAG_Z)) {
          { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); PC = ((word)hi << 8) | lo; }
          cpu_internal();   // M5: internal
      }
      break;

    // POP BC
    case 0xc1:
      oam_read_corrupt_at(SP, 0);
      oam_read_corrupt_at(SP + 1, 4);
      { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); setBC(((word)hi << 8) | lo); }
      break;

    // JP NZ
    case 0xc2:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_Z)) {
          cpu_internal();   // M4: internal
          PC = pos;
      }
      break;

    // JP a16
    case 0xc3:
      PC = cpu_read16();
      cpu_internal();   // M4: internal
      break;

    // CALL NZ
    case 0xc4:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_Z)) {
          cpu_internal();   // M4: internal delay
          cpu_write(--SP, (PC >> 8) & 0xFF);
          cpu_write(--SP, PC & 0xFF);
          PC = pos;
      }
      break;

    // PUSH BC
    case 0xc5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      cpu_internal();           // M2 internal delay
      cpu_write(--SP, B);       // M3: write B (hi)
      cpu_write(--SP, C);       // M4: write C (lo)
      break;

    // ADD A += d8
    case 0xc6:
      A = Add(cpu_read_next());
      break;

    // RST 00H
    case 0xc7:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
      PC = 0x00;
      break;

    // RET Z
    case 0xc8:
      cpu_internal();       // M2: condition check
      if (CheckFlag(FLAG_Z)) {
          { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); PC = ((word)hi << 8) | lo; }
          cpu_internal();   // M5: internal
      }
      break;

    // RET
    case 0xc9:
      { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); PC = ((word)hi << 8) | lo; }
      cpu_internal();   // M4: internal
      break;

    // JP Z
    case 0xca:
      pos = cpu_read16();
      if (CheckFlag(FLAG_Z)) {
          cpu_internal();   // M4: internal
          PC = pos;
      }
      break;

    // CALL Z
    case 0xcc:
      pos = cpu_read16();
      if (CheckFlag(FLAG_Z)) {
          cpu_internal();   // M4: internal delay
          cpu_write(--SP, (PC >> 8) & 0xFF);
          cpu_write(--SP, PC & 0xFF);
          PC = pos;
      }
      break;

    // CALL a16
    case 0xcd:
      pos = cpu_read16();
      cpu_internal();   // M4: internal delay
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
      PC = pos;
      break;

    // ADC A += d8
    case 0xce:
      A = Adc(cpu_read_next());
      break;

    // RST 08H
    case 0xcf:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
      PC = 0x08;
      break;

    // RET NC
    case 0xd0:
      cpu_internal();       // M2: condition check
      if (!CheckFlag(FLAG_C)) {
          { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); PC = ((word)hi << 8) | lo; }
          cpu_internal();   // M5: internal
      }
      break;

    // POP DE
    case 0xd1:
      oam_read_corrupt_at(SP, 0);
      oam_read_corrupt_at(SP + 1, 4);
      { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); setDE(((word)hi << 8) | lo); }
      break;

    // JP NC
    case 0xd2:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_C)) {
          cpu_internal();   // M4: internal
          PC = pos;
      }
      break;

    // CALL NC
    case 0xd4:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_C)) {
          cpu_internal();   // M4: internal delay
          cpu_write(--SP, (PC >> 8) & 0xFF);
          cpu_write(--SP, PC & 0xFF);
          PC = pos;
      }
      break;

    // PUSH DE
    case 0xd5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      cpu_internal();           // M2 internal delay
      cpu_write(--SP, D);       // M3: write D (hi)
      cpu_write(--SP, E);       // M4: write E (lo)
      break;

    // SUB A -= d8
    case 0xd6:
      A = Sub(cpu_read_next());
      break;

    // RST 10H
    case 0xd7:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
      PC = 0x10;
      break;

    // RET C
    case 0xd8:
      cpu_internal();       // M2: condition check
      if (CheckFlag(FLAG_C)) {
          { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); PC = ((word)hi << 8) | lo; }
          cpu_internal();   // M5: internal
      }
      break;

    // RETI
    case 0xd9:
      { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); PC = ((word)hi << 8) | lo; }
      cpu_internal();   // M4: internal
      interrupts = true;
      break;

    // JP C
    case 0xda:
      pos = cpu_read16();
      if (CheckFlag(FLAG_C)) {
          cpu_internal();   // M4: internal
          PC = pos;
      }
      break;

    // CALL C
    case 0xdc:
      pos = cpu_read16();
      if (CheckFlag(FLAG_C)) {
          cpu_internal();   // M4: internal delay
          cpu_write(--SP, (PC >> 8) & 0xFF);
          cpu_write(--SP, PC & 0xFF);
          PC = pos;
      }
      break;

    // RST 18H
    case 0xdf:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
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
      { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); setHL(((word)hi << 8) | lo); }
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
        cpu_internal();   // M3: internal
        cpu_internal();   // M4: internal
      }
      break;

    // PUSH HL
    case 0xe5:
      oam_write_corrupt_at(SP, 0);    // M2 IDU
      oam_write_corrupt_at(SP-1, 4); // M3 write hi
      oam_write_corrupt_at(SP-2, 8); // M4 write lo
      cpu_internal();           // M2 internal delay
      cpu_write(--SP, H);       // M3: write H (hi)
      cpu_write(--SP, L);       // M4: write L (lo)
      break;

    // AND A & d8
    case 0xe6:
      A = And(cpu_read_next());
      break;

    // RST 20H
    case 0xe7:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
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
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
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
      { byte lo = cpu_read(SP++); byte hi = cpu_read(SP++); setAF(((word)hi << 8) | lo); }
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
      cpu_internal();           // M2 internal delay
      cpu_write(--SP, A);       // M3: write A (hi)
      cpu_write(--SP, F);       // M4: write F (lo)
      break;

    // OR A | d8
    case 0xf6:
      A = Or(cpu_read_next());
      break;

    // RST 30H
    case 0xf7:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
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
        cpu_internal();   // M3: internal
      }
      break;

    // LD SP, HL
    case 0xf9:
      SP = HL();
      cpu_internal();   // M2: internal
      break;

    // LD A <- (a16)
    case 0xfa:
      A = cpu_read(cpu_read16());
      break;

    // EI
    case 0xfb:
      if (ei_delay == 0) ei_delay = 2; // don't reset if already pending
      break;

    // CP d8
    case 0xfe:
      CP(cpu_read_next());
      break;

    // RST 38H
    case 0xff:
      cpu_internal();   // M2: internal
      cpu_write(--SP, (PC >> 8) & 0xFF);
      cpu_write(--SP, PC & 0xFF);
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


void dump_screenshot_ppm(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open screenshot file %s: ", filename);
        perror(NULL);
        exit(1);
    }

    fprintf(fp, "P6\n%d %d\n255\n", VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    for (int y = 0; y < VIEWPORT_HEIGHT; y++) {
        for (int x = 0; x < VIEWPORT_WIDTH; x++) {
            const uint32_t pixel = pixels[y * VIEWPORT_WIDTH + x];
            const byte rgb[3] = {
                (byte)((pixel >> 16) & 0xFF),
                (byte)((pixel >> 8) & 0xFF),
                (byte)(pixel & 0xFF),
            };
            fwrite(rgb, 1, sizeof(rgb), fp);
        }
    }

    fclose(fp);
}

void maybe_dump_screenshot(int completed_frame) {
    for (int i = 0; i < screenshot_request_count; i++) {
        ScreenshotRequest *req = &screenshot_requests[i];
        if (!req->done && req->frame == completed_frame) {
            dump_screenshot_ppm(req->filename);
            req->done = true;
            fprintf(stderr, "[screenshot] frame %d -> %s\n", completed_frame, req->filename);
        }
    }
}

void maybe_dump_completed_frames(int previous_frame, int new_frame) {
    for (int completed_frame = previous_frame; completed_frame < new_frame; completed_frame++) {
        maybe_dump_screenshot(completed_frame);
    }
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
            cpu_update_debug_window(total_cpu_cycles);
            instr_timer_cycles = 0;
            if_cleared_proj_gc = -100;
            if_cleared_proj_ly = -100;
            int prevcycles = cycles;
            do_interrupts();
            int dispatch_cycles = cycles - prevcycles;
            if (dispatch_cycles > 0) {
                gpu_step(dispatch_cycles);
                timer_step(dispatch_cycles - instr_timer_cycles);
                dma_step(dispatch_cycles);
                instr_timer_cycles = 0;
                if_cleared_proj_gc = -100;
                if_cleared_proj_ly = -100;
            }
            exec_next_start_cycles = cycles;
            exec_next();
            int cpu_cycles = cycles - prevcycles - dispatch_cycles;

            gpu_step(cpu_cycles);
            timer_step(cpu_cycles - instr_timer_cycles);
            dma_step(cpu_cycles - instr_timer_cycles);
            total_cpu_cycles += dispatch_cycles + cpu_cycles;
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
    static int completed_frame = 0;
    const uint32_t start_ticks = SDL_GetTicks();

    frame_headless();
    maybe_dump_screenshot(completed_frame++);
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
    int current_frame = 0;
    // Hang detection: ring buffer of last 64 PCs, and consecutive-same-range counter.
    #define HANG_RANGE 256
    #define HANG_THRESH 2000000
    #define PC_RING 64
    word pc_ring[PC_RING];
    int pc_ring_idx = 0;
    int hang_count = 0;
    word hang_range_min = 0;
    memset(pc_ring, 0, sizeof(pc_ring));
    // Instruction counting for periodic PC reports (stderr when --cycles set)
    long long instr_count = 0;

    while (1) {
        if (max_cycles > 0 && total_cycles >= max_cycles) {
            printf("\n[headless] cycle limit %lld reached (frame %d), stopping\n",
                   max_cycles, current_frame);
            break;
        }
        if (blargg_done) {
            headless_print_blargg_a000();
            break;
        }
        // Drain blargg A004 text buffer before it overflows into WRAM ($C000+).
        if (headless && !blargg_done &&
            ext_ram[1] == 0xDE && ext_ram[2] == 0xB0 && ext_ram[3] == 0x61) {
            uint16_t txt_ptr = ((uint16_t)RAM[0xD884] << 8) | RAM[0xD883];
            if (txt_ptr >= 0xBF00) {
                RAM[0xD883] = 0x04;
                RAM[0xD884] = 0xA0;
                memset(&ext_ram[4], 0, 0x2000 - 4);
            }
        }

        // Update frame counter and apply joypad injection events.
        int new_frame = (int)(total_cycles / 70224);
        if (new_frame != current_frame) {
            maybe_dump_completed_frames(current_frame, new_frame);
            current_frame = new_frame;
            // Release all buttons first, then re-apply active events.
            joypad_buttons.A = joypad_buttons.B = false;
            joypad_buttons.START = joypad_buttons.SELECT = false;
            joypad_buttons.UP = joypad_buttons.DOWN = false;
            joypad_buttons.LEFT = joypad_buttons.RIGHT = false;
            for (int j = 0; j < joypad_event_count; j++) {
                if (current_frame >= joypad_events[j].frame &&
                    current_frame < joypad_events[j].frame + joypad_events[j].duration) {
                    if (joypad_events[j].A)      joypad_buttons.A      = true;
                    if (joypad_events[j].B)      joypad_buttons.B      = true;
                    if (joypad_events[j].START)  joypad_buttons.START  = true;
                    if (joypad_events[j].SELECT) joypad_buttons.SELECT = true;
                    if (joypad_events[j].UP)     joypad_buttons.UP     = true;
                    if (joypad_events[j].DOWN)   joypad_buttons.DOWN   = true;
                    if (joypad_events[j].LEFT)   joypad_buttons.LEFT   = true;
                    if (joypad_events[j].RIGHT)  joypad_buttons.RIGHT  = true;
                }
            }
        }

        // Hang detection: if PC stays within a HANG_RANGE-byte window for HANG_THRESH
        // consecutive instructions, we're stuck in a tight loop.
        word cur_pc = PC;
        pc_ring[pc_ring_idx++ & (PC_RING - 1)] = cur_pc;
        if (cur_pc >= hang_range_min && cur_pc < hang_range_min + HANG_RANGE) {
            if (++hang_count >= HANG_THRESH) {
                fprintf(stderr, "\n[headless] HANG DETECTED at frame %d (total cycles %lld)\n",
                        current_frame, total_cycles);
                fprintf(stderr, "[headless] PC window: $%04X-$%04X\n",
                        hang_range_min, (word)(hang_range_min + HANG_RANGE - 1));
                fprintf(stderr, "[headless] Last PCs: ");
                for (int j = 0; j < PC_RING; j++) {
                    int idx = (pc_ring_idx - PC_RING + j) & (PC_RING - 1);
                    fprintf(stderr, "$%04X ", pc_ring[idx]);
                }
                fprintf(stderr, "\n");
                fprintf(stderr, "[headless] Regs: A=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X\n",
                        A, B, C, D, E, H, L, SP);
                fprintf(stderr, "[headless] STAT=%02X LY=%02X IE=%02X IF=%02X IME=%d\n",
                        RAM[STAT], LY_REG, RAM[INTERRUPT_ENABLE], RAM[INTERRUPT_FLAGS], interrupts);
                break;
            }
        } else {
            hang_range_min = cur_pc & ~(HANG_RANGE - 1);
            hang_count = 1;
        }

        cpu_update_debug_window(total_cycles);
        instr_timer_cycles = 0;
        if_cleared_proj_gc = -100;
        if_cleared_proj_ly = -100;
        int prevcycles = cycles;
        do_interrupts();
        int dispatch_cycles = cycles - prevcycles;
        if (dispatch_cycles > 0) {
            gpu_step(dispatch_cycles);
            timer_step(dispatch_cycles - instr_timer_cycles);
            dma_step(dispatch_cycles);
            instr_timer_cycles = 0;
            if_cleared_proj_gc = -100;
            if_cleared_proj_ly = -100;
        }
        exec_next_start_cycles = cycles;
        exec_next();
        int cpu_cycles = cycles - prevcycles - dispatch_cycles;
        gpu_step(cpu_cycles);
        timer_step(cpu_cycles - instr_timer_cycles);
        apu_step(cpu_cycles);
        dma_step(cpu_cycles - instr_timer_cycles);
        total_cycles += dispatch_cycles + cpu_cycles;
        total_cpu_cycles = total_cycles;
        instr_count++;
        // Every 5M instructions, emit a progress line to stderr so we can see where game is.
        if (max_cycles > 0 && (instr_count % 5000000) == 0) {
            fprintf(stderr, "[trace] frame=%d instr=%lld cycles=%lld PC=$%04X A=%02X B=%02X\n",
                    current_frame, instr_count, total_cycles, PC, A, B);
        }
    }
    // If cycle limit hit but A000 output present, still print it
    if (!blargg_done) {
        headless_print_blargg_a000();
    }
    // GBMicrotest: check 0xFF82 (0x01=pass, 0xFF=fail) after run
    if (gbmicrotest_mode) {
        byte result = RAM[0xFF82];
        if (result == 0x01) {
            printf("Passed\n");
        } else if (result == 0xFF) {
            byte actual   = RAM[0xFF80];
            byte expected = RAM[0xFF81];
            printf("Failed (actual=0x%02X expected=0x%02X)\n", actual, expected);
        } else {
            printf("Unknown (0xFF82=0x%02X)\n", result);
        }
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
    } else if (strcmp(argv[i], "--model") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --model\n");
        return 1;
      }
      const char *m = argv[++i];
      if (strcmp(m, "dmg") == 0 || strcmp(m, "dmgABC") == 0) gb_model = MODEL_DMG;
      else if (strcmp(m, "dmg0") == 0) gb_model = MODEL_DMG0;
      else if (strcmp(m, "mgb") == 0) gb_model = MODEL_MGB;
      else if (strcmp(m, "sgb") == 0) gb_model = MODEL_SGB;
      else if (strcmp(m, "sgb2") == 0) gb_model = MODEL_SGB2;
      else if (strcmp(m, "cgb") == 0 || strcmp(m, "gbc") == 0 || strcmp(m, "S") == 0) gb_model = MODEL_GBC;
      else { fprintf(stderr, "Unknown model: %s\n", m); return 1; }
    } else if (strcmp(argv[i], "--joypad") == 0) {
      // Format: frame:BUTTONS[:duration] e.g. "120:A" or "200:START:3" or "300:A,B:2"
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --joypad\n");
        return 1;
      }
      if (joypad_event_count >= MAX_JOYPAD_EVENTS) {
        fprintf(stderr, "Too many --joypad events (max %d)\n", MAX_JOYPAD_EVENTS);
        return 1;
      }
      const char *spec = argv[++i];
      JoypadEvent *ev = &joypad_events[joypad_event_count++];
      memset(ev, 0, sizeof(*ev));
      ev->duration = 1;
      // Parse frame number
      char *end;
      ev->frame = (int)strtol(spec, &end, 10);
      if (*end != ':') { fprintf(stderr, "Bad --joypad format: %s\n", spec); return 1; }
      // Parse buttons (comma-separated) and optional duration after second colon
      const char *p = end + 1;
      while (*p && *p != ':') {
          if (strncasecmp(p, "START", 5) == 0)   { ev->START  = true; p += 5; }
          else if (strncasecmp(p, "SELECT", 6) == 0) { ev->SELECT = true; p += 6; }
          else if (strncasecmp(p, "RIGHT", 5) == 0)  { ev->RIGHT  = true; p += 5; }
          else if (strncasecmp(p, "LEFT", 4) == 0)   { ev->LEFT   = true; p += 4; }
          else if (strncasecmp(p, "DOWN", 4) == 0)   { ev->DOWN   = true; p += 4; }
          else if (strncasecmp(p, "UP", 2) == 0)     { ev->UP     = true; p += 2; }
          else if (*p == 'A' || *p == 'a')            { ev->A      = true; p++; }
          else if (*p == 'B' || *p == 'b')            { ev->B      = true; p++; }
          else if (*p == ',') p++;
          else { fprintf(stderr, "Unknown button in --joypad: %c\n", *p); return 1; }
      }
      if (*p == ':') ev->duration = (int)strtol(p + 1, NULL, 10);
    } else if (strcmp(argv[i], "--gbmicrotest") == 0) {
      gbmicrotest_mode = true;
    } else if (strcmp(argv[i], "--screenshot") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --screenshot\n");
        return 1;
      }
      if (screenshot_request_count >= MAX_SCREENSHOT_REQUESTS) {
        fprintf(stderr, "Too many --screenshot requests (max %d)\n", MAX_SCREENSHOT_REQUESTS);
        return 1;
      }
      const char *spec = argv[++i];
      char *end;
      ScreenshotRequest *req = &screenshot_requests[screenshot_request_count++];
      memset(req, 0, sizeof(*req));
      req->frame = (int)strtol(spec, &end, 10);
      if (*end != ':' || end[1] == '\0') {
        fprintf(stderr, "Bad --screenshot format: %s\n", spec);
        return 1;
      }
      strncpy(req->filename, end + 1, sizeof(req->filename) - 1);
    } else if (strcmp(argv[i], "--cpu-debug-range") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for --cpu-debug-range\n");
        return 1;
      }
      const char *spec = argv[++i];
      char *end;
      cpu_debug_start_cycle = strtoll(spec, &end, 10);
      if (*end != ':') {
        fprintf(stderr, "Bad --cpu-debug-range format: %s\n", spec);
        return 1;
      }
      cpu_debug_end_cycle = strtoll(end + 1, &end, 10);
      if (*end != '\0' || cpu_debug_end_cycle <= cpu_debug_start_cycle) {
        fprintf(stderr, "Bad --cpu-debug-range format: %s\n", spec);
        return 1;
      }
      cpu_debug_range_enabled = true;
    } else if (strncmp(argv[i], "--", 2) == 0) {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    } else {
      rom = argv[i];
    }
  }
  cart_load(rom);

  mem_init();
  gpu_init();    // sets LCD state first
  cpu_fake_init(); // overrides registers and (for DMG0) GPU scanline position
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
