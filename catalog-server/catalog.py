#!/usr/bin/env python3
"""DSi Shop catalog server (standard library only).

Crawls GitHub for Nintendo DS / DSi homebrew, keeps only the projects whose
latest release really contains a .nds / .dsi / .zip file, and serves a tiny
tab-separated search API for the console (easy to parse in C):

    GET /search?q=name   -> lines "owner\tname\tstars\ttag\tdescription"
    GET /user?name=who   -> same format, only that owner's projects
    GET /health          -> "ok <number of projects> <last crawl time>"

TWiLight Menu++ updates: the official release is a .7z the console cannot
decompress (16 MB LZMA dictionary), so it is repacked here into small .zip
packets the console downloads and extracts one by one:

    GET /twilight/manifest          -> version + list of packets (see below)
    GET /twilight/pack/<tag>/<name> -> one packet

This needs a 7-Zip command on the server (7zz, 7z, 7za, 7zr or bsdtar):
    sudo apt install p7zip-full

Run behind a reverse proxy (Caddy) that does the HTTPS. Optional environment:
    GITHUB_TOKEN   token with no scopes (public data only): 5000 requests/hour
    CATALOG_FILE   where the catalog is stored (default: catalog.json)
    PORT           listen port on 127.0.0.1 (default: 8081)
    TWILIGHT_DIR   where the TWiLight Menu++ packets are kept (default: twilight)
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
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


# ---------------------------------------------------------------------------
# TWiLight Menu++ updates, repacked into small .zip packets
# ---------------------------------------------------------------------------
#
# Manifest (tab-separated, like the rest of the API):
#   twl1 <TAB> tag <TAB> number of packets <TAB> free space needed (bytes)
#   name.zip <TAB> size <TAB> extracted size <TAB> sha256 <TAB> condition
#
# "condition" is "-" or a path on the SD card: the console only applies the
# packet when that path already exists (files meant for hiyaCFW or ntrboot).
# The last packet holds BOOT.NDS and version.txt, so an interrupted update
# still boots the previous menu and is still reported as the old version.

TWILIGHT_DIR = os.environ.get("TWILIGHT_DIR", "twilight")
TWL_OWNER, TWL_REPO, TWL_ASSET = "DS-Homebrew", "TWiLightMenu", "TWiLightMenu-DSi.7z"
TWL_CHECK_EVERY = 1800                 # look for a new release every 30 minutes
TWL_MAX_ARCHIVE = 200 * 1024 * 1024
TWL_MAX_EXTRACTED = 600 * 1024 * 1024
TWL_PACK_SIZE = 8 * 1024 * 1024        # extracted bytes per packet (a bigger file is alone)
TWL_MAX_PACKS = 48                     # SYSUPD_MAX_PACKS on the console
TWL_LAST = ("BOOT.NDS", "version.txt")  # written last, in this order
TWL_SKIP = ("roms/",)                  # placeholder files only
PACK_RE = re.compile(r"[a-z0-9\-]{1,32}\.zip")
BAD_PATH_RE = re.compile(r'[\x00-\x1f\\:*?"<>|]')

twl_lock = threading.Lock()
twl = {"tag": "", "manifest": "", "error": "", "building": False, "checked": 0.0}


def find_7z():
    for tool in ("7zz", "7z", "7za", "7zr", "bsdtar"):
        path = shutil.which(tool)
        if path:
            return tool, path
    return None, None


def twl_download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "dsishop-catalog"})
    size = 0
    with urllib.request.urlopen(req, timeout=60) as up, open(dest, "wb") as f:
        while True:
            chunk = up.read(65536)
            if not chunk:
                break
            size += len(chunk)
            if size > TWL_MAX_ARCHIVE:
                raise ValueError("archive too big")
            f.write(chunk)


def twl_extract(archive, dest):
    tool, path = find_7z()
    if not path:
        raise RuntimeError("no 7-Zip tool on the server (install p7zip-full)")
    if tool == "bsdtar":
        cmd = [path, "-xf", archive, "-C", dest]
    else:
        cmd = [path, "x", "-y", "-bd", "-o" + dest, archive]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=600)


def twl_list_files(root):
    """Relative paths (with "/") of the regular files worth sending, with sizes."""
    files, total = [], 0
    for base, dirs, names in os.walk(root):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            if rel.startswith(TWL_SKIP):
                continue
            if BAD_PATH_RE.search(rel) or len(rel.encode("utf-8")) >= 200 or ".." in rel:
                print("twilight: skipping odd path %r" % rel, flush=True)
                continue
            size = os.path.getsize(full)
            total += size
            if total > TWL_MAX_EXTRACTED:
                raise ValueError("release too big once extracted")
            files.append((rel, size))
    return files


def twl_plan(files):
    """Split the files into packets: [(name, condition, [(path, size)...])]."""
    main, last, title, ntrboot = [], [], {}, []
    for rel, size in files:
        parts = rel.split("/")
        if rel in TWL_LAST:
            last.append((rel, size))
        elif parts[0] == "title" and len(parts) > 3:
            # hiyaCFW only: one group per title, applied if the title is installed
            title.setdefault("/".join(parts[:3]), []).append((rel, size))
        elif rel == "ntrboot.nds":
            ntrboot.append((rel, size))
        else:
            main.append((rel, size))
    last.sort(key=lambda f: TWL_LAST.index(f[0]))
    if not any(rel == "BOOT.NDS" for rel, _ in last):
        raise ValueError("no BOOT.NDS in the release")

    packs, cur, cur_size = [], [], 0
    for f in main:
        if cur and cur_size + f[1] > TWL_PACK_SIZE:
            packs.append(cur)
            cur, cur_size = [], 0
        cur.append(f)
        cur_size += f[1]
    if cur:
        packs.append(cur)

    plan = [("main-%02d.zip" % (i + 1), "-", p) for i, p in enumerate(packs)]
    for i, (cond, group) in enumerate(sorted(title.items())):
        plan.append(("title-%02d.zip" % (i + 1), cond, group))
    if ntrboot:
        plan.append(("ntrboot.zip", "ntrboot.nds", ntrboot))
    plan.append(("boot.zip", "-", last))
    if len(plan) > TWL_MAX_PACKS:
        raise ValueError("too many packets")
    return plan


def twl_write_pack(root, dest, group):
    # Fixed dates: the same release always gives byte-identical packets, so a
    # console can resume an update even if the server rebuilt them meanwhile
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, allowZip64=False,
                         compresslevel=9) as z:
        for rel, _ in group:
            info = zipfile.ZipInfo(rel, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(root, *rel.split("/")), "rb") as f:
                z.writestr(info, f.read())
    sha = hashlib.sha256()
    with open(dest, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            sha.update(chunk)
    return os.path.getsize(dest), sha.hexdigest()


def twl_build(tag, archive_url):
    """Download, extract and repack one release into TWILIGHT_DIR/<tag>."""
    os.makedirs(TWILIGHT_DIR, exist_ok=True)
    work = tempfile.mkdtemp(prefix="build-", dir=TWILIGHT_DIR)
    try:
        archive = os.path.join(work, "release.7z")
        root = os.path.join(work, "files")
        out = os.path.join(work, "out")
        os.mkdir(root)
        os.mkdir(out)
        twl_download(archive_url, archive)
        twl_extract(archive, root)
        os.remove(archive)

        lines, need = [], 0
        plan = twl_plan(twl_list_files(root))
        for name, cond, group in plan:
            size, sha = twl_write_pack(root, os.path.join(out, name), group)
            extracted = sum(s for _, s in group)
            need = max(need, size + extracted)
            lines.append("%s\t%d\t%d\t%s\t%s\n" % (name, size, extracted, sha, cond))
        manifest = "twl1\t%s\t%d\t%d\n%s" % (tag, len(plan), need + (1 << 20), "".join(lines))
        with open(os.path.join(out, "manifest.txt"), "w", encoding="utf-8") as f:
            f.write(manifest)

        final = os.path.join(TWILIGHT_DIR, tag)
        shutil.rmtree(final, ignore_errors=True)
        os.replace(out, final)
        return manifest
    finally:
        shutil.rmtree(work, ignore_errors=True)


def twl_cleanup(keep):
    """Keep the current release and the previous one (a console may be mid-update)."""
    tags = [d for d in os.listdir(TWILIGHT_DIR)
            if os.path.isfile(os.path.join(TWILIGHT_DIR, d, "manifest.txt"))]
    tags.sort(key=lambda d: os.path.getmtime(os.path.join(TWILIGHT_DIR, d)), reverse=True)
    for d in [t for t in tags if t != keep][1:]:
        shutil.rmtree(os.path.join(TWILIGHT_DIR, d), ignore_errors=True)


def twl_refresh():
    """Check the latest release and (re)build the packets if it changed."""
    try:
        rel = get_release(TWL_OWNER, TWL_REPO, None)
        tag = rel.get("tag_name", "")
        if not TAG_RE.fullmatch(tag) or tag.startswith("."):
            raise ValueError("unexpected release tag")
        asset = next((a for a in rel.get("assets", []) if a.get("name") == TWL_ASSET), None)
        if not asset:
            raise ValueError("%s is not in the latest release" % TWL_ASSET)

        path = os.path.join(TWILIGHT_DIR, tag, "manifest.txt")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                manifest = f.read()
        else:
            print("twilight: building packets for %s" % tag, flush=True)
            manifest = twl_build(tag, asset["browser_download_url"])
            print("twilight: %s ready" % tag, flush=True)
        with twl_lock:
            twl.update(tag=tag, manifest=manifest, error="")
        with open(os.path.join(TWILIGHT_DIR, "current"), "w", encoding="utf-8") as f:
            f.write(tag)
        twl_cleanup(tag)
    except Exception as e:
        print("twilight: refresh failed:", e, flush=True)
        with twl_lock:
            twl["error"] = str(e)[:200]
    finally:
        with twl_lock:
            twl["building"] = False
            twl["checked"] = time.time()


def twl_load():
    """At startup: serve the packets built by a previous run straight away."""
    try:
        with open(os.path.join(TWILIGHT_DIR, "current"), encoding="utf-8") as f:
            tag = f.read().strip()
        if not TAG_RE.fullmatch(tag):
            return
        with open(os.path.join(TWILIGHT_DIR, tag, "manifest.txt"), encoding="utf-8") as f:
            twl.update(tag=tag, manifest=f.read())
    except OSError:
        pass


def twl_manifest():
    """(manifest or None, error). Starts a background refresh when it is due."""
    with twl_lock:
        due = not twl["building"] and time.time() - twl["checked"] > TWL_CHECK_EVERY
        if due:
            twl["building"] = True
        manifest, error = twl["manifest"], twl["error"]
    if due:
        threading.Thread(target=twl_refresh, daemon=True).start()
    return (manifest or None), error


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

    def serve_twilight(self, parts):
        if parts == ["twilight", "manifest"]:
            manifest, error = twl_manifest()
            if manifest:
                return self.reply(200, manifest)
            # Not built yet: the console asks again a little later
            return self.reply(503, "error: %s\n" % error if error else "preparing\n")
        # /twilight/pack/<tag>/<name>
        if len(parts) != 4 or parts[1] != "pack" or not TAG_RE.fullmatch(parts[2]) \
                or parts[2].startswith(".") or not PACK_RE.fullmatch(parts[3]):
            return self.reply(404, "not found\n")
        path = os.path.join(TWILIGHT_DIR, parts[2], parts[3])
        try:
            f = open(path, "rb")
        except OSError:
            return self.reply(404, "not found\n")
        with f:
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(os.fstat(f.fileno()).st_size))
            self.end_headers()
            try:
                shutil.copyfileobj(f, self.wfile, 65536)
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
        elif url.path.startswith("/twilight/"):
            self.serve_twilight([urllib.parse.unquote(p) for p in url.path.strip("/").split("/")])
        elif url.path == "/user":
            name = qs.get("name", [""])[0]
            if not re.fullmatch(r"[A-Za-z0-9\-]{1,39}", name):
                return self.reply(400, "bad user\n")
            self.reply(200, lines(user_projects(name, limit)))
        else:
            self.reply(404, "not found\n")


if __name__ == "__main__":
    load()
    twl_load()
    threading.Thread(target=crawl_loop, daemon=True).start()
    print("listening on 127.0.0.1:%d" % PORT, flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
