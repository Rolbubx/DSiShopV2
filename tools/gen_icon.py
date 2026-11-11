#!/usr/bin/env python3
# Génère icon.gif (32x32, 16 couleurs max) : l'icône affichée dans les menus.
from PIL import Image, ImageDraw

S = 8  # suréchantillonnage
img = Image.new("RGB", (32 * S, 32 * S), (16, 20, 30))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1 * S, 1 * S, 31 * S - 1, 31 * S - 1], radius=7 * S, fill=(36, 104, 200))
# Sac de shopping
d.rounded_rectangle([7 * S, 11 * S, 25 * S, 27 * S], radius=3 * S, fill=(236, 240, 248))
d.arc([11 * S, 5 * S, 21 * S, 16 * S], start=180, end=360, fill=(236, 240, 248), width=2 * S)
d.ellipse([12 * S, 14 * S, 14 * S, 16 * S], fill=(36, 104, 200))
d.ellipse([18 * S, 14 * S, 20 * S, 16 * S], fill=(36, 104, 200))
img = img.resize((32, 32), Image.LANCZOS)
img.quantize(colors=16).save("icon.gif")
print("icon.gif généré")
