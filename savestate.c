/*
 * savestate.c — CBOR-based save/load state for the cgb emulator.
 *
 * Schema (CBOR map, string keys):
 *   "ver"          : uint   format version (SAVESTATE_FORMAT_VERSION)
 *   "rom_path"     : string path of the ROM file used when saving
 *   --- CPU ---
 *   "pc"           : uint16
 *   "sp"           : uint16
 *   "a","f","b","c","d","e","h","l" : uint8 registers
 *   "ime"          : bool   interrupt master enable
 *   "ei_delay"     : int    EI delay counter (0, 1, or 2)
 *   "halted"       : bool
 *   "halt_bug"     : bool
 *   "cycles"       : int    frame-local cycle counter
 *   --- PPU ---
 *   "gpu_cycles"   : int
 *   "ly"           : uint8  LY_REG
 *   "real_ly"      : uint8
 *   "scx"          : uint8
 *   "scy"          : uint8
 *   "lcdc"         : uint8
 *   "bgp"          : uint8
 *   "stat_irq"     : bool
 *   "lcd_off_lyc"  : bool
 *   "startup_m0"   : bool
 *   "startup_ln"   : bool
 *   "vblank_pend"  : bool
 *   "mode3_extra"  : int
 *   "scx_last"     : uint8
 *   "win_line"     : int
 *   "if_proj_gc"   : int
 *   "if_proj_ly"   : int
 *   "vram"         : bytes  [0x2000]
 *   --- Timer ---
 *   "timer_int"    : uint16 timer_internal
 *   "tima_ovf"     : int    timer_overflow_pending
 *   "tima_reload"  : bool   timer_just_reloaded
 *   "tima_halt"    : bool   timer_halt_delay
 *   --- DMA ---
 *   "dma_active"   : bool
 *   "dma_locked"   : bool
 *   "dma_remain"   : int    dma_cycles_remaining
 *   "dma_startup"  : int    dma_startup_remaining
 *   "dma_src"      : int    dma_source
 *   --- Memory ---
 *   "ram"          : bytes  [0x10000]
 *   "ext_ram"      : bytes  [0x8000]
 *   --- Cartridge ---
 *   "cart_type"    : uint8
 *   "rom_banks"    : int
 *   "rom_bank"     : int
 *   "mbc1_upper"   : int
 *   "mbc1_mode"    : int
 *   "ram_bank"     : int
 *   "model"        : int    gb_model enum value
 *   --- Serial ---
 *   "ser_sb"       : uint8
 *   "ser_active"   : bool
 *   "ser_bits"     : int
 *   "ser_out"      : uint8
 *   --- APU ---
 *   "apu_ch1_on"   : bool
 *   "apu_ch1_len_en": bool
 *   "apu_ch1_len"  : int
 *   "apu_ch2_on"   : bool
 *   "apu_ch2_len_en": bool
 *   "apu_ch2_len"  : int
 *   "apu_cyc"      : int
 *   --- Misc ---
 *   "dbl_spd"      : bool
 *   "joypad"       : uint8
 *   "in_bios"      : bool
 */

#include "savestate.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>

typedef uint8_t byte;
typedef uint16_t word;

/* ---- extern declarations for all emulator globals ---- */

/* GPU */
extern int   gpu_cycles;
extern byte  LY_REG;
extern byte  real_ly;
extern byte  SCX_REG;
extern byte  SCY_REG;
extern byte  LCDC_REG;
extern byte  BGP_REG;
extern byte  VRAM[0x2000];

/* GPU internals (static in gb.c — accessed via helper accessors below) */
/* We export these via save/load accessor pairs defined near them in gb.c */

/* CPU */
extern byte F, A, C, B, E, D, L, H;
extern word PC, SP;
extern bool interrupts;
extern int  ei_delay;
extern bool halted;
extern bool halt_bug_active;
extern int  cycles;

/* Memory */
extern byte RAM[0xffff + 1];

/* Cartridge */
extern byte  cart_type;
extern int   cart_rom_banks;
extern int   cart_rom_bank;
extern int   cart_mbc1_upper;
extern int   cart_mbc1_mode;
extern int   cart_ram_bank;
extern byte  ext_ram[0x8000];

/* Serial */
extern byte serial_sb;

/* APU */
extern bool apu_ch1_active;
extern bool apu_ch1_length_enable;
extern int  apu_ch1_length;
extern bool apu_ch2_active;
extern bool apu_ch2_length_enable;
extern int  apu_ch2_length;
extern int  apu_length_cycles;

/* Misc */
extern bool double_speed;
extern byte joypad;
extern bool inBios;
extern bool dma_active;
extern bool dma_oam_locked;
extern int  dma_cycles_remaining;
extern int  dma_startup_remaining;
extern int  dma_source;

/* ROM path (set by cart_load, accessed here) */
extern char savestate_rom_path[4096];

/*
 * The following state lives in static variables inside gb.c.
 * We expose them through small accessor/mutator pairs defined in gb.c.
 */
extern bool ss_get_stat_irq_line(void);
extern bool ss_get_lcd_off_lyc_flag(void);
extern bool ss_get_lcd_startup_mode0(void);
extern bool ss_get_lcd_startup_line(void);
extern bool ss_get_vblank_pending(void);
extern int  ss_get_mode3_extra(void);
extern byte ss_get_scx_at_last_compute(void);
extern int  ss_get_window_line(void);
extern int  ss_get_if_cleared_proj_gc(void);
extern int  ss_get_if_cleared_proj_ly(void);
extern uint16_t ss_get_timer_internal(void);
extern int  ss_get_timer_overflow_pending(void);
extern bool ss_get_timer_just_reloaded(void);
extern bool ss_get_timer_halt_delay(void);
extern bool ss_get_serial_active(void);
extern int  ss_get_serial_bits_remaining(void);
extern byte ss_get_serial_out_byte(void);
extern int  ss_get_gb_model(void);

extern void ss_set_stat_irq_line(bool v);
extern void ss_set_lcd_off_lyc_flag(bool v);
extern void ss_set_lcd_startup_mode0(bool v);
extern void ss_set_lcd_startup_line(bool v);
extern void ss_set_vblank_pending(bool v);
extern void ss_set_mode3_extra(int v);
extern void ss_set_scx_at_last_compute(byte v);
extern void ss_set_window_line(int v);
extern void ss_set_if_cleared_proj_gc(int v);
extern void ss_set_if_cleared_proj_ly(int v);
extern void ss_set_timer_internal(uint16_t v);
extern void ss_set_timer_overflow_pending(int v);
extern void ss_set_timer_just_reloaded(bool v);
extern void ss_set_timer_halt_delay(bool v);
extern void ss_set_serial_active(bool v);
extern void ss_set_serial_bits_remaining(int v);
extern void ss_set_serial_out_byte(byte v);
extern void ss_set_gb_model(int v);

/* Called after load to rebuild derived state (gpu_control, etc.) */
extern void gpu_parse_control(byte control);
extern void ss_post_load(void);

/* ---- CBOR helpers ---- */

/* Build a CBOR integer from a signed value */
static cbor_item_t *build_sint(int64_t v) {
    if (v >= 0) {
        return cbor_build_uint64((uint64_t)v);
    } else {
        /* CBOR negint encodes -(n+1), so n = -(v+1) */
        return cbor_build_negint64((uint64_t)(-(v + 1)));
    }
}

/* Add a key/value pair to a CBOR map, transferring ownership of both */
static void map_put(cbor_item_t *map, const char *key, cbor_item_t *val) {
    struct cbor_pair pair = {
        .key   = cbor_move(cbor_build_string(key)),
        .value = cbor_move(val)
    };
    (void)cbor_map_add(map, pair);
}

#define MAP_BOOL(m, k, v)   map_put((m), (k), cbor_build_bool((v)))
#define MAP_U8(m, k, v)     map_put((m), (k), cbor_build_uint8((uint8_t)(v)))
#define MAP_U16(m, k, v)    map_put((m), (k), cbor_build_uint16((uint16_t)(v)))
#define MAP_INT(m, k, v)    map_put((m), (k), build_sint((int64_t)(v)))
#define MAP_BYTES(m, k, p, n) map_put((m), (k), \
    cbor_build_bytestring((const uint8_t *)(p), (size_t)(n)))
#define MAP_STR(m, k, s)    map_put((m), (k), cbor_build_string(s))

/* ---- Lookup a value in a CBOR map by string key ---- */
static cbor_item_t *map_get(const cbor_item_t *map, const char *key) {
    size_t n = cbor_map_size(map);
    struct cbor_pair *pairs = cbor_map_handle(map);
    for (size_t i = 0; i < n; i++) {
        cbor_item_t *k = pairs[i].key;
        if (cbor_isa_string(k)) {
            size_t klen = cbor_string_length(k);
            unsigned char *kdata = cbor_string_handle(k);
            if (klen == strlen(key) && memcmp(kdata, key, klen) == 0) {
                return pairs[i].value;
            }
        }
    }
    return NULL;
}

static int64_t get_int(const cbor_item_t *map, const char *key, int64_t def) {
    cbor_item_t *v = map_get(map, key);
    if (!v) return def;
    if (cbor_isa_uint(v))   return (int64_t)cbor_get_int(v);
    if (cbor_isa_negint(v)) return -(int64_t)cbor_get_int(v) - 1;
    return def;
}

static bool get_bool(const cbor_item_t *map, const char *key, bool def) {
    cbor_item_t *v = map_get(map, key);
    if (!v) return def;
    if (cbor_is_bool(v)) return cbor_get_bool(v);
    return def;
}

static uint8_t get_u8(const cbor_item_t *map, const char *key, uint8_t def) {
    cbor_item_t *v = map_get(map, key);
    if (!v) return def;
    if (cbor_isa_uint(v)) return (uint8_t)cbor_get_int(v);
    return def;
}

static uint16_t get_u16(const cbor_item_t *map, const char *key, uint16_t def) {
    cbor_item_t *v = map_get(map, key);
    if (!v) return def;
    if (cbor_isa_uint(v)) return (uint16_t)cbor_get_int(v);
    return def;
}

/* Copy bytes from a CBOR bytestring into dst, up to max_len bytes */
static size_t get_bytes(const cbor_item_t *map, const char *key,
                        void *dst, size_t max_len) {
    cbor_item_t *v = map_get(map, key);
    if (!v || !cbor_isa_bytestring(v)) return 0;
    size_t len = cbor_bytestring_length(v);
    if (len > max_len) len = max_len;
    memcpy(dst, cbor_bytestring_handle(v), len);
    return len;
}

/* Get a string value (NUL-terminated, up to max_len-1 chars) */
static void get_string(const cbor_item_t *map, const char *key,
                       char *dst, size_t max_len) {
    cbor_item_t *v = map_get(map, key);
    if (!v || !cbor_isa_string(v)) { dst[0] = '\0'; return; }
    size_t len = cbor_string_length(v);
    if (len >= max_len) len = max_len - 1;
    memcpy(dst, cbor_string_handle(v), len);
    dst[len] = '\0';
}

/* ---- Number of fields in the map — keep in sync with MAP_* calls below ---- */
#define SS_MAP_SIZE 76

/* ---- save_state_internal: shared implementation ---- */

static bool save_state_internal(const char *path, const char *slot_name) {
    int has_meta = (slot_name && slot_name[0]) ? 1 : 0;
    cbor_item_t *map = cbor_new_definite_map((size_t)(SS_MAP_SIZE + has_meta * 2));
    if (!map) { fprintf(stderr, "save_state: cbor_new_definite_map failed\n"); return false; }

    /*
     * Sync named-register variables into the dead RAM slots before saving.
     * These registers are stored in named globals (SCX_REG, etc.) at runtime
     * and their RAM[] slots are otherwise stale. Canonicalize them here so
     * the saved RAM bytestring is self-consistent.
     */
    RAM[0xFF44] = LY_REG;   /* LY   (read-only, never written by ROM) */
    RAM[0xFF43] = SCX_REG;  /* SCX */
    RAM[0xFF42] = SCY_REG;  /* SCY */
    RAM[0xFF40] = LCDC_REG; /* LCDC */
    RAM[0xFF47] = BGP_REG;  /* BGP */

    /* Version */
    MAP_INT(map, "ver", SAVESTATE_FORMAT_VERSION);
    MAP_STR(map, "rom_path", savestate_rom_path);

    /* CPU */
    MAP_U16(map, "pc",       PC);
    MAP_U16(map, "sp",       SP);
    MAP_U8 (map, "a",        A);
    MAP_U8 (map, "f",        F);
    MAP_U8 (map, "b",        B);
    MAP_U8 (map, "c",        C);
    MAP_U8 (map, "d",        D);
    MAP_U8 (map, "e",        E);
    MAP_U8 (map, "h",        H);
    MAP_U8 (map, "l",        L);
    MAP_BOOL(map, "ime",     interrupts);
    MAP_INT (map, "ei_delay",ei_delay);
    MAP_BOOL(map, "halted",  halted);
    MAP_BOOL(map, "halt_bug",halt_bug_active);
    MAP_INT (map, "cycles",  cycles);

    /* PPU */
    MAP_INT (map, "gpu_cycles",  gpu_cycles);
    MAP_U8  (map, "ly",          LY_REG);
    MAP_U8  (map, "real_ly",     real_ly);
    MAP_U8  (map, "scx",         SCX_REG);
    MAP_U8  (map, "scy",         SCY_REG);
    MAP_U8  (map, "lcdc",        LCDC_REG);
    MAP_U8  (map, "bgp",         BGP_REG);
    MAP_BOOL(map, "stat_irq",    ss_get_stat_irq_line());
    MAP_BOOL(map, "lcd_off_lyc", ss_get_lcd_off_lyc_flag());
    MAP_BOOL(map, "startup_m0",  ss_get_lcd_startup_mode0());
    MAP_BOOL(map, "startup_ln",  ss_get_lcd_startup_line());
    MAP_BOOL(map, "vblank_pend", ss_get_vblank_pending());
    MAP_INT (map, "mode3_extra", ss_get_mode3_extra());
    MAP_U8  (map, "scx_last",    ss_get_scx_at_last_compute());
    MAP_INT (map, "win_line",    ss_get_window_line());
    MAP_INT (map, "if_proj_gc",  ss_get_if_cleared_proj_gc());
    MAP_INT (map, "if_proj_ly",  ss_get_if_cleared_proj_ly());
    MAP_BYTES(map, "vram",       VRAM, 0x2000);

    /* Timer */
    MAP_U16 (map, "timer_int",   ss_get_timer_internal());
    MAP_INT (map, "tima_ovf",    ss_get_timer_overflow_pending());
    MAP_BOOL(map, "tima_reload", ss_get_timer_just_reloaded());
    MAP_BOOL(map, "tima_halt",   ss_get_timer_halt_delay());

    /* DMA */
    MAP_BOOL(map, "dma_active",  dma_active);
    MAP_BOOL(map, "dma_locked",  dma_oam_locked);
    MAP_INT (map, "dma_remain",  dma_cycles_remaining);
    MAP_INT (map, "dma_startup", dma_startup_remaining);
    MAP_INT (map, "dma_src",     dma_source);

    /* Memory */
    MAP_BYTES(map, "ram",        RAM, 0x10000);
    MAP_BYTES(map, "ext_ram",    ext_ram, 0x8000);

    /* Cartridge */
    MAP_U8  (map, "cart_type",   cart_type);
    MAP_INT (map, "rom_banks",   cart_rom_banks);
    MAP_INT (map, "rom_bank",    cart_rom_bank);
    MAP_INT (map, "mbc1_upper",  cart_mbc1_upper);
    MAP_INT (map, "mbc1_mode",   cart_mbc1_mode);
    MAP_INT (map, "ram_bank",    cart_ram_bank);
    MAP_INT (map, "model",       ss_get_gb_model());

    /* Serial */
    MAP_U8  (map, "ser_sb",      serial_sb);
    MAP_BOOL(map, "ser_active",  ss_get_serial_active());
    MAP_INT (map, "ser_bits",    ss_get_serial_bits_remaining());
    MAP_U8  (map, "ser_out",     ss_get_serial_out_byte());

    /* APU */
    MAP_BOOL(map, "apu_ch1_on",      apu_ch1_active);
    MAP_BOOL(map, "apu_ch1_len_en",  apu_ch1_length_enable);
    MAP_INT (map, "apu_ch1_len",     apu_ch1_length);
    MAP_BOOL(map, "apu_ch2_on",      apu_ch2_active);
    MAP_BOOL(map, "apu_ch2_len_en",  apu_ch2_length_enable);
    MAP_INT (map, "apu_ch2_len",     apu_ch2_length);
    MAP_INT (map, "apu_cyc",         apu_length_cycles);

    /* Misc */
    MAP_BOOL(map, "dbl_spd",     double_speed);
    MAP_U8  (map, "joypad",      joypad);
    MAP_BOOL(map, "in_bios",     inBios);

    /* Optional slot metadata (only written when slot_name is non-empty) */
    if (has_meta) {
        MAP_STR(map, "slot_name", slot_name);
        MAP_INT(map, "save_ts",   (int64_t)time(NULL));
    }

    /* Serialize to buffer */
    uint8_t *buf = NULL;
    size_t buf_len = 0;
    buf_len = cbor_serialize_alloc(map, &buf, &buf_len);
    cbor_decref(&map);

    if (!buf || buf_len == 0) {
        fprintf(stderr, "save_state: cbor_serialize_alloc failed\n");
        free(buf);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "save_state: cannot open %s for writing: ", path);
        perror("");
        free(buf);
        return false;
    }
    size_t written = fwrite(buf, 1, buf_len, f);
    fclose(f);
    free(buf);

    if (written != buf_len) {
        fprintf(stderr, "save_state: short write to %s\n", path);
        return false;
    }

    fprintf(stderr, "[savestate] saved %zu bytes to %s\n", buf_len, path);
    return true;
}

bool save_state(const char *path)                            { return save_state_internal(path, NULL); }
bool save_state_slot(const char *path, const char *name)     { return save_state_internal(path, name); }

/* ---- load_state ---- */

/* Forward-declare cart_load from gb.c (takes a non-const pointer) */
void cart_load(char *path);
void mem_init(void);
void gpu_init(void);
void cpu_fake_init(void);
void joypad_init(void);

bool load_state(const char *path) {
    /* Read the entire file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "load_state: cannot open %s: ", path);
        perror("");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    rewind(f);
    if (file_len <= 0) {
        fprintf(stderr, "load_state: empty or unreadable file %s\n", path);
        fclose(f);
        return false;
    }

    uint8_t *buf = malloc((size_t)file_len);
    if (!buf) { perror("load_state: malloc"); fclose(f); return false; }
    if (fread(buf, 1, (size_t)file_len, f) != (size_t)file_len) {
        fprintf(stderr, "load_state: short read from %s\n", path);
        fclose(f); free(buf); return false;
    }
    fclose(f);

    struct cbor_load_result result;
    cbor_item_t *root = cbor_load(buf, (size_t)file_len, &result);
    free(buf);

    if (!root) {
        fprintf(stderr, "load_state: CBOR parse error at offset %zu\n", result.error.position);
        return false;
    }
    if (!cbor_isa_map(root)) {
        fprintf(stderr, "load_state: root is not a CBOR map\n");
        cbor_decref(&root);
        return false;
    }

    /* Version check */
    int64_t ver = get_int(root, "ver", -1);
    if (ver != SAVESTATE_FORMAT_VERSION) {
        fprintf(stderr, "load_state: unsupported format version %" PRId64
                        " (expected %d)\n", ver, SAVESTATE_FORMAT_VERSION);
        cbor_decref(&root);
        return false;
    }

    /* Load the ROM first */
    char rom_path[4096];
    get_string(root, "rom_path", rom_path, sizeof(rom_path));
    if (rom_path[0] == '\0') {
        fprintf(stderr, "load_state: no rom_path in save state\n");
        cbor_decref(&root);
        return false;
    }
#ifdef __EMSCRIPTEN__
    /* In WASM, save states may reference a host path that doesn't exist.
     * Fall back to /rom.gb which is always written to MEMFS before load. */
    if (access(rom_path, F_OK) != 0) {
        strncpy(rom_path, "/rom.gb", sizeof(rom_path) - 1);
        rom_path[sizeof(rom_path) - 1] = '\0';
    }
#endif
    cart_load(rom_path);
    mem_init();
    gpu_init();
    cpu_fake_init();
    joypad_init();

    /* Restore ROM path global */
    strncpy(savestate_rom_path, rom_path, sizeof(savestate_rom_path) - 1);
    savestate_rom_path[sizeof(savestate_rom_path) - 1] = '\0';

    /* CPU */
    PC           = get_u16(root, "pc",       0x0100);
    SP           = get_u16(root, "sp",       0xFFFE);
    A            = get_u8 (root, "a",        0);
    F            = get_u8 (root, "f",        0);
    B            = get_u8 (root, "b",        0);
    C            = get_u8 (root, "c",        0);
    D            = get_u8 (root, "d",        0);
    E            = get_u8 (root, "e",        0);
    H            = get_u8 (root, "h",        0);
    L            = get_u8 (root, "l",        0);
    interrupts   = get_bool(root, "ime",     false);
    ei_delay     = (int)get_int(root, "ei_delay", 0);
    halted       = get_bool(root, "halted",  false);
    halt_bug_active = get_bool(root, "halt_bug", false);
    cycles       = (int)get_int(root, "cycles",  0);

    /* PPU */
    gpu_cycles   = (int)get_int(root, "gpu_cycles",  0);
    LY_REG       = get_u8 (root, "ly",       0);
    real_ly      = get_u8 (root, "real_ly",  0);
    SCX_REG      = get_u8 (root, "scx",      0);
    SCY_REG      = get_u8 (root, "scy",      0);
    LCDC_REG     = get_u8 (root, "lcdc",     0x91);
    BGP_REG      = get_u8 (root, "bgp",      0xFC);
    gpu_parse_control(LCDC_REG);

    ss_set_stat_irq_line      (get_bool(root, "stat_irq",    false));
    ss_set_lcd_off_lyc_flag   (get_bool(root, "lcd_off_lyc", false));
    ss_set_lcd_startup_mode0  (get_bool(root, "startup_m0",  false));
    ss_set_lcd_startup_line   (get_bool(root, "startup_ln",  false));
    ss_set_vblank_pending     (get_bool(root, "vblank_pend", false));
    ss_set_mode3_extra        ((int)get_int(root,  "mode3_extra", 0));
    ss_set_scx_at_last_compute(get_u8(root,  "scx_last",     0));
    ss_set_window_line        ((int)get_int(root,  "win_line",    0));
    ss_set_if_cleared_proj_gc ((int)get_int(root,  "if_proj_gc",  -100));
    ss_set_if_cleared_proj_ly ((int)get_int(root,  "if_proj_ly",  -100));
    get_bytes(root, "vram", VRAM, 0x2000);

    /* Timer */
    ss_set_timer_internal      (get_u16 (root, "timer_int",   0));
    ss_set_timer_overflow_pending((int)get_int(root, "tima_ovf", 0));
    ss_set_timer_just_reloaded (get_bool(root, "tima_reload", false));
    ss_set_timer_halt_delay    (get_bool(root, "tima_halt",   false));

    /* DMA */
    dma_active            = get_bool(root, "dma_active",  false);
    dma_oam_locked        = get_bool(root, "dma_locked",  false);
    dma_cycles_remaining  = (int)get_int(root, "dma_remain",  0);
    dma_startup_remaining = (int)get_int(root, "dma_startup", 0);
    dma_source            = (int)get_int(root, "dma_src",     0);

    /* Memory */
    get_bytes(root, "ram",     RAM,     0x10000);
    get_bytes(root, "ext_ram", ext_ram, 0x8000);

    /* Cartridge banking */
    cart_type     = get_u8 (root, "cart_type",  0);
    cart_rom_banks= (int)get_int(root, "rom_banks",  1);
    cart_rom_bank = (int)get_int(root, "rom_bank",   1);
    cart_mbc1_upper= (int)get_int(root, "mbc1_upper", 0);
    cart_mbc1_mode = (int)get_int(root, "mbc1_mode",  0);
    cart_ram_bank  = (int)get_int(root, "ram_bank",   0);
    ss_set_gb_model((int)get_int(root, "model",       0));

    /* Serial */
    serial_sb = get_u8 (root, "ser_sb", 0);
    ss_set_serial_active          (get_bool(root, "ser_active", false));
    ss_set_serial_bits_remaining  ((int)get_int(root, "ser_bits", 0));
    ss_set_serial_out_byte        (get_u8(root, "ser_out", 0));

    /* APU */
    apu_ch1_active        = get_bool(root, "apu_ch1_on",     false);
    apu_ch1_length_enable = get_bool(root, "apu_ch1_len_en", false);
    apu_ch1_length        = (int)get_int(root,  "apu_ch1_len",   0);
    apu_ch2_active        = get_bool(root, "apu_ch2_on",     false);
    apu_ch2_length_enable = get_bool(root, "apu_ch2_len_en", false);
    apu_ch2_length        = (int)get_int(root,  "apu_ch2_len",   0);
    apu_length_cycles     = (int)get_int(root,  "apu_cyc",       0);

    /* Misc */
    double_speed = get_bool(root, "dbl_spd", false);
    joypad       = get_u8  (root, "joypad",  0x3F);
    inBios       = get_bool(root, "in_bios", false);

    cbor_decref(&root);

    /* Rebuild any derived state */
    ss_post_load();

    fprintf(stderr, "[savestate] loaded from %s\n", path);
    return true;
}

/* ---- Utility ---- */

bool savestate_is_cbor_path(const char *path) {
    size_t len = strlen(path);
    return len > 5 && strcmp(path + len - 5, ".cbor") == 0;
}

char *savestate_default_path(const char *rom_path, char *out, size_t out_size) {
    strncpy(out, rom_path, out_size - 6);
    out[out_size - 6] = '\0';
    /* Strip existing extension if any */
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        *dot = '\0';
    }
    strncat(out, ".cbor", out_size - strlen(out) - 1);
    return out;
}

char *savestate_slot_path(const char *rom_path, int slot, char *out, size_t out_size) {
    /* Reserve room for ".slotN.cbor\0" (12 bytes) */
    strncpy(out, rom_path, out_size - 13);
    out[out_size - 13] = '\0';
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    char suffix[16];
    snprintf(suffix, sizeof(suffix), ".slot%d.cbor", slot);
    strncat(out, suffix, out_size - strlen(out) - 1);
    return out;
}

bool slot_read_meta(const char *path, char *name_out, size_t name_size,
                    int64_t *ts_out) {
    if (name_out && name_size > 0) name_out[0] = '\0';
    if (ts_out) *ts_out = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return false; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) { free(buf); fclose(f); return false; }
    fclose(f);

    struct cbor_load_result res;
    cbor_item_t *root = cbor_load(buf, (size_t)sz, &res);
    free(buf);
    if (!root) return false;

    if (name_out) get_string(root, "slot_name", name_out, name_size);
    if (ts_out)   *ts_out = get_int(root, "save_ts", 0);

    cbor_decref(&root);
    return true;
}
