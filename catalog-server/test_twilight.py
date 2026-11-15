#!/usr/bin/env python3
"""Tests of the TWiLight Menu++ repacking (python3 catalog-server/test_twilight.py).

GitHub is replaced by a local fake release. Needs a 7-Zip command to build
the fake .7z; the tests are skipped without one.
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import zipfile
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog  # noqa: E402

SEVENZIP = next((shutil.which(t) for t in ("7zz", "7z", "7za", "7zr") if shutil.which(t)), None)

FILES = {
    "BOOT.NDS": b"boot" * 1000,
    "version.txt": b"TWiLight Menu++: v9.9.9\nnds-bootstrap: Release v1.0.0\n",
    "_nds/TWiLightMenu/main.srldr": os.urandom(300_000),
    "_nds/TWiLightMenu/manual.srldr": os.urandom(700_000),
    "_nds/colorLut/[10000K] Aquarium.lut": bytes(65536),
    "_nds/colorLut/Default.lut": b"",
    "ntrboot.nds": b"ntr" * 100,
    "roms/nds/Place NDS ROMs here": b"",
    "title/00030004/53524c41/content/title.tmd": b"tmd",
    "title/00030004/53524c41/content/00000000.app": b"app" * 50,
}


@unittest.skipUnless(SEVENZIP, "no 7-Zip command available")
class TwilightTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="twl-test-")
        src = os.path.join(cls.tmp, "src")
        for rel, data in FILES.items():
            path = os.path.join(src, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        cls.archive = os.path.join(cls.tmp, "TWiLightMenu-DSi.7z")
        subprocess.run([SEVENZIP, "a", cls.archive, "."], cwd=src, check=True,
                       stdout=subprocess.DEVNULL)

        catalog.TWILIGHT_DIR = os.path.join(cls.tmp, "twilight")
        catalog.TWL_PACK_SIZE = 400_000      # several packets with these small files
        catalog.get_release = lambda owner, repo, tag: {
            "tag_name": "v9.9.9",
            "assets": [{"name": "TWiLightMenu-DSi.7z", "size": os.path.getsize(cls.archive),
                        "browser_download_url": "fake://release"}]}
        catalog.twl_download = lambda url, dest: shutil.copyfile(cls.archive, dest)

        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), catalog.Handler)
        cls.base = "http://127.0.0.1:%d" % cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

        # First request: nothing built yet, the build starts in the background
        try:
            urllib.request.urlopen(cls.base + "/twilight/manifest")
            raise AssertionError("expected 503 before the first build")
        except urllib.error.HTTPError as e:
            assert e.code == 503, e.code
        for _ in range(200):
            with catalog.twl_lock:
                if not catalog.twl["building"]:
                    break
            threading.Event().wait(0.05)
        cls.manifest = urllib.request.urlopen(cls.base + "/twilight/manifest").read().decode()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def packs(self):
        lines = self.manifest.strip().split("\n")
        head = lines[0].split("\t")
        return head, [l.split("\t") for l in lines[1:]]

    def test_header(self):
        head, packs = self.packs()
        self.assertEqual(head[:2], ["twl1", "v9.9.9"])
        self.assertEqual(int(head[2]), len(packs))
        self.assertGreaterEqual(int(head[3]), max(int(p[1]) + int(p[2]) for p in packs))

    def test_packets_match_release(self):
        _, packs = self.packs()
        seen = {}
        for name, size, extracted, sha, cond in packs:
            data = urllib.request.urlopen("%s/twilight/pack/v9.9.9/%s" % (self.base, name)).read()
            self.assertEqual(len(data), int(size))
            self.assertEqual(hashlib.sha256(data).hexdigest(), sha)
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                self.assertEqual(sum(i.file_size for i in z.infolist()), int(extracted))
                for info in z.infolist():
                    self.assertIn(info.compress_type, (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED))
                    self.assertNotIn(info.filename, seen)
                    seen[info.filename] = (z.read(info), name, cond)
        expected = {k: v for k, v in FILES.items() if not k.startswith("roms/")}
        self.assertEqual({k: v[0] for k, v in seen.items()}, expected)

        # BOOT.NDS and version.txt come last; optional files carry a condition
        self.assertEqual(packs[-1][0], "boot.zip")
        self.assertEqual(seen["BOOT.NDS"][1], "boot.zip")
        self.assertEqual(seen["version.txt"][1], "boot.zip")
        self.assertEqual(seen["ntrboot.nds"][2], "ntrboot.nds")
        self.assertEqual(seen["title/00030004/53524c41/content/title.tmd"][2],
                         "title/00030004/53524c41")
        self.assertEqual(seen["_nds/TWiLightMenu/main.srldr"][2], "-")

    def test_rebuild_is_identical(self):
        again = catalog.twl_build("v9.9.9", "fake://release")
        self.assertEqual(again, self.manifest)

    def test_bad_paths(self):
        for path in ("/twilight/pack/v9.9.9/../current", "/twilight/pack/../x.zip",
                     "/twilight/pack/v9.9.9/manifest.txt", "/twilight/pack/v9.9.9/nope.zip",
                     "/twilight/pack/%2e%2e/boot.zip", "/twilight/other"):
            with self.assertRaises(urllib.error.HTTPError) as ctx:
                urllib.request.urlopen(self.base + path)
            self.assertEqual(ctx.exception.code, 404, path)


if __name__ == "__main__":
    unittest.main()
