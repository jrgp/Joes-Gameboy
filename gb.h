#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

/* Set by SIGINT/SIGTERM handler; frontends check this to exit their loops. */
extern volatile sig_atomic_t g_shutdown_requested;

/* Pixel buffer */
extern uint32_t *pixels;
void pixels_init(void);

/* Runtime state */
extern bool headless;
extern bool g_fast_mode;
extern long long max_cycles;
extern bool gbmicrotest_mode;
extern char savestate_rom_path[4096];

/* Battery RAM */
extern uint8_t ext_ram[0x8000];
extern int cart_ram_size;
extern bool ext_ram_dirty;

/* Emulator lifecycle */
void cart_load(char *path);
void mem_init(void);
void gpu_init(void);
void cpu_fake_init(void);
void joypad_init(void);

/* Battery save */
void sav_load(const char *rom_path);
void sav_save(const char *rom_path);

/* Model selection: name is "dmg","dmg0","mgb","sgb","sgb2","cgb"/"gbc"
 * Returns true on success. */
bool gb_set_model(const char *name);

/* Per-frame step (core step — no SDL, no timing) */
bool frame_headless(void);

/* Headless test runner (blargg/cycle-limited) */
void headless_main_impl(void);
void headless_print_blargg_a000(void);
void gb_dump_state(void);

/* Joypad: bit 0=RIGHT,1=LEFT,2=UP,3=DOWN,4=A,5=B,6=SELECT,7=START */
void gb_set_button(int bit, bool pressed);

/* Soft reset */
void gb_reset(void);
bool gb_is_cgb_mode(void);
