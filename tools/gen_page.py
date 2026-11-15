#!/usr/bin/env python3
# Intègre la page du Remote local (source/web/*) dans la ROM :
# génère source/lremote_page.c. À relancer après avoir modifié source/web/.
import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "source")
FILES = [
    ("lremote_index_html", "web/index.html"),
    ("lremote_app_js", "web/app.js"),
    ("lremote_style_css", "web/style.css"),
]

def c_string(data: bytes) -> str:
    out = []
    for line in data.split(b"\n"):
        s = "".join(
            chr(b) if 32 <= b < 127 and chr(b) not in '"\\?' else f"\\{b:03o}"
            for b in line
        )
        out.append(f'    "{s}\\n"')
    return "\n".join(out)

with open(os.path.join(ROOT, "lremote_page.c"), "w") as f:
    f.write("// Généré par tools/gen_page.py à partir de source/web/ — ne pas modifier.\n\n")
    f.write('#include "lremote_page.h"\n\n')
    for name, path in FILES:
        data = open(os.path.join(ROOT, path), "rb").read().rstrip(b"\n")
        f.write(f"const char {name}[] =\n{c_string(data)};\n\n")
print("source/lremote_page.c généré")
