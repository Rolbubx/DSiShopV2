#!/usr/bin/env python3
# Test de bout en bout de la mise à jour de TWiLight Menu++ : les paquets sont
# fabriqués par le vrai code du serveur (catalog-server/catalog.py) puis
# appliqués par le vrai code de la console (sysupdate.c + zip.c) sur une
# fausse carte SD.
#
# Usage : python3 test_sysupdate.py <./test_sysupdate> <dossier de travail>
#         [dossier d'une vraie release extraite]

import os
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "catalog-server"))
import catalog  # noqa: E402

binary, work = sys.argv[1], os.path.join(sys.argv[2], "sysupdate")
real = sys.argv[3] if len(sys.argv) > 3 else None
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)

NEW = {
    "BOOT.NDS": b"nouveau boot" * 500,
    "version.txt": b"TWiLight Menu++: v2.0.0\nnds-bootstrap: Release v1.0.0\n",
    "_nds/TWiLightMenu/main.srldr": os.urandom(200_000),
    "_nds/TWiLightMenu/manual.srldr": os.urandom(900_000),
    "_nds/TWiLightMenu/settings.srldr": os.urandom(150_000),
    "_nds/TWiLightMenu/extras/fonts/Default/large.nftr": os.urandom(120_000),
    "_nds/colorLut/[10000K] Aquarium.lut": bytes(65536),
    "_nds/colorLut/Default.lut": b"",
    "_nds/nds-bootstrap-release.nds": os.urandom(100_000),
    "ntrboot.nds": b"ntrboot v2",
    "roms/nds/Place NDS ROMs here": b"",
    "title/00030004/53524c41/content/title.tmd": b"tmd v2",
    "title/00030004/53524c41/content/00000000.app": b"app v2",
}
OLD = {
    "BOOT.NDS": b"ancien boot",
    "version.txt": b"TWiLight Menu++: v1.0.0\r\n",
    "_nds/TWiLightMenu/main.srldr": b"ancien main",
    "_nds/TWiLightMenu/settings.ini": b"[SRLOADER]\nTHEME=3\n",   # réglages : à garder
    "_nds/TWiLightMenu/manual.srldr": b"ancien manuel",
    "roms/nds/MonJeu.nds": b"un jeu",
    "roms/nds/saves/MonJeu.sav": b"une sauvegarde",
}


def write_tree(root, files):
    for rel, data in files.items():
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


def read_tree(root):
    out = {}
    for base, _, names in os.walk(root):
        for name in names:
            full = os.path.join(base, name)
            with open(full, "rb") as f:
                out[os.path.relpath(full, root).replace(os.sep, "/")] = f.read()
    return out


def build(release_dir, tag):
    """Paquets fabriqués par le serveur, sans GitHub ni 7-Zip."""
    catalog.TWILIGHT_DIR = os.path.join(work, "server")
    catalog.twl_download = lambda url, dest: open(dest, "wb").close()
    catalog.twl_extract = lambda archive, dest: shutil.copytree(release_dir, dest,
                                                                dirs_exist_ok=True)
    manifest = catalog.twl_build(tag, "fake://release")
    return os.path.join(catalog.TWILIGHT_DIR, tag), manifest


def run(packs, sd, mode):
    r = subprocess.run([binary, packs, sd, mode], capture_output=True, text=True)
    lines = r.stdout.strip().split("\n")
    assert r.returncode in (0, 2), r.stdout + r.stderr
    return r.returncode, lines, [l[6:] for l in lines if l.startswith("fetch ")]


def check(cond, what):
    if not cond:
        print("  ÉCHEC :", what)
        sys.exit(1)


release = os.path.join(work, "release")
write_tree(release, NEW)
catalog.TWL_PACK_SIZE = 300_000
packs, manifest = build(release, "v2.0.0")
names = [l.split("\t")[0] for l in manifest.strip().split("\n")[1:]]
print("paquets :", " ".join(names))
check(len(names) >= 6 and names[-1] == "boot.zip", "découpage inattendu")
expected_new = {k: v for k, v in NEW.items() if not k.startswith("roms/")}

# 1. Carte SD classique (ni hiyaCFW ni ntrboot) : les paquets optionnels sont sautés
print("mise à jour complète")
sd = os.path.join(work, "sd1")
write_tree(sd, OLD)
code, lines, fetched = run(packs, sd, "apply")
check(code == 0 and lines[-1] == "done", "échec : %s" % lines[-1])
check("installed v1.0.0" in lines and "latest v2.0.0" in lines and "present 1" in lines,
      "versions mal lues : %s" % lines)
check(fetched == [n for n in names if n.startswith(("main", "boot"))],
      "paquets téléchargés : %s" % fetched)
got = read_tree(sd)
for rel, data in expected_new.items():
    if rel.startswith("title/") or rel == "ntrboot.nds":
        check(rel not in got, "%s écrit sans que la condition soit remplie" % rel)
    else:
        check(got.get(rel) == data, "%s différent" % rel)
for rel in ("_nds/TWiLightMenu/settings.ini", "roms/nds/MonJeu.nds", "roms/nds/saves/MonJeu.sav"):
    check(got.get(rel) == OLD[rel], "%s a été touché" % rel)
check(not any(".dsitmp" in k or k.startswith("_dsishop/") for k in got),
      "fichiers temporaires restants : %s" % [k for k in got if "dsi" in k])

# 2. hiyaCFW + ntrboot présents : tout est mis à jour
print("carte avec hiyaCFW et ntrboot")
sd = os.path.join(work, "sd2")
write_tree(sd, OLD)
write_tree(sd, {"ntrboot.nds": b"vieux", "title/00030004/53524c41/content/title.tmd": b"vieux"})
code, lines, fetched = run(packs, sd, "apply")
check(code == 0 and fetched == names, "paquets téléchargés : %s" % fetched)
got = read_tree(sd)
check(all(got.get(rel) == data for rel, data in expected_new.items()), "contenu différent")

# 3. Coupure pendant le 3e paquet, puis reprise
print("coupure puis reprise")
sd = os.path.join(work, "sd3")
write_tree(sd, OLD)
code, lines, fetched = run(packs, sd, "cancel:2")
check(code == 2 and fetched == names[:3], "annulation : %s" % lines)
got = read_tree(sd)
check(got["BOOT.NDS"] == OLD["BOOT.NDS"] and got["version.txt"] == OLD["version.txt"],
      "BOOT.NDS / version.txt remplacés avant la fin")
check(not any(".dsitmp" in k or "twl_pack" in k for k in got), "fichiers temporaires restants")
code, lines, fetched = run(packs, sd, "apply")
check(code == 0 and "resume 2" in lines, "reprise : %s" % lines)
check(fetched == [n for n in names[2:] if n.startswith(("main", "boot"))],
      "paquets retéléchargés : %s" % fetched)
got = read_tree(sd)
check(all(got.get(rel) == data for rel, data in expected_new.items()
          if not rel.startswith("title/") and rel != "ntrboot.nds"), "contenu différent")
code, lines, fetched = run(packs, sd, "apply")
check("resume 0" in lines and "installed v2.0.0" in lines, "état non remis à zéro : %s" % lines)

# 4. Paquet abîmé pendant le téléchargement : refusé, rien n'est écrit
print("paquet corrompu")
sd = os.path.join(work, "sd4")
write_tree(sd, OLD)
code, lines, fetched = run(packs, sd, "corrupt:0")
check(code == 2 and "corrompu" in lines[-1], "corruption non détectée : %s" % lines)
check(read_tree(sd) == OLD, "la carte a été modifiée")

# 5. Optionnel : une vraie release extraite
if real:
    print("vraie release :", real)
    catalog.TWL_PACK_SIZE = 8 * 1024 * 1024
    packs, manifest = build(real, "v0.0.0-test")
    print(manifest.replace("\t", "  "), end="")
    sd = os.path.join(work, "sd5")
    write_tree(sd, OLD)
    write_tree(sd, {"ntrboot.nds": b"vieux", "title/00030004/53524c41/content/title.tmd": b"x"})
    code, lines, fetched = run(packs, sd, "apply")
    check(code == 0, "échec : %s" % lines[-1])
    got, ref = read_tree(sd), read_tree(real)
    for rel, data in ref.items():
        if not rel.startswith("roms/"):
            check(got.get(rel) == data, "%s différent" % rel)
    print("  %d fichiers identiques à la release" % len([r for r in ref if not r.startswith("roms/")]))

print("\nTout est OK")
