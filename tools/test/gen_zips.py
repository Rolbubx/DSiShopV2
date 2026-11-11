#!/usr/bin/env python3
# Génère les archives de test de zip.c / splitfile.c, découpées en morceaux
# comme le fait l'installeur : <nom>.000, <nom>.001...
#
# Usage : python3 gen_zips.py <dossier> [--big]
#   --big : ajoute une archive ZIP64 de plus de 4 Go (fichiers creux/sparse)

import io
import os
import random
import struct
import sys
import zipfile
import zlib

out = sys.argv[1]
big = "--big" in sys.argv
os.makedirs(out, exist_ok=True)

rnd = random.Random(1234)
rom = bytes(rnd.getrandbits(8) for _ in range(300_000)) + bytes(200_000)
other = b"pas une rom\n" * 1000
open(os.path.join(out, "rom.bin"), "wb").write(rom)


def write_parts(name, data, chunk, base_offset=0, total=None):
    """Écrit data à l'offset base_offset d'un fichier virtuel découpé."""
    total = total if total is not None else base_offset + len(data)
    nparts = (total + chunk - 1) // chunk
    for n in range(nparts):
        path = os.path.join(out, f"{name}.{n:03d}")
        size = min(chunk, total - n * chunk)
        with open(path, "wb") as f:
            f.truncate(size)  # Creux : ne prend pas de place disque
            start, end = n * chunk, n * chunk + size
            lo, hi = max(start, base_offset), min(end, base_offset + len(data))
            if lo < hi:
                f.seek(lo - start)
                f.write(data[lo - base_offset:hi - base_offset])


def make_zip(method, entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=method) as z:
        for name, data in entries:
            z.writestr(name, data)
    return buf.getvalue()


# 1. Deflate, plusieurs fichiers, la ROM dans un sous-dossier, petits morceaux
z = make_zip(zipfile.ZIP_DEFLATED, [("LISEZMOI.txt", other), ("jeu/MonJeu.dsi", other),
                                     ("jeu/MonJeu.nds", rom)])
write_parts("deflate", z, 1000)

# 2. Stored (sans compression), morceaux de 4096 octets
z = make_zip(zipfile.ZIP_STORED, [("Truc.nds", rom)])
write_parts("stored", z, 4096)

# 3. Pas de ROM dedans
z = make_zip(zipfile.ZIP_DEFLATED, [("doc.txt", other)])
write_parts("norom", z, 1 << 20)

# 4. Archive corrompue (un octet de données modifié)
z = bytearray(make_zip(zipfile.ZIP_STORED, [("Casse.nds", rom)]))
z[100] ^= 0xFF
write_parts("corrupt", bytes(z), 1 << 20)


def zip64_archive(base, name, data):
    """Archive ZIP64 minimale dont la ROM commence à l'offset `base`."""
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    cdata = comp.compress(data) + comp.flush()
    crc = zlib.crc32(data)
    fname = name.encode()

    extra_local = struct.pack("<HHQQ", 1, 16, len(data), len(cdata))
    local = struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, 8, 0, 0, crc,
                        0xFFFFFFFF, 0xFFFFFFFF, len(fname), len(extra_local))
    local += fname + extra_local

    extra_cd = struct.pack("<HHQQQ", 1, 24, len(data), len(cdata), base)
    cd = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45, 0, 8, 0, 0, crc,
                     0xFFFFFFFF, 0xFFFFFFFF, len(fname), len(extra_cd), 0, 0, 0,
                     0, 0xFFFFFFFF)
    cd += fname + extra_cd

    cd_off = base + len(local) + len(cdata)
    eocd64_off = cd_off + len(cd)
    eocd64 = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, 1, 1,
                         len(cd), cd_off)
    locator = struct.pack("<IIQI", 0x07064b50, 0, eocd64_off, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xFFFF, 0xFFFF,
                       0xFFFFFFFF, 0xFFFFFFFF, 0)
    return local + cdata + cd + eocd64 + locator + eocd


if big:
    # 5. ZIP64 > 4 Go : la ROM est placée à 4,5 Gio, morceaux de 1 Gio
    base = 4 * (1 << 30) + (1 << 29)
    blob = zip64_archive(base, "Gros/MonGrosJeu.nds", rom)
    write_parts("big", blob, 1 << 30, base_offset=base)

    # Vérification croisée avec le module zipfile de Python
    class Joined(io.RawIOBase):
        def __init__(self, name, chunk):
            self.parts = sorted(p for p in os.listdir(out) if p.startswith(name + "."))
            self.sizes = [os.path.getsize(os.path.join(out, p)) for p in self.parts]
            self.chunk, self.pos, self.name = chunk, 0, name
        def seekable(self): return True
        def readable(self): return True
        def seek(self, off, whence=0):
            total = sum(self.sizes)
            self.pos = off if whence == 0 else (self.pos + off if whence == 1 else total + off)
            return self.pos
        def tell(self): return self.pos
        def readinto(self, b):
            n = self.pos // self.chunk
            if n >= len(self.parts): return 0
            with open(os.path.join(out, self.parts[n]), "rb") as f:
                f.seek(self.pos % self.chunk)
                d = f.read(min(len(b), self.chunk - self.pos % self.chunk))
            b[:len(d)] = d
            self.pos += len(d)
            return len(d)

    with zipfile.ZipFile(io.BufferedReader(Joined("big", 1 << 30))) as zf:
        assert zf.read("Gros/MonGrosJeu.nds") == rom, "zipfile ne relit pas l'archive"
    print("big: archive ZIP64 de", (base + len(blob)) / (1 << 30), "Gio, relue par Python")

print("archives générées dans", out)
