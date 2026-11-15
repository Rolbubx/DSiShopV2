#!/usr/bin/env python3
"""Tests of the Universal-DB source (python3 catalog-server/test_udb.py [full.json])."""
import json
import os
import sys
import threading
import unittest
import urllib.request
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog  # noqa: E402

REAL = sys.argv.pop(1) if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]) else None

SAMPLE = [
    {"title": "Pong", "github": "auteur/pong", "systems": ["DS"], "stars": "12",
     "version": "v1.2", "description": "Un\tpong\nclassique",
     "downloads": {"pong.nds": {"url": "https://github.com/auteur/pong/releases/download/v1.2/pong.nds"}}},
    {"title": "Zip only", "github": "a/zipgame", "systems": ["DS"], "stars": 3, "version": "1",
     "description": "", "downloads": {"Game.ZIP": {}}},
    {"title": "3DS app", "github": "a/three", "systems": ["3DS"], "downloads": {"x.nds": {}}},
    {"title": "7z only", "github": "a/seven", "systems": ["DS"], "downloads": {"x.7z": {}}},
    {"title": "No github", "systems": ["DS"], "downloads": {"x.nds": {}}},
    {"title": "Bad repo", "github": "a/b/../c", "systems": ["DS"], "downloads": {"x.nds": {}}},
    {"title": "No downloads", "github": "a/none", "systems": ["DS"]},
    {"title": "Odd stars", "github": "B/Odd", "systems": ["ds"], "stars": "n/a",
     "downloads": {"o.dsi": {}}},
    "garbage", None,
]


class UdbTest(unittest.TestCase):
    def test_filtering(self):
        got = catalog.udb_entries(SAMPLE)
        self.assertEqual(sorted(got), ["a/zipgame", "auteur/pong", "b/odd"])
        pong = got["auteur/pong"]
        self.assertEqual((pong["owner"], pong["name"], pong["stars"], pong["tag"]),
                         ("auteur", "pong", 12, "v1.2"))
        self.assertNotIn("\t", pong["desc"])
        self.assertNotIn("\n", pong["desc"])
        self.assertEqual(got["b/odd"]["stars"], 0)
        self.assertEqual(catalog.udb_entries({"not": "a list"}), {})

    def test_search_and_api(self):
        catalog.catalog.clear()
        catalog.catalog["Auteur/Pong"] = {"owner": "Auteur", "name": "Pong", "stars": 1,
                                         "tag": "old", "desc": "crawl", "checked": 0}
        catalog.catalog["x/crawled"] = {"owner": "x", "name": "crawled", "stars": 5,
                                        "tag": "v9", "desc": "only crawled", "checked": 0}
        catalog.udb.clear()
        catalog.udb.update(catalog.udb_entries(SAMPLE))
        catalog.augment = lambda q: None          # no GitHub here
        server = ThreadingHTTPServer(("127.0.0.1", 0), catalog.Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = "http://127.0.0.1:%d" % server.server_address[1]
        try:
            rows = urllib.request.urlopen(base + "/search?q=pong").read().decode().splitlines()
            self.assertEqual(len(rows), 1, rows)      # same project in both: shown once
            self.assertEqual(rows[0].split("\t")[:5],
                             ["auteur", "pong", "12", "v1.2", "Un pong classique"])
            rows = urllib.request.urlopen(base + "/search?q=crawled").read().decode().splitlines()
            self.assertEqual(rows[0].split("\t")[:2], ["x", "crawled"])   # crawl still completes
            rows = urllib.request.urlopen(base + "/user?name=a").read().decode().splitlines()
            self.assertEqual([r.split("\t")[1] for r in rows], ["zipgame"])
            health = urllib.request.urlopen(base + "/health").read().decode()
            self.assertTrue(health.startswith("ok 4 "), health)
        finally:
            server.shutdown()

    @unittest.skipUnless(REAL, "pass a real full.json to check it")
    def test_real_list(self):
        with open(REAL, encoding="utf-8") as f:
            got = catalog.udb_entries(json.load(f))
        print("\nliste réelle : %d projets DS/DSi installables" % len(got))
        self.assertGreater(len(got), 30)
        for e in got.values():
            self.assertTrue(catalog.GITHUB_RE.fullmatch(e["owner"] + "/" + e["name"]))


if __name__ == "__main__":
    unittest.main()
