#pragma once

#include <stdint.h>

#define LCDC 0xFF40
#define STAT 0xFF41
#define SCY 0xFF42
#define SCX 0xFF43
#define LY 0xFF44
#define LYC 0xFF45
#define WY 0xFF4A
#define WX 0xFF4B
#define BGP 0xFF47
#define JOYPAD 0xFF00

#define DMA 0xff46

#define OBP0 0xFF48
#define OBP1 0xFF49

#define INTERRUPT_ENABLE 0xFFFF
#define INTERRUPT_FLAGS  0xFF0F

#define INTERRUPT_VBLANK  1
#define INTERRUPT_STAT    2
#define INTERRUPT_TIMER   4
#define INTERRUPT_SERIAL  8
#define INTERRUPT_JOYPAD  16

static const char INTERRUPT_PRIORITY[5] = {
	INTERRUPT_VBLANK,
	INTERRUPT_STAT,
	INTERRUPT_TIMER,
	INTERRUPT_SERIAL,
	INTERRUPT_JOYPAD
};

static const char *const INTERRUPT_NAMES[5] = {
	"VBLANK",
	"STAT",
	"TIMER",
	"SERIAL",
	"JOYPAD"
};

static const uint16_t INTERRUPT_OFFSETS[5] = {
	0x40,
	0x48,
	0x50,
	0x58,
	0x60
};

#define FPS 60

static const uint32_t pallette[4] = {
    0xffffffff,
    0xffaaaaaa,
    0xff555555,
    0xff000000,
};

#define VIEWPORT_WIDTH 160
#define VIEWPORT_HEIGHT 144

#define WIDTH 256
#define HEIGHT 256
