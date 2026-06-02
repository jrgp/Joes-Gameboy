/*
 * tests/test_savestate.c — Round-trip tests for save/load state.
 *
 * Compiled as a standalone binary that links gb.c + savestate.c.
 * Uses the same headless init path as the emulator.
 *
 * Build:
 *   gcc -I.. $(pkg-config --cflags libcbor) -Wall -Wextra -g \
 *       test_savestate.c ../savestate.c ../gb.c \
 *       -o test_savestate_bin \
 *       $(pkg-config --libs libcbor)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "../savestate.h"

/* ---- extern declarations for emulator globals we need to inspect/mutate ---- */
typedef uint8_t  byte;
typedef uint16_t word;

extern byte  A, F, B, C, D, E, H, L;
extern word  PC, SP;
extern bool  interrupts;
extern bool  halted;
extern int   cycles;
extern int   gpu_cycles;
extern byte  LY_REG;
extern byte  LCDC_REG;
extern byte  RAM[];
extern byte  VRAM[];

/* Timer accessors */
extern uint16_t ss_get_timer_internal(void);
extern void     ss_set_timer_internal(uint16_t v);

/* Emulator init functions */
extern void cart_load(char *path);
extern void mem_init(void);
extern void gpu_init(void);
extern void cpu_fake_init(void);
extern void joypad_init(void);
extern void pixels_init(void);
extern bool frame_headless(void);

/* headless flag */
extern bool headless;

/* ---- tiny test framework ---- */

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        tests_failed++; \
    } else { \
        printf("PASS %s\n", msg); \
    } \
} while (0)

#define CHECK_EQ_U(a, b, msg) do { \
    tests_run++; \
    if ((unsigned long long)(a) != (unsigned long long)(b)) { \
        fprintf(stderr, "FAIL [%s:%d] %s: got %llu expected %llu\n", \
                __FILE__, __LINE__, msg, \
                (unsigned long long)(a), (unsigned long long)(b)); \
        tests_failed++; \
    } else { \
        printf("PASS %s\n", msg); \
    } \
} while (0)

/* ROM path — blargg 01-special.gb is a small ROM available from the test suite */
#define TEST_ROM "roms/blargg/01-special.gb"
#define TMP_STATE "/tmp/cgb_test_savestate.cbor"
#define TMP_STATE2 "/tmp/cgb_test_savestate2.cbor"

static void init_emulator(void) {
    headless = true;
    cart_load(TEST_ROM);
    mem_init();
    gpu_init();
    cpu_fake_init();
    pixels_init();
    joypad_init();
}

/* ---- Tests ---- */

static void test_save_and_load_basic(void) {
    printf("\n--- test_save_and_load_basic ---\n");

    init_emulator();

    /* Run some frames to reach non-trivial state */
    for (int i = 0; i < 60; i++) frame_headless();

    /* Snapshot state before save */
    byte snap_A    = A;
    byte snap_F    = F;
    word snap_PC   = PC;
    word snap_SP   = SP;
    int  snap_gpu  = gpu_cycles;
    byte snap_LY   = LY_REG;
    uint16_t snap_timer = ss_get_timer_internal();
    byte snap_ram_FF00 = RAM[0xFF00];
    byte snap_vram_00  = VRAM[0];
    byte snap_vram_100 = VRAM[0x100];

    bool saved = save_state(TMP_STATE);
    CHECK(saved, "save_state returns true");

    /* Mutate state aggressively */
    A   = 0xAB;
    F   = 0xCD;
    PC  = 0x1234;
    SP  = 0x5678;
    gpu_cycles = 9999;
    LY_REG = 0x55;
    ss_set_timer_internal(0xDEAD);
    RAM[0xFF00] = 0x42;
    VRAM[0]     = 0x99;
    VRAM[0x100] = 0x77;

    /* Load */
    bool loaded = load_state(TMP_STATE);
    CHECK(loaded, "load_state returns true");

    /* Verify CPU registers restored */
    CHECK_EQ_U(A,  snap_A,  "A register restored");
    CHECK_EQ_U(F,  snap_F,  "F register restored");
    CHECK_EQ_U(PC, snap_PC, "PC restored");
    CHECK_EQ_U(SP, snap_SP, "SP restored");

    /* PPU */
    CHECK_EQ_U(gpu_cycles, snap_gpu, "gpu_cycles restored");
    CHECK_EQ_U(LY_REG,     snap_LY,  "LY_REG restored");

    /* Timer */
    CHECK_EQ_U(ss_get_timer_internal(), snap_timer, "timer_internal restored");

    /* Memory spot-checks */
    CHECK_EQ_U(RAM[0xFF00],  snap_ram_FF00,  "RAM[0xFF00] restored");
    CHECK_EQ_U(VRAM[0],      snap_vram_00,   "VRAM[0] restored");
    CHECK_EQ_U(VRAM[0x100],  snap_vram_100,  "VRAM[0x100] restored");
}

static void test_deterministic_round_trip(void) {
    printf("\n--- test_deterministic_round_trip ---\n");

    init_emulator();
    for (int i = 0; i < 30; i++) frame_headless();

    /* Round trip: save → load → save → load */
    CHECK(save_state(TMP_STATE),  "round-trip save1 succeeds");

    /*
     * Snapshot key fields AFTER save_state — save_state canonicalizes some
     * dead RAM slots (like RAM[0xFF44] for LY) so we compare against the
     * canonical saved values, not the pre-save raw slots.
     */
    byte s_A  = A;  byte s_F  = F;
    word s_PC = PC; word s_SP = SP;
    int  s_gpu = gpu_cycles;
    byte s_LY  = LY_REG;
    uint16_t s_timer = ss_get_timer_internal();
    byte s_r0 = RAM[0x8000];
    byte s_r1 = RAM[0xC100];
    byte s_r2 = RAM[0xFF44]; /* LY — now canonical */

    CHECK(load_state(TMP_STATE),  "round-trip load1 succeeds");

    /* Verify state matches snapshot after first load */
    CHECK_EQ_U(A,  s_A,  "A after load1");
    CHECK_EQ_U(F,  s_F,  "F after load1");
    CHECK_EQ_U(PC, s_PC, "PC after load1");
    CHECK_EQ_U(SP, s_SP, "SP after load1");
    CHECK_EQ_U(gpu_cycles, s_gpu, "gpu_cycles after load1");
    CHECK_EQ_U(LY_REG,     s_LY,  "LY_REG after load1");
    CHECK_EQ_U(ss_get_timer_internal(), s_timer, "timer after load1");
    CHECK_EQ_U(RAM[0x8000], s_r0, "RAM[0x8000] after load1");
    CHECK_EQ_U(RAM[0xC100], s_r1, "RAM[0xC100] after load1");
    CHECK_EQ_U(RAM[0xFF44], s_r2, "RAM[0xFF44] after load1");

    /* Second save/load should be idempotent */
    CHECK(save_state(TMP_STATE2), "round-trip save2 succeeds");
    CHECK(load_state(TMP_STATE2), "round-trip load2 succeeds");

    CHECK_EQ_U(A,  s_A,  "A after load2");
    CHECK_EQ_U(PC, s_PC, "PC after load2");
    CHECK_EQ_U(ss_get_timer_internal(), s_timer, "timer after load2");
}

static void test_load_invalid_path(void) {
    printf("\n--- test_load_invalid_path ---\n");
    bool ok = load_state("/tmp/nonexistent_cbor_file_12345.cbor");
    CHECK(!ok, "load_state returns false for missing file");
}

static void test_cbor_path_detection(void) {
    printf("\n--- test_cbor_path_detection ---\n");
    CHECK( savestate_is_cbor_path("foo.cbor"),      "foo.cbor detected");
    CHECK( savestate_is_cbor_path("/path/bar.cbor"), "/path/bar.cbor detected");
    CHECK(!savestate_is_cbor_path("foo.gb"),         "foo.gb not detected");
    CHECK(!savestate_is_cbor_path("foo.cbor.bak"),   "foo.cbor.bak not detected");
    CHECK(!savestate_is_cbor_path("cbor"),           "bare 'cbor' not detected");
}

static void test_default_path(void) {
    printf("\n--- test_default_path ---\n");
    char out[256];
    savestate_default_path("roms/pokemon.gb", out, sizeof(out));
    CHECK(strcmp(out, "roms/pokemon.cbor") == 0, "default path strips .gb, appends .cbor");

    savestate_default_path("/abs/path/game.rom", out, sizeof(out));
    CHECK(strcmp(out, "/abs/path/game.cbor") == 0, "default path works for .rom");

    savestate_default_path("noext", out, sizeof(out));
    CHECK(strcmp(out, "noext.cbor") == 0, "default path works for no-extension");
}

int main(void) {
    test_cbor_path_detection();
    test_default_path();
    test_load_invalid_path();
    test_save_and_load_basic();
    test_deterministic_round_trip();

    printf("\n=== Results: %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed == 0 ? 0 : 1;
}
