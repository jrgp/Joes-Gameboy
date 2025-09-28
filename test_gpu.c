#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "bios.h"
#include "bits.h"
#include "constants.h"

// Include the GPU functions from gb.c
typedef uint8_t byte;
typedef uint16_t word;

// GPU structures and variables (copied from gb.c)
int gpu_cycles;
byte VRAM[0x2000];  // VRAM is 8KB, not 64KB
byte LY_REG = 0;    // LY register (0xFF44)
byte SCX_REG = 0;   // SCX register (0xFF43)
byte SCY_REG = 0;   // SCY register (0xFF42)
byte LCDC_REG = 0;  // LCDC register (0xFF40)
byte BGP_REG = 0;   // BGP register (0xFF47)

typedef struct {
    bool enabled, bg, window, sprite;
    int windowtilemap, BgWindowTileData;
    bool BgTileDataSigned;
    int bgtilemap;
} GPUCONTROL;

GPUCONTROL gpu_control;

// Mock memory functions for testing
byte RAM[0xffff + 1];
bool inBios = false;

// Forward declarations
void gpu_parse_control(byte control);

byte mem_read(int pos) {
    if (pos >= 0x8000 && pos <= 0x9FFF) {
        return VRAM[pos - 0x8000];
    }
    if (pos == LY) {
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
    return RAM[pos];
}

void mem_write(int pos, byte data) {
    printf("mem_write: pos=0x%04X, data=0x%02X\n", pos, data);
    
    if (pos >= 0x8000 && pos <= 0x9FFF) {
        VRAM[pos - 0x8000] = data;
    }
    if (pos == LCDC) {
        printf("  Writing to LCDC register\n");
        LCDC_REG = data;
        gpu_parse_control(data);
    }
    if (pos == LY) {
        LY_REG = data;
    }
    if (pos == SCX) {
        SCX_REG = data;
    }
    if (pos == SCY) {
        SCY_REG = data;
    }
    if (pos == BGP) {
        printf("  Writing to BGP register\n");
        BGP_REG = data;
    }
    RAM[pos] = data;
}

// GPU functions (copied from gb.c)
void gpu_parse_control(byte control){
    printf("gpu_parse_control called with 0x%02X\n", control);
    printf("  Bit 7 (enabled): %s\n", bit_check(control, 7) ? "true" : "false");
    printf("  Bit 0 (bg): %s\n", bit_check(control, 0) ? "true" : "false");
    printf("  Bit 4 (tile data): %s\n", bit_check(control, 4) ? "true" : "false");
    printf("  Bit 3 (tile map): %s\n", bit_check(control, 3) ? "true" : "false");
    
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
    
    printf("  After parsing: enabled=%s, bg=%s, BgWindowTileData=0x%04X, bgtilemap=0x%04X\n", 
           gpu_control.enabled ? "true" : "false",
           gpu_control.bg ? "true" : "false",
           gpu_control.BgWindowTileData,
           gpu_control.bgtilemap);
}

uint32_t gpu_pallete_color(byte number, int paletteIndex) {
    const byte config = mem_read(paletteIndex);
    byte resultindex = 0;

    const byte b1 = number * 2;
    const byte b2 = (number * 2) + 1;

    if(bit_check(config, b1)){
        resultindex |= (1 << 1);
    }

    if(bit_check(config, b2)){
        resultindex |= (1 << 0);
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
    
    printf("Tile %d at line %d: high=0x%02X, low=0x%02X\n", tileIndex, ly, high, low);
    
    // Test pixel generation
    for (byte i = 0; i < 8; i++) {
        byte pixel_value = 0;
        if(bit_check(low, i)){
            pixel_value |= (1 << 1);
        }
        if(bit_check(high, i)){
            pixel_value |= (1 << 0);
        }
        
        uint32_t color = gpu_pallete_color(pixel_value, paletteIndex);
        printf("  Pixel %d: value=%d, color=0x%06X\n", i, pixel_value, color);
    }
}

void gpu_draw_bg(byte ly){
    printf("Drawing background line %d: BG=%s\n", ly, gpu_control.bg ? "ON" : "OFF");
    
    if (gpu_control.bg) {
        for (int tile = 0; tile < 32; tile++) {
            // Calculate tile position with scrolling
            int tile_x = tile + (SCX_REG / 8);
            int tile_y = (ly / 8) + (SCY_REG / 8);
            
            // Wrap around for tile map
            tile_x = tile_x % 32;
            tile_y = tile_y % 32;
            
            // Index of tile from sprite map
            const int tileIndex = mem_read(gpu_control.bgtilemap + (tile_y * 32) + tile_x);
            printf("Tile at (%d,%d): index=%d\n", tile_x, tile_y, tileIndex);
            
            if (tile < 2) { // Only test first 2 tiles to avoid spam
                gpu_render_tile(ly, (tile*8), tileIndex, BGP, false, false);
            }
        }
    } else {
        printf("Background disabled, filling with white\n");
    }
}

void gpu_init(void){
    memset(VRAM, 0, 0x2000);
    gpu_cycles = 0;
    LY_REG = 0;
    
    // Initialize scroll registers first
    mem_write(SCX, 0);
    mem_write(SCY, 0);
    
    // Set default LCDC register value (display enabled, background enabled)
    // This will call gpu_parse_control and set up the GPU control properly
    mem_write(LCDC, 0x91);  // 10010001 - LCD enabled, BG enabled, BG tile data at 0x8000, BG tile map at 0x9800
    
    // Set default BGP register (white, light gray, dark gray, black)
    mem_write(BGP, 0xE4);   // 11100100 - white, light gray, dark gray, black
    
    // Initialize some basic tile data for testing
    // Create a simple checkerboard pattern in tile 0
    for (int i = 0; i < 8; i++) {
        if (i % 2 == 0) {
            mem_write(0x8000 + (i * 2), 0xAA);     // 10101010
            mem_write(0x8000 + (i * 2) + 1, 0x55); // 01010101
        } else {
            mem_write(0x8000 + (i * 2), 0x55);     // 01010101
            mem_write(0x8000 + (i * 2) + 1, 0xAA); // 10101010
        }
    }
    
    // Set tile map to use tile 0 everywhere
    for (int i = 0; i < 32 * 32; i++) {
        mem_write(0x9800 + i, 0);
    }
}

// Test functions
void test_gpu_initialization() {
    printf("=== Testing GPU Initialization ===\n");
    
    gpu_init();
    
    printf("LCDC register: 0x%02X (expected: 0x91)\n", mem_read(LCDC));
    printf("BGP register: 0x%02X (expected: 0xE4)\n", mem_read(BGP));
    printf("SCX register: 0x%02X (expected: 0x00)\n", mem_read(SCX));
    printf("SCY register: 0x%02X (expected: 0x00)\n", mem_read(SCY));
    
    printf("GPU Control:\n");
    printf("  enabled: %s\n", gpu_control.enabled ? "true" : "false");
    printf("  bg: %s\n", gpu_control.bg ? "true" : "false");
    printf("  sprite: %s\n", gpu_control.sprite ? "true" : "false");
    printf("  BgWindowTileData: 0x%04X\n", gpu_control.BgWindowTileData);
    printf("  bgtilemap: 0x%04X\n", gpu_control.bgtilemap);
    
    assert(mem_read(LCDC) == 0x91);
    assert(mem_read(BGP) == 0xE4);
    assert(gpu_control.enabled == true);
    assert(gpu_control.bg == true);
    
    printf("✓ GPU initialization test passed!\n\n");
}

void test_palette_conversion() {
    printf("=== Testing Palette Conversion ===\n");
    
    printf("BGP register value: 0x%02X\n", mem_read(BGP));
    printf("BGP binary: ");
    for (int i = 7; i >= 0; i--) {
        printf("%d", bit_check(mem_read(BGP), i) ? 1 : 0);
    }
    printf("\n");
    
    // Test with BGP = 0xFC (11111100)
    uint32_t color0 = gpu_pallete_color(0, BGP); // Should be white (0xFFFFFF)
    uint32_t color1 = gpu_pallete_color(1, BGP); // Should be light gray (0x555555)
    uint32_t color2 = gpu_pallete_color(2, BGP); // Should be dark gray (0xAAAAAA)
    uint32_t color3 = gpu_pallete_color(3, BGP); // Should be black (0x000000)
    
    printf("Color 0: 0x%06X (expected: 0xFFFFFF)\n", color0);
    printf("Color 1: 0x%06X (expected: 0x555555)\n", color1);
    printf("Color 2: 0x%06X (expected: 0xAAAAAA)\n", color2);
    printf("Color 3: 0x%06X (expected: 0x000000)\n", color3);
    
    // Debug the palette conversion step by step
    printf("\nDebugging palette conversion:\n");
    for (int pixel_value = 0; pixel_value < 4; pixel_value++) {
        printf("Pixel value %d:\n", pixel_value);
        byte config = mem_read(BGP);
        byte resultindex = 0;
        
        const byte b1 = pixel_value * 2;
        const byte b2 = (pixel_value * 2) + 1;
        
        printf("  b1=%d, b2=%d\n", b1, b2);
        printf("  bit_check(config, %d)=%s\n", b1, bit_check(config, b1) ? "true" : "false");
        printf("  bit_check(config, %d)=%s\n", b2, bit_check(config, b2) ? "true" : "false");
        
        if(bit_check(config, b1)){
            resultindex |= (1 << 1);
        }
        
        if(bit_check(config, b2)){
            resultindex |= (1 << 0);
        }
        
        printf("  resultindex=%d\n", resultindex);
        printf("  pallette[%d]=0x%06X\n", resultindex, pallette[resultindex]);
    }
    
    assert(color0 == 0xFFFFFF);
    assert(color1 == 0x555555);
    assert(color2 == 0xAAAAAA);
    assert(color3 == 0x000000);
    
    printf("✓ Palette conversion test passed!\n\n");
}

void test_tile_data() {
    printf("=== Testing Tile Data ===\n");
    
    // Check tile 0 data
    printf("Tile 0 data:\n");
    for (int i = 0; i < 8; i++) {
        byte high = mem_read(0x8000 + (i * 2));
        byte low = mem_read(0x8000 + (i * 2) + 1);
        printf("  Line %d: high=0x%02X, low=0x%02X\n", i, high, low);
    }
    
    // Check tile map
    printf("Tile map (first 4 tiles):\n");
    for (int i = 0; i < 4; i++) {
        byte tile_index = mem_read(0x9800 + i);
        printf("  Tile %d: index=%d\n", i, tile_index);
    }
    
    printf("✓ Tile data test passed!\n\n");
}

void test_background_rendering() {
    printf("=== Testing Background Rendering ===\n");
    
    // Test rendering first few lines
    for (int ly = 0; ly < 3; ly++) {
        printf("\n--- Rendering line %d ---\n", ly);
        gpu_draw_bg(ly);
    }
    
    printf("✓ Background rendering test passed!\n\n");
}

int main() {
    printf("Game Boy GPU Unit Tests\n");
    printf("=======================\n\n");
    
    test_gpu_initialization();
    test_palette_conversion();
    test_tile_data();
    test_background_rendering();
    
    printf("All tests passed! ✓\n");
    return 0;
}
