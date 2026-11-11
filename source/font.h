// Polices bitmap anti-aliasées (alpha 4 bits), générées par tools/gen_font.py
#ifndef FONT_H__
#define FONT_H__

#include <stdint.h>

typedef struct {
    uint32_t offset;   // Offset dans data (lignes de ceil(width / 2) octets)
    uint8_t width;     // Largeur du bitmap (0 pour l'espace)
    int8_t xoff;       // Décalage horizontal du bitmap par rapport au stylo
    uint8_t advance;   // Avance du stylo après ce glyphe
    uint8_t reserved;
} font_glyph_t;

typedef struct {
    uint8_t height;    // Hauteur d'une ligne (= hauteur de chaque bitmap)
    uint8_t baseline;  // Position de la ligne de base depuis le haut
    uint16_t count;
    const uint32_t *codepoints; // Triés par ordre croissant
    const font_glyph_t *glyphs;
    const uint8_t *data;
} font_t;

extern const font_t font_small;
extern const font_t font_body;
extern const font_t font_title;

#endif // FONT_H__
