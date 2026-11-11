#!/usr/bin/env python3
# Génère source/font_data.c à partir de polices TTF/OTF (anti-aliasing 4 bits).
#
# Usage : python3 tools/gen_font.py [dossier_inter]
# Dépendance : Pillow. Le fichier généré est commité, donc ce script n'est
# nécessaire que si tu veux changer de police / taille.

import os
import sys

from PIL import Image, ImageDraw, ImageFont

INTER_DIR = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/inter"

# (nom C, fichier, taille en px)
FONTS = [
    ("font_small", "Inter-Medium.otf", 10),
    ("font_body", "Inter-Medium.otf", 12),
    ("font_title", "Inter-Bold.otf", 17),
]

# ASCII imprimable + Latin-1 (accents FR) + quelques symboles utiles
CODEPOINTS = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [
    0x0152, 0x0153,  # Œ œ
    0x2013, 0x2014,  # – —
    0x2018, 0x2019, 0x201C, 0x201D,  # ‘ ’ “ ”
    0x2022, 0x2026,  # • …
    0x20AC,  # €
    0x2190, 0x2191, 0x2192, 0x2193,  # ← ↑ → ↓
]

OUT = os.path.join(os.path.dirname(__file__), "..", "source", "font_data.c")


def gen(cname, path, size, out):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    height = ascent + descent

    glyphs = []
    data = bytearray()

    for cp in CODEPOINTS:
        ch = chr(cp)
        advance = int(round(font.getlength(ch)))
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w = max(0, r - l)
        offset = len(data)
        if w > 0:
            img = Image.new("L", (w, height), 0)
            ImageDraw.Draw(img).text((-l, ascent), ch, font=font, fill=255, anchor="ls")
            px = img.load()
            for y in range(height):
                row = [min(15, (px[x, y] + 8) // 17) for x in range(w)]
                if len(row) & 1:
                    row.append(0)
                for i in range(0, len(row), 2):
                    data.append(row[i] | (row[i + 1] << 4))
        glyphs.append((cp, offset, w, l, advance))

    out.write(f"// {cname}: {os.path.basename(path)} {size}px, hauteur {height}, baseline {ascent}\n")
    out.write(f"static const uint32_t {cname}_cps[] = {{\n")
    for i in range(0, len(glyphs), 12):
        out.write("    " + ", ".join(f"0x{g[0]:04X}" for g in glyphs[i:i + 12]) + ",\n")
    out.write("};\n\n")
    out.write(f"static const font_glyph_t {cname}_glyphs[] = {{\n")
    for cp, off, w, l, adv in glyphs:
        out.write(f"    {{ {off}, {w}, {l}, {adv}, 0 }}, // U+{cp:04X}\n")
    out.write("};\n\n")
    out.write(f"static const uint8_t {cname}_data[] __attribute__((aligned(4))) = {{\n")
    for i in range(0, len(data), 16):
        out.write("    " + ", ".join(f"0x{v:02X}" for v in data[i:i + 16]) + ",\n")
    out.write("};\n\n")
    out.write(f"const font_t {cname} = {{\n")
    out.write(f"    .height = {height},\n    .baseline = {ascent},\n")
    out.write(f"    .count = {len(glyphs)},\n")
    out.write(f"    .codepoints = {cname}_cps,\n    .glyphs = {cname}_glyphs,\n")
    out.write(f"    .data = {cname}_data,\n}};\n\n")
    print(f"{cname}: {len(glyphs)} glyphes, {len(data)} octets")


with open(OUT, "w") as out:
    out.write("// Fichier généré par tools/gen_font.py — ne pas modifier à la main.\n")
    out.write("// Police : Inter (SIL Open Font License 1.1), https://rsms.me/inter\n\n")
    out.write('#include "font.h"\n\n')
    for cname, fname, size in FONTS:
        gen(cname, os.path.join(INTER_DIR, fname), size, out)
