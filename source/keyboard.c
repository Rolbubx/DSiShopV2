// Clavier virtuel AZERTY

#include <string.h>

#include "keyboard.h"
#include "ui.h"

#define ROWS 3

static const char *const layout_lower[ROWS] = {
    "azertyuiop", "qsdfghjklm", "wxcvbn./",
};
static const char *const layout_upper[ROWS] = {
    "AZERTYUIOP", "QSDFGHJKLM", "WXCVBN-_",
};
static const char *const layout_sym1[ROWS] = {
    "1234567890", "@#&_-+()/*", ":;,.!?'\"",
};
static const char *const layout_sym2[ROWS] = {
    "1234567890", "~`|\\^$[]{}", "%=<>!?'\"",
};

static struct {
    char *buf;
    size_t cap;
    bool shift;
    bool caps;     // Verrouillage (double appui sur Maj)
    bool symbols;
    unsigned int last_shift_frame;
    unsigned int frame;
} kbd;

void kbd_open(char *buffer, size_t capacity)
{
    kbd.buf = buffer;
    kbd.cap = capacity;
    kbd.shift = false;
    kbd.caps = false;
    kbd.symbols = false;
    ui_reset_focus();
}

static void type_char(char ch)
{
    size_t len = strlen(kbd.buf);
    if (len + 1 < kbd.cap)
    {
        kbd.buf[len] = ch;
        kbd.buf[len + 1] = '\0';
    }
    if (kbd.shift && !kbd.caps)
        kbd.shift = false;
}

static void backspace(void)
{
    size_t len = strlen(kbd.buf);
    if (len > 0)
        kbd.buf[len - 1] = '\0';
}

static void toggle_shift(void)
{
    // Double appui rapide = verrouillage majuscules
    if (kbd.shift && !kbd.caps && kbd.frame - kbd.last_shift_frame < 20)
    {
        kbd.caps = true;
    }
    else if (kbd.shift)
    {
        kbd.shift = false;
        kbd.caps = false;
    }
    else
    {
        kbd.shift = true;
    }
    kbd.last_shift_frame = kbd.frame;
}

// Touche "caractère" : petit bouton plat
static bool key(canvas_t *c, rect_t r, const char *label, bool highlight)
{
    bool pressed, focused;
    bool clicked = ui_hit(r, true, &pressed, &focused);

    uint16_t bg = highlight ? COL_ACCENT_DK : COL_PANEL_HI;
    if (pressed)
        bg = COL_ACCENT;

    if (focused)
        gfx_round_rect(c, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 4, COL_ACCENT);
    gfx_fill_round_rect(c, r.x, r.y, r.w, r.h, 3, bg);

    gfx_text_box(c, &font_body, r.x + 1, r.y + (r.h - font_body.height) / 2,
                 r.w - 2, ALIGN_CENTER, label, COL_TEXT);

    // Aperçu agrandi au-dessus de la touche pressée
    if (pressed && label[1] == '\0')
    {
        int pw = 26, ph = 30;
        int px = r.x + (r.w - pw) / 2, py = r.y - ph - 2;
        gfx_fill_round_rect(c, px, py, pw, ph, 5, COL_ACCENT);
        gfx_text_box(c, &font_title, px, py + (ph - font_title.height) / 2,
                     pw, ALIGN_CENTER, label, COL_TEXT);
    }

    return clicked;
}

kbd_result_t kbd_update(canvas_t *c, const input_t *in, int y)
{
    kbd.frame++;

    kbd_result_t result = KBD_EDITING;

    if (in->repeat & BTN_B)
        backspace();
    if (in->down & BTN_SELECT)
        toggle_shift();
    if (in->down & BTN_START)
        result = KBD_ACCEPTED;

    const char *const *layout;
    if (kbd.symbols)
        layout = kbd.shift ? layout_sym2 : layout_sym1;
    else
        layout = kbd.shift ? layout_upper : layout_lower;

    const int kw = 23, kh = 24, gap = 2;
    const int pitch = kw + gap;

    gfx_fill_rect(c, 0, y - 4, SCREEN_W, SCREEN_H - y + 4, COL_BG2);
    gfx_hline(c, 0, y - 4, SCREEN_W, COL_BORDER);

    // Rangées de caractères (la 3e a aussi Maj et Effacer, même largeur)
    for (int row = 0; row < ROWS; row++)
    {
        const char *keys = layout[row];
        int n = (int)strlen(keys);
        int count = n + ((row == 2) ? 2 : 0);
        int x = (SCREEN_W - (count * pitch - gap)) / 2;
        int ky = y + row * (kh + gap);

        if (row == 2)
        {
            const char *label = kbd.caps ? "MAJ" : "Maj";
            if (kbd.symbols)
                label = kbd.shift ? "2/2" : "1/2";
            if (key(c, RECT(x, ky, kw, kh), label, kbd.shift))
                toggle_shift();
            x += pitch;
        }

        for (int i = 0; i < n; i++)
        {
            char label[2] = { keys[i], '\0' };
            if (key(c, RECT(x, ky, kw, kh), label, false))
                type_char(keys[i]);
            x += pitch;
        }

        if (row == 2)
        {
            if (key(c, RECT(x, ky, kw, kh), "\xE2\x86\x90", false)) // ←
                backspace();
        }
    }

    // Dernière rangée : symboles, espace, annuler, OK
    int ky = y + ROWS * (kh + gap);
    int x = (SCREEN_W - (10 * pitch - gap)) / 2;
    int full = 10 * pitch - gap;

    int w_sym = 2 * pitch - gap;
    int w_ok = 2 * pitch - gap;
    int w_cancel = 2 * pitch - gap;
    int w_space = full - w_sym - w_ok - w_cancel - 3 * gap;

    if (key(c, RECT(x, ky, w_sym, kh), kbd.symbols ? "abc" : "123", kbd.symbols))
    {
        kbd.symbols = !kbd.symbols;
        kbd.shift = false;
        kbd.caps = false;
    }
    x += w_sym + gap;
    if (key(c, RECT(x, ky, w_space, kh), "espace", false))
        type_char(' ');
    x += w_space + gap;
    if (key(c, RECT(x, ky, w_cancel, kh), "Annuler", false))
        result = KBD_CANCELLED;
    x += w_cancel + gap;
    if (key(c, RECT(x, ky, w_ok, kh), "OK", true))
        result = KBD_ACCEPTED;

    return result;
}
