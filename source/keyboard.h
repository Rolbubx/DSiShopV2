// Clavier virtuel AZERTY dessiné sur l'écran du bas (saisie ASCII)
#ifndef KEYBOARD_H__
#define KEYBOARD_H__

#include <stddef.h>

#include "gfx.h"
#include "input.h"

typedef enum {
    KBD_EDITING,
    KBD_ACCEPTED,
    KBD_CANCELLED,
} kbd_result_t;

// Commence l'édition du buffer (le contenu actuel est conservé)
void kbd_open(char *buffer, size_t capacity);

// Dessine le clavier à partir de y et traite les entrées.
// Raccourcis : B = effacer, START = valider, SELECT = majuscules
kbd_result_t kbd_update(canvas_t *c, const input_t *in, int y);

#endif // KEYBOARD_H__
