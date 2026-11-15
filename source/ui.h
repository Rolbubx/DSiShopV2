// Mini-toolkit d'interface en mode immédiat (boutons, listes, en-têtes...).
// Gère le tactile et la navigation à la croix (focus 2D + A pour valider).
#ifndef UI_H__
#define UI_H__

#include <stdbool.h>

#include "gfx.h"
#include "input.h"

// Thème (sombre ou clair, modifiable à l'exécution : voir ui_set_theme)
typedef struct {
    uint16_t bg, bg2, panel, panel_hi, panel_down, border;
    uint16_t accent, accent_dk;
    uint16_t text, text_dim, text_off;
    uint16_t ok, warn, err;
} theme_t;

extern theme_t ui_theme;

#define COL_BG         (ui_theme.bg)
#define COL_BG2        (ui_theme.bg2)
#define COL_PANEL      (ui_theme.panel)
#define COL_PANEL_HI   (ui_theme.panel_hi)
#define COL_PANEL_DOWN (ui_theme.panel_down)
#define COL_BORDER     (ui_theme.border)
#define COL_ACCENT     (ui_theme.accent)
#define COL_ACCENT_DK  (ui_theme.accent_dk)
#define COL_TEXT       (ui_theme.text)
#define COL_TEXT_DIM   (ui_theme.text_dim)
#define COL_TEXT_OFF   (ui_theme.text_off)
#define COL_OK         (ui_theme.ok)
#define COL_WARN       (ui_theme.warn)
#define COL_ERR        (ui_theme.err)
// Texte sur fond bleu / rouge / sombre : blanc dans les deux thèmes
#define COL_WHITE      COLOR_RGB(255, 255, 255)

void ui_set_theme(bool light);
bool ui_theme_is_light(void);

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
