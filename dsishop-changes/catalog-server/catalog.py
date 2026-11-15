#!/usr/bin/env python3
"""DSi Shop catalog server (standard library only).

Crawls GitHub for Nintendo DS / DSi homebrew, keeps only the projects whose
latest release really contains a .nds / .dsi / .zip file, and serves a tiny
tab-separated search API for the console (easy to parse in C):

    GET /search?q=name   -> lines "owner\tname\tstars\ttag\tdescription"
    GET /user?name=who   -> same format, only that owner's projects
    GET /health          -> "ok <number of projects> <last crawl time>"

Run behind a reverse proxy (Caddy) that does the HTTPS. Optional environment:
    GITHUB_TOKEN   token with no scopes (public data only): 5000 requests/hour
    CATALOG_FILE   where the catalog is stored (default: catalog.json)
    PORT           listen port on 127.0.0.1 (default: 8081)
"""
import json
import os
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = os.environ.get("GITHUB_TOKEN", "")
CATALOG_FILE = os.environ.get("CATALOG_FILE", "catalog.json")
PORT = int(os.environ.get("PORT", "8081"))

TOPICS = ["nds", "nintendo-ds", "nintendo-dsi", "libnds", "blocksds",
          "nds-homebrew", "dsi-homebrew", "devkitpro-nds"]
EXTENSIONS = (".nds", ".dsi", ".zip")
CRAWL_EVERY = 6 * 3600        # seconds between full crawls
RECHECK_AFTER = 24 * 3600     # a project is re-checked at most once a day
MAX_NEW_PER_CRAWL = 150       # keeps us under the API limits
USER_CACHE_TTL = 3600

lock = threading.Lock()
catalog = {}       # "owner/name" -> {owner, name, stars, tag, desc, checked}
last_crawl = 0.0
user_cache = {}    # login -> (timestamp, [entries])


def api(path):
    req = urllib.request.Request(
        "https://api.github.com" + path,
        headers={"Accept": "application/vnd.github+json",
                 "User-Agent": "dsishop-catalog",
                 "X-GitHub-Api-Version": "2022-11-28"})
    if TOKEN:
        req.add_header("Authorization", "Bearer " + TOKEN)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def release_info(full):
    """Latest release tag, or None if it has no DS/DSi file."""
    try:
        rel = api("/repos/%s/releases/latest" % full)
    except urllib.error.HTTPError as e:
        if e.code in (403, 429):
            raise
        return None
    except Exception:
        return None
    for asset in rel.get("assets", []):
        name = asset.get("name", "").lower()
        if name.endswith((".nds", ".dsi")):
            return rel.get("tag_name", "")
        # A .zip only counts when its name says it is a DS / DSi game
        if name.endswith(".zip") and re.search(r"(^|[^a-z])(nds|dsi|rom)([^a-z]|$)", name):
            return rel.get("tag_name", "")
    return None


def entry_from_repo(repo, tag):
    return {
        "owner": repo["owner"]["login"],
        "name": repo["name"],
        "stars": repo.get("stargazers_count", 0),
        "tag": tag,
        "desc": (repo.get("description") or "").replace("\t", " ").replace("\n", " ")[:120],
        "checked": time.time(),
    }


def save():
    tmp = CATALOG_FILE + ".tmp"
    with lock, open(tmp, "w", encoding="utf-8") as f:
        json.dump({"last_crawl": last_crawl, "items": catalog}, f)
    os.replace(tmp, CATALOG_FILE)


def load():
    global last_crawl
    try:
        with open(CATALOG_FILE, encoding="utf-8") as f:
            data = json.load(f)
        with lock:
            catalog.update(data.get("items", {}))
        last_crawl = data.get("last_crawl", 0.0)
    except (OSError, ValueError):
        pass


def crawl():
    global last_crawl
    print("crawl: start", flush=True)
    seen = {}
    try:
        for topic in TOPICS:
            for page in (1, 2, 3):
                res = api("/search/repositories?q=topic:%s&sort=stars&per_page=100&page=%d"
                          % (topic, page))
                for repo in res.get("items", []):
                    if not repo.get("fork") and not repo.get("archived"):
                        seen[repo["full_name"]] = repo
                if len(res.get("items", [])) < 100:
                    break
                time.sleep(7)  # search API: 10 requests/minute
            time.sleep(7)
    except Exception as e:  # rate limit, network...
        print("crawl: search stopped:", e, flush=True)

    new_checked = 0
    try:
        for full, repo in seen.items():
            old = catalog.get(full)
            if old and time.time() - old["checked"] < RECHECK_AFTER:
                old["stars"] = repo.get("stargazers_count", old["stars"])
                continue
            if new_checked >= MAX_NEW_PER_CRAWL:
                break
            new_checked += 1
            tag = release_info(full)
            with lock:
                if tag is None:
                    catalog.pop(full, None)
                else:
                    catalog[full] = entry_from_repo(repo, tag)
    except Exception as e:
        print("crawl: release check stopped:", e, flush=True)

    last_crawl = time.time()
    save()
    print("crawl: done, %d projects" % len(catalog), flush=True)


def crawl_loop():
    while True:
        if time.time() - last_crawl > CRAWL_EVERY:
            try:
                crawl()
            except Exception as e:
                print("crawl failed:", e, flush=True)
        time.sleep(300)


def lines(entries):
    return "".join("%s\t%s\t%d\t%s\t%s\n" % (e["owner"], e["name"], e["stars"],
                                              e["tag"], e["desc"]) for e in entries)


def search(query, limit):
    words = [w for w in re.split(r"\s+", query.lower().strip()) if w]
    scored = []
    with lock:
        for e in catalog.values():
            hay_name = (e["name"] + " " + e["owner"]).lower()
            hay = hay_name + " " + e["desc"].lower()
            if not all(w in hay for w in words):
                continue
            # Name matches first, then by stars
            in_name = all(w in hay_name for w in words)
            scored.append((not in_name, -e["stars"], e))
    scored.sort(key=lambda t: t[:2])
    return [t[2] for t in scored[:limit]]


search_seen = {}               # normalised query -> time of its last GitHub lookup
augment_lock = threading.Lock()
SEARCH_TTL = 3600              # the same query hits GitHub at most once an hour
MAX_CHECKS_PER_SEARCH = 10     # new projects whose release is checked per query


def augment(query):
    """Look the query up on GitHub and add the projects we don't know yet.

    Projects already in the catalog (checked within RECHECK_AFTER) are skipped:
    no extra request for them. Results are always served from the catalog."""
    key = " ".join(query.lower().split())
    with augment_lock:
        if time.time() - search_seen.get(key, 0) < SEARCH_TTL:
            return
        search_seen[key] = time.time()
        if len(search_seen) > 500:
            search_seen.clear()
        topics = "+OR+".join("topic:" + t for t in TOPICS)
        try:
            res = api("/search/repositories?q=%s+(%s)&sort=stars&per_page=20"
                      % (urllib.parse.quote_plus(query), topics))
        except Exception as e:
            print("search lookup failed:", e, flush=True)
            return
        checks = 0
        changed = False
        for repo in res.get("items", []):
            full = repo["full_name"]
            if repo.get("fork") or repo.get("archived"):
                continue
            old = catalog.get(full)
            if old and time.time() - old["checked"] < RECHECK_AFTER:
                continue                      # already known: no GitHub request
            if checks >= MAX_CHECKS_PER_SEARCH:
                break
            checks += 1
            try:
                tag = release_info(full)
            except Exception as e:            # rate limit: stop here
                print("release check stopped:", e, flush=True)
                break
            with lock:
                if tag is None:
                    catalog.pop(full, None)
                else:
                    catalog[full] = entry_from_repo(repo, tag)
            changed = True
        if changed:
            save()


def user_projects(login, limit):
    with lock:
        mine = [e for e in catalog.values() if e["owner"].lower() == login.lower()]
    if mine:
        return sorted(mine, key=lambda e: -e["stars"])[:limit]
    cached = user_cache.get(login.lower())
    if cached and time.time() - cached[0] < USER_CACHE_TTL:
        return cached[1][:limit]
    # Not crawled yet: look the user up live (at most 20 projects)
    out = []
    try:
        repos = api("/users/%s/repos?per_page=20&sort=pushed" % urllib.parse.quote(login))
        for repo in repos:
            if repo.get("fork"):
                continue
            tag = release_info(repo["full_name"])
            if tag is not None:
                out.append(entry_from_repo(repo, tag))
    except Exception:
        pass
    user_cache[login.lower()] = (time.time(), out)
    if out:
        with lock:  # remember them: the next lookup is served from the catalog
            for e in out:
                catalog["%s/%s" % (e["owner"], e["name"])] = e
        save()
    return out[:limit]


PUBLIC_URL = os.environ.get("PUBLIC_URL", "https://dsi-shop.duckdns.org")
RELEASE_TTL = 600
MAX_FILE = 600 * 1024 * 1024
release_cache = {}   # (owner, repo, tag or "latest") -> (timestamp, release json)

OWNER_RE = re.compile(r"[A-Za-z0-9\-]{1,39}")
REPO_RE = re.compile(r"[A-Za-z0-9._\-]{1,100}")
TAG_RE = re.compile(r"[A-Za-z0-9._+\-]{1,64}")


def get_release(owner, repo, tag):
    """Release JSON from GitHub (cached 10 minutes). tag=None means latest."""
    key = (owner.lower(), repo.lower(), tag or "latest")
    hit = release_cache.get(key)
    if hit and time.time() - hit[0] < RELEASE_TTL:
        return hit[1]
    path = "/repos/%s/%s/releases/%s" % (
        owner, repo, "latest" if tag is None else "tags/" + urllib.parse.quote(tag))
    rel = api(path)
    if len(release_cache) > 500:
        release_cache.clear()
    release_cache[key] = (time.time(), rel)
    return rel


def rewrite_release(owner, repo, rel):
    """Same shape as GitHub's answer, but every file points back to this server,
    so the console never has to resolve a GitHub host name."""
    tag = rel.get("tag_name", "")
    assets = []
    for a in rel.get("assets", []):
        name = a.get("name", "")
        assets.append({
            "name": name,
            "size": a.get("size", 0),
            "browser_download_url": "%s/file/%s/%s/%s/%s" % (
                PUBLIC_URL, owner, repo, urllib.parse.quote(tag, safe=""),
                urllib.parse.quote(name, safe="")),
        })
    return {"tag_name": tag, "name": rel.get("name", ""), "assets": assets}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def github_error(self, e):
        if isinstance(e, urllib.error.HTTPError):
            if e.code == 404:
                return self.reply(404, "not found\n")
            if e.code in (403, 429):
                return self.reply(403, "rate limited\n")
        self.reply(502, "upstream error\n")

    def serve_api(self, parts):
        # /api/repos/<owner>/<repo>/releases/latest   |   .../releases/tags/<tag>
        if len(parts) < 6 or parts[0:1] != ["api"] or parts[1] != "repos" \
                or parts[4] != "releases":
            return self.reply(404, "not found\n")
        owner, repo = parts[2], parts[3]
        tag = None
        if parts[5] == "latest" and len(parts) == 6:
            pass
        elif parts[5] == "tags" and len(parts) == 7 and TAG_RE.fullmatch(parts[6]):
            tag = parts[6]
        else:
            return self.reply(404, "not found\n")
        if not (OWNER_RE.fullmatch(owner) and REPO_RE.fullmatch(repo)):
            return self.reply(400, "bad repo\n")
        try:
            rel = get_release(owner, repo, tag)
        except Exception as e:
            return self.github_error(e)
        body = json.dumps(rewrite_release(owner, repo, rel))
        self.reply(200, body, "application/json")

    def serve_file(self, parts):
        # /file/<owner>/<repo>/<tag>/<asset name>
        if len(parts) != 5:
            return self.reply(404, "not found\n")
        owner, repo, tag, name = parts[1], parts[2], parts[3], parts[4]
        if not (OWNER_RE.fullmatch(owner) and REPO_RE.fullmatch(repo)
                and TAG_RE.fullmatch(tag)):
            return self.reply(400, "bad path\n")
        try:
            rel = get_release(owner, repo, tag)
        except Exception as e:
            return self.github_error(e)
        # Only files that really belong to that release can be fetched
        asset = next((a for a in rel.get("assets", []) if a.get("name") == name), None)
        if not asset or asset.get("size", 0) > MAX_FILE:
            return self.reply(404, "not found\n")
        try:
            up = urllib.request.urlopen(urllib.request.Request(
                asset["browser_download_url"], headers={"User-Agent": "dsishop-catalog"}),
                timeout=60)
        except Exception as e:
            return self.github_error(e)
        with up:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(asset.get("size", 0)))
            self.send_header("ETag", '"%s"' % asset.get("updated_at", asset.get("id", "")))
            self.end_headers()
            try:
                while True:
                    chunk = up.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionError):
                pass

    def reply(self, code, text, ctype="text/plain; charset=utf-8"):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(url.query)
        try:
            limit = max(1, min(int(qs.get("limit", ["30"])[0]), 50))
        except ValueError:
            limit = 30
        if url.path == "/health":
            with lock:
                n = len(catalog)
            self.reply(200, "ok %d %d\n" % (n, last_crawl))
        elif url.path == "/search":
            q = qs.get("q", [""])[0][:60]
            if not re.fullmatch(r"[\w .\-]+", q):
                return self.reply(400, "bad query\n")
            augment(q)
            self.reply(200, lines(search(q, limit)))
        elif url.path.startswith("/api/"):
            self.serve_api([urllib.parse.unquote(p) for p in url.path.strip("/").split("/")])
        elif url.path.startswith("/file/"):
            self.serve_file([urllib.parse.unquote(p) for p in url.path.strip("/").split("/")])
        elif url.path == "/user":
            name = qs.get("name", [""])[0]
            if not re.fullmatch(r"[A-Za-z0-9\-]{1,39}", name):
                return self.reply(400, "bad user\n")
            self.reply(200, lines(user_projects(name, limit)))
        else:
            self.reply(404, "not found\n")


if __name__ == "__main__":
    load()
    threading.Thread(target=crawl_loop, daemon=True).start()
    print("listening on 127.0.0.1:%d" % PORT, flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
