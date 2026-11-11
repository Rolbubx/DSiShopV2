// Mini-toolkit d'interface en mode immédiat (boutons, listes, en-têtes...).
// Gère le tactile et la navigation à la croix (focus 2D + A pour valider).
#ifndef UI_H__
#define UI_H__

#include <stdbool.h>

#include "gfx.h"
#include "input.h"

// Thème
#define COL_BG         RGB8(16, 20, 30)
#define COL_BG2        RGB8(24, 30, 46)
#define COL_PANEL      RGB8(34, 42, 62)
#define COL_PANEL_HI   RGB8(48, 60, 88)
#define COL_PANEL_DOWN RGB8(26, 32, 48)
#define COL_BORDER     RGB8(60, 72, 104)
#define COL_ACCENT     RGB8(64, 156, 255)
#define COL_ACCENT_DK  RGB8(36, 104, 200)
#define COL_TEXT       RGB8(236, 240, 248)
#define COL_TEXT_DIM   RGB8(150, 160, 184)
#define COL_TEXT_OFF   RGB8(96, 104, 128)
#define COL_OK         RGB8(80, 210, 130)
#define COL_WARN       RGB8(250, 190, 70)
#define COL_ERR        RGB8(250, 96, 96)

typedef struct {
    int x, y, w, h;
} rect_t;

static inline rect_t RECT(int x, int y, int w, int h)
{
    rect_t r = { x, y, w, h };
    return r;
}

typedef enum {
    BTN_STYLE_NORMAL,
    BTN_STYLE_PRIMARY,
    BTN_STYLE_DANGER,
} btn_style_t;

// À appeler au début et à la fin de chaque frame qui dessine l'UI
void ui_begin(const input_t *in);
void ui_end(void);

// Remet le focus sur le premier widget (à appeler en changeant d'écran)
void ui_reset_focus(void);

// Vrai si l'UI doit être redessinée (interaction en cours)
bool ui_wants_redraw(void);

// Gestion bas niveau d'une zone interactive. Renvoie true si activée.
bool ui_hit(rect_t r, bool enabled, bool *pressed, bool *focused);

bool ui_button(canvas_t *c, rect_t r, const char *label, btn_style_t style,
               bool enabled);
// Élément de liste : titre + sous-titre + texte à droite optionnel
bool ui_list_item(canvas_t *c, rect_t r, const char *title,
                  const char *subtitle, const char *right, uint16_t right_col);

// Éléments non interactifs
void ui_header(canvas_t *c, const char *title, const char *right);
void ui_spinner(canvas_t *c, int cx, int cy, int r, unsigned int frame,
                uint16_t color);
void ui_wifi_icon(canvas_t *c, int x, int y, int bars, uint16_t on,
                  uint16_t off);
void ui_panel(canvas_t *c, rect_t r);

// Petit message temporaire en haut de l'écran du bas
void ui_toast(const char *msg);
void ui_draw_toast(canvas_t *c);

#endif // UI_H__
