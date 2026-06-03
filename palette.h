#pragma once
#include <stdint.h>

/* Active DMG palette — 4 RGBA32 entries, shade 0 = lightest.
 * Encoding: uint32 = (A<<24)|(B<<16)|(G<<8)|R  (bytes in memory: R,G,B,A)
 * Used by gpu_pallete_color() in gb.c.  Mutable so frontends can swap palettes. */
extern uint32_t pallette[4];

typedef struct {
    const char  *name;
    uint32_t     colors[4]; /* same RGBA32 encoding as above */
} gb_palette_t;

extern const gb_palette_t GB_PALETTES[];
extern const int          GB_PALETTE_COUNT;

void palette_set(int idx);
int  palette_get(void);
