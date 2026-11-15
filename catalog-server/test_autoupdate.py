#!/usr/bin/env python3
"""Tests of the automatic update (python3 catalog-server/test_autoupdate.py).

The server runs for real in a subprocess and follows a fake "GitHub" served
locally: unchanged file, broken files, then a good new version while a big
download is in progress (it must finish untouched, new downloads must get 503).
"""
import hashlib
import http.client
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import catalog  # noqa: E402


class DrainTest(unittest.TestCase):
    def tearDown(self):
        catalog.paused.clear()

    def test_pause_and_drain(self):
        self.assertTrue(catalog.begin_transfer())
        result = []
        t = threading.Thread(target=lambda: result.append(catalog.drain(5)))
        t.start()
        time.sleep(0.3)
        self.assertTrue(catalog.paused.is_set())
        self.assertFalse(catalog.begin_transfer(), "new downloads must be refused")
        self.assertTrue(t.is_alive(), "drain must wait for the running download")
        catalog.end_transfer()
        t.join(5)
        self.assertEqual(result, [True])

    def test_loads_check(self):
        d = tempfile.mkdtemp(prefix="loads-")
        try:
            for name, text, ok in (("good.new", "class Handler: pass\n", True),
                                   ("syntax.new", "def (((\n", False),
                                   ("nohandler.new", "x = 1\n", False),
                                   ("raises.new", "class Handler: pass\nraise SystemExit(3)\n", False)):
                path = os.path.join(d, name)
                with open(path, "w") as f:
                    f.write(text)
                self.assertEqual(catalog.update_loads(path), ok, name)
            with open(os.path.join(d, "real.new"), "wb") as f, \
                    open(os.path.join(HERE, "catalog.py"), "rb") as src:
                f.write(src.read())
            self.assertTrue(catalog.update_loads(os.path.join(d, "real.new")), "real catalog.py")
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_drain_timeout(self):
        self.assertTrue(catalog.begin_transfer())
        self.assertFalse(catalog.drain(0.5))
        catalog.end_transfer()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class FakeGithub(BaseHTTPRequestHandler):
    content = b""
    hits = 0
    conditional_hits = 0

    def log_message(self, *a):
        pass

    def do_GET(self):
        cls = type(self)
        cls.hits += 1
        etag = '"%s"' % hashlib.sha256(cls.content).hexdigest()[:16]
        if self.headers.get("If-None-Match") == etag:
            cls.conditional_hits += 1
            self.send_response(304)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("ETag", etag)
        self.send_header("Content-Length", str(len(cls.content)))
        self.end_headers()
        self.wfile.write(cls.content)


class LiveUpdateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="autoupd-")
        cls.script = os.path.join(cls.tmp, "catalog.py")
        shutil.copyfile(os.path.join(HERE, "catalog.py"), cls.script)
        with open(cls.script, "rb") as f:
            cls.original = f.read()
        cls.original_hash = hashlib.sha256(cls.original).hexdigest()

        FakeGithub.content = cls.original
        cls.gh = ThreadingHTTPServer(("127.0.0.1", 0), FakeGithub)
        threading.Thread(target=cls.gh.serve_forever, daemon=True).start()

        # A 30 MB packet: too big for the socket buffers, so a slow reader
        # keeps the server busy sending it
        os.makedirs(os.path.join(cls.tmp, "twilight", "v1"))
        cls.pack = os.path.join(cls.tmp, "twilight", "v1", "big.zip")
        with open(cls.pack, "wb") as f:
            f.write(os.urandom(1 << 20) * 30)
        cls.pack_size = 30 << 20

        cls.port = free_port()
        cls.base = "http://127.0.0.1:%d" % cls.port
        env = dict(os.environ, AUTO_UPDATE="1", UPDATE_EVERY="1", PORT=str(cls.port),
                   UPDATE_URL="http://127.0.0.1:%d/catalog.py" % cls.gh.server_address[1],
                   CATALOG_FILE=os.path.join(cls.tmp, "catalog.json"),
                   UDB_FILE=os.path.join(cls.tmp, "udb.json"), UDB_URL="http://127.0.0.1:9/",
                   TWILIGHT_DIR=os.path.join(cls.tmp, "twilight"))
        cls.log = open(os.path.join(cls.tmp, "server.log"), "w")
        cls.proc = subprocess.Popen([sys.executable, cls.script], cwd=cls.tmp, env=env,
                                    stdout=cls.log, stderr=subprocess.STDOUT)
        cls.wait_health()

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.proc.wait()
        cls.gh.shutdown()
        cls.log.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def wait_health(cls, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            try:
                return cls.version()
            except Exception:
                time.sleep(0.2)
        raise AssertionError("server not answering:\n" + cls.logtext())

    @classmethod
    def version(cls):
        return urllib.request.urlopen(cls.base + "/health", timeout=3).read().decode().split()[3]

    @classmethod
    def logtext(cls):
        cls.log.flush()
        with open(os.path.join(cls.tmp, "server.log"), encoding="utf-8", errors="replace") as f:
            return f.read()

    def wait_for(self, cond, timeout, what):
        end = time.time() + timeout
        while time.time() < end:
            if cond():
                return
            time.sleep(0.2)
        self.fail("timeout: %s\n%s" % (what, self.logtext()))

    def test_sequence(self):
        old = self.original_hash[:8]
        self.assertEqual(self.version(), old)

        # 1. Nothing changed: no restart, and the check uses If-None-Match (304)
        self.wait_for(lambda: FakeGithub.conditional_hits >= 2, 8, "conditional checks")
        self.assertEqual(self.version(), old)

        # 2. Broken files are never applied (and not retried every second)
        for name, bad in (("syntax error", b"def (((\n"), ("no Handler", b"x = 1\n")):
            FakeGithub.content = bad
            self.wait_for(lambda: self.logtext().count("not applied") >= 1 + (name == "no Handler"),
                          8, name)
            self.assertEqual(self.version(), old, name)
            with open(self.script, "rb") as f:
                self.assertEqual(f.read(), self.original, name)
            self.assertFalse(os.path.exists(self.script + ".new"), name)
        time.sleep(2.5)
        self.assertEqual(self.logtext().count("not applied"), 2, "the same bad file was retried")
        self.assertEqual(self.version(), old)

        # 3. A good new version arrives while a big download is running
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        conn.request("GET", "/twilight/pack/v1/big.zip")
        resp = conn.getresponse()
        got = len(resp.read(1))          # reads a byte, then stops reading
        self.assertEqual(got, 1)

        good = self.original + b"\nMARKER = 'new version'\n"
        FakeGithub.content = good
        self.wait_for(lambda: "pausing new downloads" in self.logtext(), 8, "update starts")

        # new downloads are refused, everything else still works
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            urllib.request.urlopen(self.base + "/twilight/pack/v1/big.zip", timeout=5)
        self.assertEqual(ctx.exception.code, 503)
        self.assertEqual(ctx.exception.headers.get("Retry-After"), "30")
        self.assertEqual(self.version(), old, "restarted while a download was running")
        with open(self.script, "rb") as f:
            self.assertEqual(f.read(), self.original, "file replaced too early")

        # the running download finishes untouched, then the server restarts
        rest = resp.read()
        conn.close()
        self.assertEqual(1 + len(rest), self.pack_size, "the running download was cut")
        new = hashlib.sha256(good).hexdigest()[:8]
        self.wait_for(lambda: self.logtext().count("restarting") >= 1, 8, "restart")
        self.wait_health()
        self.wait_for(lambda: self.version() == new, 10, "new version running")
        self.assertIsNone(self.proc.poll(), "the process must still be alive (same PID)")
        with open(self.script, "rb") as f:
            self.assertEqual(f.read(), good)
        with open(self.script + ".prev", "rb") as f:
            self.assertEqual(f.read(), self.original)

        # 4. Downloads work again, and the same version is not applied twice
        data = urllib.request.urlopen(self.base + "/twilight/pack/v1/big.zip", timeout=30).read()
        self.assertEqual(len(data), self.pack_size)
        time.sleep(3)
        self.assertEqual(self.logtext().count("restarting"), 1)
        self.assertEqual(self.version(), new)


if __name__ == "__main__":
    unittest.main()
