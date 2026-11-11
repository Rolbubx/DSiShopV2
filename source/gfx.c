// Primitives de dessin portables (aucune dépendance à libnds)

#include <string.h>

#include "gfx.h"

static uint16_t framebuffers[2][SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static canvas_t canvases[2];

// Implémenté dans gfx_hw.c (DS) ou tools/sim (PC)
void gfx_hw_init(void);
void gfx_hw_present(screen_id_t screen, const uint16_t *pixels);

void gfx_init(void)
{
    for (int i = 0; i < 2; i++)
    {
        canvases[i].px = framebuffers[i];
        gfx_reset_clip(&canvases[i]);
        canvases[i].dirty = true;
        memset(framebuffers[i], 0, sizeof(framebuffers[i]));
    }
    gfx_hw_init();
}

canvas_t *gfx_canvas(screen_id_t screen)
{
    canvas_t *c = &canvases[screen];
    c->dirty = true;
    return c;
}

void gfx_present(void)
{
    for (int i = 0; i < 2; i++)
    {
        if (!canvases[i].dirty)
            continue;
        canvases[i].dirty = false;
        gfx_hw_present((screen_id_t)i, canvases[i].px);
    }
}

void gfx_set_clip(canvas_t *c, int x, int y, int w, int h)
{
    c->clip_x0 = x < 0 ? 0 : x;
    c->clip_y0 = y < 0 ? 0 : y;
    c->clip_x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    c->clip_y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
}

void gfx_reset_clip(canvas_t *c)
{
    gfx_set_clip(c, 0, 0, SCREEN_W, SCREEN_H);
}

uint16_t gfx_blend(uint16_t dst, uint16_t src, int a)
{
    if (a <= 0)
        return dst;
    if (a >= 16)
        return src;

    int dr = dst & 31, dg = (dst >> 5) & 31, db = (dst >> 10) & 31;
    int sr = src & 31, sg = (src >> 5) & 31, sb = (src >> 10) & 31;

    int r = dr + (((sr - dr) * a) >> 4);
    int g = dg + (((sg - dg) * a) >> 4);
    int b = db + (((sb - db) * a) >> 4);

    return (uint16_t)(r | (g << 5) | (b << 10) | 0x8000);
}

// Intersecte un rectangle avec la zone de clipping. Renvoie false si vide.
static bool clip_rect(const canvas_t *c, int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < c->clip_x0) x0 = c->clip_x0;
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (x0 >= x1 || y0 >= y1)
        return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

static inline void put_pixel_alpha(canvas_t *c, int x, int y, uint16_t color, int a)
{
    if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1)
        return;
    uint16_t *p = &c->px[y * SCREEN_W + x];
    *p = gfx_blend(*p, color, a);
}

void gfx_clear(canvas_t *c, uint16_t color)
{
    gfx_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H, color);
}

void gfx_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (!clip_rect(c, &x, &y, &w, &h))
        return;

    uint32_t pair = color | ((uint32_t)color << 16);

    for (int j = 0; j < h; j++)
    {
        uint16_t *p = &c->px[(y + j) * SCREEN_W + x];
        int n = w;
        // Aligne sur 32 bits puis écrit deux pixels à la fois
        if (((uintptr_t)p & 2) && n > 0)
        {
            *p++ = color;
            n--;
        }
        uint32_t *p32 = (uint32_t *)p;
        for (; n >= 2; n -= 2)
            *p32++ = pair;
        if (n)
            *(uint16_t *)p32 = color;
    }
}

void gfx_fill_rect_alpha(canvas_t *c, int x, int y, int w, int h,
                         uint16_t color, int alpha16)
{
    if (!clip_rect(c, &x, &y, &w, &h))
        return;
    for (int j = 0; j < h; j++)
    {
        uint16_t *p = &c->px[(y + j) * SCREEN_W + x];
        for (int i = 0; i < w; i++)
            p[i] = gfx_blend(p[i], color, alpha16);
    }
}

void gfx_hline(canvas_t *c, int x, int y, int w, uint16_t color)
{
    gfx_fill_rect(c, x, y, w, 1, color);
}

void gfx_vline(canvas_t *c, int x, int y, int h, uint16_t color)
{
    gfx_fill_rect(c, x, y, 1, h, color);
}

void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    gfx_hline(c, x, y, w, color);
    gfx_hline(c, x, y + h - 1, w, color);
    gfx_vline(c, x, y + 1, h - 2, color);
    gfx_vline(c, x + w - 1, y + 1, h - 2, color);
}

void gfx_gradient_v(canvas_t *c, int x, int y, int w, int h,
                    uint16_t top, uint16_t bottom)
{
    if (h <= 0)
        return;
    for (int j = 0; j < h; j++)
    {
        int a = (h == 1) ? 0 : (j * 16) / (h - 1);
        gfx_hline(c, x, y + j, w, gfx_blend(top, bottom, a));
    }
}

// Couverture (0..16) du pixel [dx, dx+1) x [dy, dy+1) par un disque de rayon
// r centré à l'origine. Échantillonnage 4x4, calculs en 1/8 de pixel.
static int corner_coverage(int dx, int dy, int r)
{
    int inside = 0;
    int r8 = r * 8;
    for (int sy = 0; sy < 4; sy++)
    {
        int fy = dy * 8 + sy * 2 + 1;
        for (int sx = 0; sx < 4; sx++)
        {
            int fx = dx * 8 + sx * 2 + 1;
            if (fx * fx + fy * fy <= r8 * r8)
                inside++;
        }
    }
    return inside;
}

static void round_rect_impl(canvas_t *c, int x, int y, int w, int h, int r,
                            uint16_t color, bool filled)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0)
    {
        if (filled)
            gfx_fill_rect(c, x, y, w, h, color);
        else
            gfx_rect(c, x, y, w, h, color);
        return;
    }

    if (filled)
    {
        gfx_fill_rect(c, x, y + r, w, h - 2 * r, color);
        gfx_fill_rect(c, x + r, y, w - 2 * r, r, color);
        gfx_fill_rect(c, x + r, y + h - r, w - 2 * r, r, color);
    }
    else
    {
        gfx_hline(c, x + r, y, w - 2 * r, color);
        gfx_hline(c, x + r, y + h - 1, w - 2 * r, color);
        gfx_vline(c, x, y + r, h - 2 * r, color);
        gfx_vline(c, x + w - 1, y + r, h - 2 * r, color);
    }

    // Les 4 coins, pixel par pixel avec anti-aliasing
    for (int j = 0; j < r; j++)
    {
        for (int i = 0; i < r; i++)
        {
            // Distance depuis le centre du coin (r, r) vers le pixel (i, j)
            int dx = r - 1 - i, dy = r - 1 - j;
            int a = corner_coverage(dx, dy, r);
            if (!filled)
            {
                int inner = (r > 1) ? corner_coverage(dx, dy, r - 1) : 0;
                a -= inner;
            }
            if (a <= 0)
                continue;
            put_pixel_alpha(c, x + i, y + j, color, a);
            put_pixel_alpha(c, x + w - 1 - i, y + j, color, a);
            put_pixel_alpha(c, x + i, y + h - 1 - j, color, a);
            put_pixel_alpha(c, x + w - 1 - i, y + h - 1 - j, color, a);
        }
    }
}

void gfx_fill_round_rect(canvas_t *c, int x, int y, int w, int h, int r,
                         uint16_t color)
{
    round_rect_impl(c, x, y, w, h, r, color, true);
}

void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r,
                    uint16_t color)
{
    round_rect_impl(c, x, y, w, h, r, color, false);
}

void gfx_fill_circle(canvas_t *c, int cx, int cy, int r, uint16_t color)
{
    gfx_fill_round_rect(c, cx - r, cy - r, r * 2, r * 2, r, color);
}

// ---------------------------------------------------------------------------
// Texte
// ---------------------------------------------------------------------------

uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp;
    int extra;

    if (p[0] < 0x80)      { cp = p[0];        extra = 0; }
    else if (p[0] < 0xC0) { cp = '?';         extra = 0; } // Octet invalide
    else if (p[0] < 0xE0) { cp = p[0] & 0x1F; extra = 1; }
    else if (p[0] < 0xF0) { cp = p[0] & 0x0F; extra = 2; }
    else                  { cp = p[0] & 0x07; extra = 3; }

    p++;
    for (int i = 0; i < extra; i++)
    {
        if ((*p & 0xC0) != 0x80)
        {
            cp = '?';
            break;
        }
        cp = (cp << 6) | (*p++ & 0x3F);
    }

    *s = (const char *)p;
    return cp;
}

static const font_glyph_t *find_glyph(const font_t *f, uint32_t cp)
{
    // Chemin rapide pour l'ASCII (les 95 premiers glyphes sont 0x20..0x7E)
    if (cp >= 0x20 && cp < 0x7F)
        return &f->glyphs[cp - 0x20];

    int lo = 0, hi = f->count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint32_t v = f->codepoints[mid];
        if (v == cp)
            return &f->glyphs[mid];
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return &f->glyphs['?' - 0x20];
}

static void draw_glyph(canvas_t *c, const font_t *f, const font_glyph_t *g,
                       int x, int y, uint16_t color)
{
    int w = g->width;
    if (w == 0)
        return;
    int stride = (w + 1) / 2;
    const uint8_t *src = f->data + g->offset;

    x += g->xoff;

    // Rejet rapide si totalement hors clipping
    if (x >= c->clip_x1 || x + w <= c->clip_x0 || y >= c->clip_y1 ||
        y + f->height <= c->clip_y0)
        return;

    for (int j = 0; j < f->height; j++, src += stride)
    {
        int py = y + j;
        if (py < c->clip_y0 || py >= c->clip_y1)
            continue;
        uint16_t *row = &c->px[py * SCREEN_W];
        for (int i = 0; i < w; i++)
        {
            int px = x + i;
            if (px < c->clip_x0 || px >= c->clip_x1)
                continue;
            int a = (src[i >> 1] >> ((i & 1) * 4)) & 0xF;
            if (a == 0)
                continue;
            // alpha 4 bits (0..15) -> 0..16
            row[px] = gfx_blend(row[px], color, a + (a >> 3));
        }
    }
}

int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *str,
             uint16_t color)
{
    while (*str)
    {
        uint32_t cp = utf8_next(&str);
        if (cp == '\n')
            break;
        const font_glyph_t *g = find_glyph(f, cp);
        draw_glyph(c, f, g, x, y, color);
        x += g->advance;
    }
    return x;
}

int gfx_text_width(const font_t *f, const char *str)
{
    int w = 0;
    while (*str)
    {
        uint32_t cp = utf8_next(&str);
        if (cp == '\n')
            break;
        w += find_glyph(f, cp)->advance;
    }
    return w;
}

void gfx_text_box(canvas_t *c, const font_t *f, int x, int y, int w,
                  align_t align, const char *str, uint16_t color)
{
    int tw = gfx_text_width(f, str);

    if (tw <= w)
    {
        int dx = 0;
        if (align == ALIGN_CENTER)
            dx = (w - tw) / 2;
        else if (align == ALIGN_RIGHT)
            dx = w - tw;
        gfx_text(c, f, x + dx, y, str, color);
        return;
    }

    // Trop long : on dessine caractère par caractère puis "…"
    const font_glyph_t *ell = find_glyph(f, 0x2026);
    int max = w - ell->advance;
    int px = x;
    const char *s = str;
    while (*s)
    {
        const char *prev = s;
        uint32_t cp = utf8_next(&s);
        const font_glyph_t *g = find_glyph(f, cp);
        if (px - x + g->advance > max)
        {
            s = prev;
            break;
        }
        draw_glyph(c, f, g, px, y, color);
        px += g->advance;
    }
    draw_glyph(c, f, ell, px, y, color);
}

int gfx_text_wrap(canvas_t *c, const font_t *f, int x, int y, int w, int h,
                  const char *str, uint16_t color)
{
    int line_y = y;
    const char *s = str;

    while (*s && line_y + f->height <= y + h)
    {
        // Cherche la coupure la plus longue qui tient dans w
        const char *p = s;
        const char *last_break = NULL;
        int width = 0;

        while (*p && *p != '\n')
        {
            const char *prev = p;
            uint32_t cp = utf8_next(&p);
            int adv = find_glyph(f, cp)->advance;
            if (width + adv > w)
            {
                p = prev;
                break;
            }
            width += adv;
            if (cp == ' ')
                last_break = p;
        }

        const char *end = p;
        if (*p && *p != '\n' && last_break != NULL)
            end = last_break;
        if (end == s && *s != '\n' && *s) // Mot plus large que la boîte
            utf8_next(&end);

        // Dessine [s, end)
        int px = x;
        const char *q = s;
        while (q < end)
        {
            uint32_t cp = utf8_next(&q);
            const font_glyph_t *g = find_glyph(f, cp);
            draw_glyph(c, f, g, px, line_y, color);
            px += g->advance;
        }

        s = end;
        if (*s == '\n')
            s++;
        while (*s == ' ')
            s++;
        line_y += f->height;
    }

    return line_y - y;
}
