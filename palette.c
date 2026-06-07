#include "palette.h"
#include <string.h>

/* Active palette — initialised to DMG Gray. */
uint32_t pallette[4] = { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 };

/* Full palette table.  Every entry has 4 RGBA32 colours ordered lightest→darkest.
 * Encoding: (A<<24)|(B<<16)|(G<<8)|R  →  bytes in RAM: [R, G, B, A]
 * (matches SDL_PIXELFORMAT_RGBA32 and canvas ImageData RGBA layout). */
const gb_palette_t GB_PALETTES[] = {
    /* ---- Standard DMG ---- */
    { "DMG Gray",
      { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 } },
    { "DMG Inverted",
      { 0xFF000000, 0xFF555555, 0xFFAAAAAA, 0xFFFFFFFF } },
    /* Classic DMG "pea soup" green screen */
    { "GB Green",
      { 0xFF0FBC9B, 0xFF0FAC8B, 0xFF306230, 0xFF0F380F } },
    /* Game Boy Pocket — warm gray-green LCD */
    { "GB Pocket",
      { 0xFFA1CFC4, 0xFF6D958B, 0xFF3C534D, 0xFF1F1F1F } },
    /* Game Boy Light — slightly warmer, backlit */
    { "GB Light",
      { 0xFFE0F8E0, 0xFF88D088, 0xFF386838, 0xFF081008 } },

    /* ---- Game Boy Color inspired ---- */
    { "GBC Brown",
      { 0xFFC8E8F8, 0xFF6090B0, 0xFF203868, 0xFF080818 } },
    { "GBC Red",
      { 0xFFEAEAFF, 0xFF4444CC, 0xFF141488, 0xFF000030 } },
    { "GBC Dark Brown",
      { 0xFF88D0F0, 0xFF3070C0, 0xFF104070, 0xFF000810 } },
    { "GBC Pastel Mix",
      { 0xFFE8F8F8, 0xFF58A8C8, 0xFF285068, 0xFF001018 } },
    { "GBC Blue",
      { 0xFFF8F8E0, 0xFFD08858, 0xFFA03818, 0xFF180800 } },
    { "GBC Dark Blue",
      { 0xFFFFF0D0, 0xFFC06020, 0xFF803010, 0xFF100000 } },
    { "GBC Green",
      { 0xFFD0F8E0, 0xFF40C070, 0xFF187030, 0xFF081808 } },
    { "GBC Dark Green",
      { 0xFFB0E8C8, 0xFF40A060, 0xFF205030, 0xFF081008 } },
    { "GBC Yellow",
      { 0xFFA8F8F8, 0xFF20C8E8, 0xFF0878A0, 0xFF002028 } },
    { "GBC Orange",
      { 0xFFA8E0F8, 0xFF2090F0, 0xFF0848A0, 0xFF001020 } },
    /* GB Green reversed (dark → light for a negative look) */
    { "GBC Reverse",
      { 0xFF0F380F, 0xFF306230, 0xFF0FAC8B, 0xFF0FBC9B } },

    /* ---- Extras ---- */
    { "Amber Terminal",
      { 0xFF90E8FF, 0xFF0098FF, 0xFF0050C0, 0xFF000820 } },
    { "Green Terminal",
      { 0xFFAAFFAA, 0xFF00EE00, 0xFF007700, 0xFF001400 } },
    { "Ice Blue",
      { 0xFFFFF4E8, 0xFFF0C890, 0xFFB86020, 0xFF281400 } },
    { "High Contrast",
      { 0xFFFFFFFF, 0xFF888888, 0xFF333333, 0xFF000000 } },
    { "Night Mode",
      { 0xFFC0A88F, 0xFF685840, 0xFF382818, 0xFF100804 } },
    { "Nord",
      { 0xFFF4EFEC, 0xFFD0C088, 0xFF6A564C, 0xFF40342E } },
    { "Nord Dark",
      { 0xFF40342E, 0xFF6A564C, 0xFFD0C088, 0xFFF4EFEC } },
};

const int GB_PALETTE_COUNT = (int)(sizeof(GB_PALETTES) / sizeof(GB_PALETTES[0]));

static int g_palette_idx = 0;

void palette_set(int idx) {
    if (idx < 0) idx = GB_PALETTE_COUNT - 1;
    if (idx >= GB_PALETTE_COUNT) idx = 0;
    g_palette_idx = idx;
    memcpy(pallette, GB_PALETTES[idx].colors, sizeof(pallette));
}

int palette_get(void) {
    return g_palette_idx;
}
