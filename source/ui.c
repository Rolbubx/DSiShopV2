// Mini-toolkit d'interface en mode immédiat

#include <stdio.h>
#include <string.h>

#include "ui.h"

#define MAX_WIDGETS 64

static struct {
    const input_t *in;

    // Widgets de la frame en cours / précédente (pour la navigation)
    int count;
    rect_t rects[MAX_WIDGETS];
    bool enabled[MAX_WIDGETS];
    int prev_count;
    rect_t prev_rects[MAX_WIDGETS];
    bool prev_enabled[MAX_WIDGETS];

    int focus;        // Widget qui a le focus (navigation à la croix)
    bool show_focus;  // Le focus n'est affiché que si on utilise la croix
    int active;       // Widget sur lequel le stylet a été posé (-1 : aucun)
    int activate;     // Widget activé avec A cette frame (-1 : aucun)
    bool redraw;

    char toast[96];
    int toast_timer;
} ui = {
    .focus = 0,
    .active = -1,
    .activate = -1,
};

static bool rect_contains(rect_t r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Déplace le focus vers le widget le plus proche dans la direction donnée
static void navigate(int dx, int dy)
{
    if (ui.prev_count == 0)
        return;

    if (ui.focus < 0 || ui.focus >= ui.prev_count)
    {
        ui.focus = 0;
        return;
    }

    rect_t f = ui.prev_rects[ui.focus];
    int fx = f.x + f.w / 2, fy = f.y + f.h / 2;

    int best = -1, best_score = 0x7FFFFFFF;

    for (int i = 0; i < ui.prev_count; i++)
    {
        if (i == ui.focus || !ui.prev_enabled[i])
            continue;

        rect_t r = ui.prev_rects[i];
        int cx = r.x + r.w / 2 - fx, cy = r.y + r.h / 2 - fy;

        int primary = cx * dx + cy * dy;          // Distance dans la direction
        int secondary = dx ? cy : cx;              // Écart perpendiculaire
        if (secondary < 0)
            secondary = -secondary;

        if (primary <= 0)
            continue;

        int score = primary + secondary * 2;
        if (score < best_score)
        {
            best_score = score;
            best = i;
        }
    }

    if (best >= 0)
        ui.focus = best;
}

void ui_begin(const input_t *in)
{
    ui.in = in;
    ui.count = 0;
    ui.activate = -1;
    ui.redraw = false;

    if (in->down & BTN_TOUCH)
    {
        ui.show_focus = false;
        ui.redraw = true;
    }

    uint32_t dirs = in->repeat & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
    if (dirs)
    {
        if (!ui.show_focus)
        {
            ui.show_focus = true;
        }
        else
        {
            if (dirs & BTN_UP)    navigate(0, -1);
            if (dirs & BTN_DOWN)  navigate(0, 1);
            if (dirs & BTN_LEFT)  navigate(-1, 0);
            if (dirs & BTN_RIGHT) navigate(1, 0);
        }
        ui.redraw = true;
    }

    if (in->down & BTN_A)
    {
        // Le premier appui sur A affiche simplement le focus
        if (ui.show_focus)
            ui.activate = ui.focus;
        ui.show_focus = true;
        ui.redraw = true;
    }

    if (in->touching || (in->up & BTN_TOUCH))
        ui.redraw = true;
}

void ui_end(void)
{
    if (ui.in->up & BTN_TOUCH)
        ui.active = -1;

    memcpy(ui.prev_rects, ui.rects, sizeof(rect_t) * ui.count);
    memcpy(ui.prev_enabled, ui.enabled, sizeof(bool) * ui.count);
    ui.prev_count = ui.count;

    // Si le widget qui avait le focus a disparu ou est désactivé, on en
    // cherche un autre
    if (ui.count > 0 && (ui.focus >= ui.count || !ui.enabled[ui.focus]))
    {
        ui.focus = 0;
        for (int i = 0; i < ui.count; i++)
        {
            if (ui.enabled[i])
            {
                ui.focus = i;
                break;
            }
        }
    }
}

void ui_reset_focus(void)
{
    ui.focus = 0;
    ui.active = -1;
    ui.prev_count = 0;
    ui.redraw = true;
    ui.toast_timer = 0;
}

bool ui_wants_redraw(void)
{
    return ui.redraw || ui.toast_timer > 0;
}

bool ui_hit(rect_t r, bool enabled, bool *pressed, bool *focused)
{
    int id = ui.count;
    if (ui.count < MAX_WIDGETS)
    {
        ui.rects[ui.count] = r;
        ui.enabled[ui.count] = enabled;
        ui.count++;
    }

    const input_t *in = ui.in;
    bool inside = rect_contains(r, in->tx, in->ty);
    bool clicked = false;

    if (enabled && (in->down & BTN_TOUCH) && inside)
    {
        ui.active = id;
        ui.focus = id;
    }

    if (ui.active == id && (in->up & BTN_TOUCH) && inside && enabled)
        clicked = true;

    if (ui.activate == id && enabled)
        clicked = true;

    if (pressed)
        *pressed = enabled && ui.active == id && in->touching && inside;
    if (focused)
        *focused = ui.show_focus && ui.focus == id;

    return clicked;
}

static void draw_focus_ring(canvas_t *c, rect_t r, int radius)
{
    gfx_round_rect(c, r.x - 2, r.y - 2, r.w + 4, r.h + 4, radius + 2, COL_ACCENT);
    gfx_round_rect(c, r.x - 1, r.y - 1, r.w + 2, r.h + 2, radius + 1, COL_ACCENT);
}

bool ui_button(canvas_t *c, rect_t r, const char *label, btn_style_t style,
               bool enabled)
{
    bool pressed, focused;
    bool clicked = ui_hit(r, enabled, &pressed, &focused);

    uint16_t bg, fg = COL_TEXT;
    switch (style)
    {
        case BTN_STYLE_PRIMARY: bg = COL_ACCENT_DK; break;
        case BTN_STYLE_DANGER:  bg = RGB8(150, 52, 64); break;
        default:                bg = COL_PANEL_HI; break;
    }

    if (!enabled)
    {
        bg = COL_PANEL_DOWN;
        fg = COL_TEXT_OFF;
    }
    else if (pressed)
    {
        bg = gfx_blend(bg, RGB8(0, 0, 0), 5);
    }

    const int radius = 6;
    if (focused)
        draw_focus_ring(c, r, radius);

    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, radius, bg);
    // Petit reflet en haut pour donner du relief
    if (enabled && !pressed)
        gfx_hline(c, r.x + radius, r.y + 1, r.w - radius * 2,
                  gfx_blend(bg, RGB8(255, 255, 255), 3));

    const font_t *f = &font_body;
    gfx_text_box(c, f, r.x + 6, r.y + (r.h - f->height) / 2 + (pressed ? 1 : 0),
                 r.w - 12, ALIGN_CENTER, label, fg);

    return clicked;
}

bool ui_list_item(canvas_t *c, rect_t r, const char *title,
                  const char *subtitle, const char *right, uint16_t right_col)
{
    bool pressed, focused;
    bool clicked = ui_hit(r, true, &pressed, &focused);

    const int radius = 4;
    if (focused)
        draw_focus_ring(c, r, radius);

    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, radius,
                        pressed ? COL_PANEL_DOWN : COL_PANEL);

    int right_w = 0;
    if (right && right[0])
    {
        right_w = gfx_text_width(&font_small, right) + 8;
        gfx_text(c, &font_small, r.x + r.w - right_w,
                 r.y + (r.h - font_small.height) / 2, right, right_col);
    }

    int text_w = r.w - 16 - right_w;
    if (subtitle && subtitle[0])
    {
        int total = font_body.height + font_small.height;
        int y = r.y + (r.h - total) / 2;
        gfx_text_box(c, &font_body, r.x + 8, y, text_w, ALIGN_LEFT, title, COL_TEXT);
        gfx_text_box(c, &font_small, r.x + 8, y + font_body.height, text_w,
                     ALIGN_LEFT, subtitle, COL_TEXT_DIM);
    }
    else
    {
        gfx_text_box(c, &font_body, r.x + 8, r.y + (r.h - font_body.height) / 2,
                     text_w, ALIGN_LEFT, title, COL_TEXT);
    }

    return clicked;
}

void ui_header(canvas_t *c, const char *title, const char *right)
{
    const int h = 22;
    gfx_gradient_v(c, 0, 0, SCREEN_W, h, COL_PANEL_HI, COL_PANEL);
    gfx_hline(c, 0, h, SCREEN_W, COL_BORDER);

    int right_w = 0;
    if (right && right[0])
    {
        right_w = gfx_text_width(&font_small, right);
        gfx_text(c, &font_small, SCREEN_W - 8 - right_w,
                 (h - font_small.height) / 2, right, COL_TEXT_DIM);
        right_w += 12;
    }

    gfx_text_box(c, &font_body, 8, (h - font_body.height) / 2,
                 SCREEN_W - 16 - right_w, ALIGN_LEFT, title, COL_TEXT);
}

void ui_spinner(canvas_t *c, int cx, int cy, int r, unsigned int frame,
                uint16_t color)
{
    // Positions des 8 points (cos/sin * 64)
    static const int8_t dirs[8][2] = {
        { 0, -64 }, { 45, -45 }, { 64, 0 }, { 45, 45 },
        { 0, 64 }, { -45, 45 }, { -64, 0 }, { -45, -45 },
    };

    unsigned int head = (frame / 5) % 8;

    for (int k = 0; k < 8; k++)
    {
        int age = (int)((head - k + 8) % 8); // 0 = point le plus récent
        int alpha = 16 - age * 2;
        int px = cx + dirs[k][0] * r / 64;
        int py = cy + dirs[k][1] * r / 64;
        gfx_fill_circle(c, px, py, 2, gfx_blend(COL_BG, color, alpha));
    }
}

void ui_wifi_icon(canvas_t *c, int x, int y, int bars, uint16_t on,
                  uint16_t off)
{
    for (int i = 0; i < 4; i++)
    {
        int h = 3 + i * 3;
        gfx_fill_rect(c, x + i * 4, y + 12 - h, 3, h, i < bars ? on : off);
    }
}

void ui_panel(canvas_t *c, rect_t r)
{
    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, 6, COL_BORDER);
    gfx_fill_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, COL_BG2);
}

void ui_toast(const char *msg)
{
    snprintf(ui.toast, sizeof(ui.toast), "%s", msg);
    ui.toast_timer = 120;
}

void ui_draw_toast(canvas_t *c)
{
    if (ui.toast_timer <= 0)
        return;
    ui.toast_timer--;

    const font_t *f = &font_small;
    int w = gfx_text_width(f, ui.toast) + 20;
    if (w > SCREEN_W - 16)
        w = SCREEN_W - 16;
    int h = f->height + 10;
    int x = (SCREEN_W - w) / 2, y = 26;

    gfx_fill_round_rect(c, x, y, w, h, h / 2, RGB8(8, 10, 16));
    gfx_round_rect(c, x, y, w, h, h / 2, COL_BORDER);
    gfx_text_box(c, f, x + 10, y + 5, w - 20, ALIGN_CENTER, ui.toast, COL_TEXT);
}
