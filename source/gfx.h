// Rendu 2D logiciel : chaque écran est un framebuffer 16 bits (ARGB1555) en
// RAM, recopié en VRAM par DMA pendant le VBlank. Ce module ne dépend pas de
// libnds (sauf gfx_hw.c), ce qui permet de le tester sur PC (tools/sim).
#ifndef GFX_H__
#define GFX_H__

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

#define SCREEN_W 256
#define SCREEN_H 192

// Couleur 15 bits à partir de composantes 8 bits, bit 15 = pixel opaque
#define COLOR_RGB(r, g, b) ((uint16_t)((((r) >> 3) & 31) | ((((g) >> 3) & 31) << 5) | \
                                  ((((b) >> 3) & 31) << 10) | 0x8000))

typedef enum {
    SCREEN_TOP = 0,
    SCREEN_BOTTOM = 1,
} screen_id_t;

typedef enum {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT,
} align_t;

typedef struct {
    uint16_t *px; // SCREEN_W * SCREEN_H pixels
    // Zone de clipping (x1/y1 exclus)
    int clip_x0, clip_y0, clip_x1, clip_y1;
    bool dirty;
} canvas_t;

// Initialise le matériel (gfx_hw.c) et les buffers
void gfx_init(void);

// Renvoie le canvas d'un écran et le marque comme "à afficher"
canvas_t *gfx_canvas(screen_id_t screen);

// À appeler juste après le VBlank : envoie les canvas modifiés à l'écran
void gfx_present(void);

void gfx_set_clip(canvas_t *c, int x, int y, int w, int h);
void gfx_reset_clip(canvas_t *c);

uint16_t gfx_blend(uint16_t dst, uint16_t src, int alpha16); // alpha 0..16

void gfx_clear(canvas_t *c, uint16_t color);
void gfx_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color);
void gfx_fill_rect_alpha(canvas_t *c, int x, int y, int w, int h,
                         uint16_t color, int alpha16);
void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color);
void gfx_hline(canvas_t *c, int x, int y, int w, uint16_t color);
void gfx_vline(canvas_t *c, int x, int y, int h, uint16_t color);
void gfx_gradient_v(canvas_t *c, int x, int y, int w, int h,
                    uint16_t top, uint16_t bottom);
void gfx_fill_round_rect(canvas_t *c, int x, int y, int w, int h, int r,
                         uint16_t color);
void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r,
                    uint16_t color);
void gfx_fill_circle(canvas_t *c, int cx, int cy, int r, uint16_t color);

// Texte UTF-8. y = haut de la ligne. Renvoie la position x finale.
int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *str,
             uint16_t color);
int gfx_text_width(const font_t *f, const char *str);
// Texte aligné dans une boîte de largeur w, tronqué avec "…" si trop long
void gfx_text_box(canvas_t *c, const font_t *f, int x, int y, int w,
                  align_t align, const char *str, uint16_t color);
// Texte avec retour à la ligne automatique. Renvoie la hauteur utilisée.
int gfx_text_wrap(canvas_t *c, const font_t *f, int x, int y, int w, int h,
                  const char *str, uint16_t color);

// Décode un caractère UTF-8 et avance le pointeur
uint32_t utf8_next(const char **s);

#endif // GFX_H__
