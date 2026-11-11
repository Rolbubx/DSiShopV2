// Partie matérielle du rendu : un fond bitmap 16 bits sur chaque écran

#include <nds.h>

#include "gfx.h"

static int bg_main;
static int bg_sub;

void gfx_hw_init(void)
{
    // Mode 5 : BG3 en bitmap "étendu" sur les deux moteurs 2D
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);

    vramSetBankA(VRAM_A_MAIN_BG); // 128 Ko : 256x256x16 bits
    vramSetBankC(VRAM_C_SUB_BG);

    bg_main = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bg_sub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    // Moteur principal en haut, secondaire (tactile) en bas
    lcdMainOnTop();
}

void gfx_hw_present(screen_id_t screen, const uint16_t *pixels)
{
    uint16_t *dst = bgGetGfxPtr(screen == SCREEN_TOP ? bg_main : bg_sub);
    const size_t size = SCREEN_W * SCREEN_H * sizeof(uint16_t);

    // Le DMA lit la RAM directement : il faut vider le cache avant
    DC_FlushRange(pixels, size);
    dmaCopyWords(3, pixels, dst, size);
}
