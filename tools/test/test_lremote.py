#!/usr/bin/env python3
# Tests HTTP du Remote local (serveur C réel, sockets, sessions, sécurité).
#   make -C tools/test check   (ou : python3 test_lremote.py ./lremote_pc <dossier>)
import http.client
import json
import os
import subprocess
import sys
import time

binary, workdir = sys.argv[1], sys.argv[2]
port = 8097
games = os.path.join(workdir, "lremote_games")
os.makedirs(games, exist_ok=True)
for name, size in (("Pong.nds", 1000), ("Zelda.nds", 2000), ("notes.txt", 10)):
    with open(os.path.join(games, name), "wb") as f:
        f.write(b"\0" * size)
config = os.path.join(workdir, "lremote.cfg")
if os.path.exists(config):
    os.remove(config)

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("  ÉCHEC :", msg)


def req(method, path, body=None, cookie=None, csrf=True, host=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = {"Host": host or f"192.168.1.42:{port}"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    if csrf:
        headers["X-DSiShop"] = "1"
    if cookie:
        headers["Cookie"] = cookie
    c.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, dict(r.getheaders()), data


proc = subprocess.Popen([binary, str(port), games, config, "alice", "motdepasse123"],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    assert proc.stdout.readline().strip() == "PRET", "le serveur n'a pas démarré"

    print("page et fichiers statiques")
    st, h, body = req("GET", "/")
    check(st == 200 and b"Remote local" in body, f"page : {st}")
    check("default-src 'self'" in h.get("Content-Security-Policy", ""), "CSP")
    st, _, body = req("GET", "/app.js")
    check(st == 200 and b"api('POST', 'login'" in body, "app.js")
    check(req("GET", "/style.css")[0] == 200, "style.css")
    check(req("GET", "/../lremote.cfg")[0] == 404, "pas d'accès aux fichiers")

    print("sécurité")
    check(req("GET", "/", host="evil.example.com")[0] == 403, "DNS rebinding : hôte refusé")
    check(req("GET", "/api/status")[0] == 401, "statut sans session")
    check(req("POST", "/api/login", {"user": "alice", "pass": "motdepasse123"},
              csrf=False)[0] == 403, "CSRF : en-tête obligatoire")
    st, _, body = req("POST", "/api/login", {"user": "alice", "pass": "faux"})
    check(st == 401, f"mauvais mot de passe : {st}")
    check(req("POST", "/api/login", {"user": "bob", "pass": "motdepasse123"})[0] == 401,
          "mauvais identifiant")
    check(req("GET", "/api/status", cookie="dsishop_local=" + "a" * 64)[0] == 401,
          "faux cookie")
    with open(config) as f:
        cfg = f.read()
    check("motdepasse123" not in cfg and "hash\t" in cfg, "mot de passe stocké en clair !")

    print("connexion et statut")
    st, h, body = req("POST", "/api/login", {"user": "alice", "pass": "motdepasse123"})
    check(st == 200, f"connexion : {st} {body}")
    cookie_hdr = h.get("Set-Cookie", "")
    check("HttpOnly" in cookie_hdr and "SameSite=Strict" in cookie_hdr, "cookie sécurisé")
    cookie = cookie_hdr.split(";")[0]

    st, _, body = req("GET", "/api/status", cookie=cookie)
    s = json.loads(body)
    check(st == 200 and s["user"] == "alice", "statut")
    files = [g["file"] for g in s["games"]]
    check(files == ["Pong.nds", "Zelda.nds"], f"jeux : {files}")
    check(s["games"][0]["update"] == "available", "badge de mise à jour")
    check(s["free"] == 3 << 30, "espace libre")

    print("commandes")
    st, _, body = req("POST", "/api/command", {"type": "install", "arg": "github.com/a/pong"},
                      cookie=cookie)
    check(st == 200 and json.loads(body)["ok"], f"install : {st} {body}")
    s = json.loads(req("GET", "/api/status", cookie=cookie)[2])
    check(s["state"] == "downloading" and s["progress"]["total"] == 4096, "progression")
    check(req("POST", "/api/command", {"type": "install", "arg": "a b"}, cookie=cookie)[0] == 400,
          "lien avec espace refusé")
    check(req("POST", "/api/command", {"type": "uninstall", "arg": "../Pong.nds"},
              cookie=cookie)[0] == 400, "chemin refusé")
    check(req("POST", "/api/command", {"type": "rm", "arg": ""}, cookie=cookie)[0] == 400,
          "commande inconnue refusée")
    st, _, body = req("POST", "/api/command", {"type": "uninstall", "arg": "Zelda.nds"},
                      cookie=cookie)
    check(st == 200 and not os.path.exists(os.path.join(games, "Zelda.nds")), "désinstallation")
    check(os.path.exists(os.path.join(games, "notes.txt")), "autres fichiers intacts")

    print("JSON : échappements et caractères spéciaux")
    st, _, body = req("POST", "/api/command", {"type": "install", "arg": "https://x.fr/é\"t.nds"},
                      cookie=cookie)
    check(st == 200, f"install avec é et guillemet : {st}")

    print("requêtes invalides ou trop grosses")
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    c.putrequest("POST", "/api/command")
    c.putheader("Host", "192.168.1.42")
    c.putheader("Content-Length", "100000")
    c.endheaders()
    check(c.getresponse().status == 413, "corps trop gros refusé")
    c.close()

    print("déconnexion")
    req("POST", "/api/logout", cookie=cookie)
    check(req("GET", "/api/status", cookie=cookie)[0] == 401, "session fermée")

    print("blocage après 5 essais ratés")
    for _ in range(5):
        req("POST", "/api/login", {"user": "alice", "pass": "nope"})
    st, _, body = req("POST", "/api/login", {"user": "alice", "pass": "motdepasse123"})
    check(st == 429, f"6e essai bloqué même avec le bon mot de passe : {st}")

    check(proc.poll() is None, "le serveur a planté")
finally:
    proc.terminate()
    _, err = proc.communicate(timeout=5)
    check('COMMANDE install https://x.fr/é"t.nds' in err, "URL décodée intacte côté DSi")
    if "ERROR" in err or "runtime error" in err:
        print(err)
        failures += 1

print("\nTout est OK" if not failures else f"\n{failures} ÉCHEC(S)")
sys.exit(1 if failures else 0)
