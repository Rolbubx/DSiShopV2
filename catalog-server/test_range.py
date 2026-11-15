#!/usr/bin/env python3
"""Tests of partial downloads (python3 catalog-server/test_range.py).

The console resumes an interrupted download with "Range: bytes=N-": the server must
send exactly the missing end of the file, and never glue two versions together.
"""
import os
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog  # noqa: E402

DATA = os.urandom(300_000)


class FakeUpstream(BaseHTTPRequestHandler):
    supports_range = True
    seen_ranges = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        rng = self.headers.get("Range")
        type(self).seen_ranges.append(rng)
        if rng and type(self).supports_range:
            start = int(rng.split("=")[1].rstrip("-"))
            body = DATA[start:]
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(DATA) - 1, len(DATA)))
        else:
            body = DATA
            self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class RangeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.up = ThreadingHTTPServer(("127.0.0.1", 0), FakeUpstream)
        threading.Thread(target=cls.up.serve_forever, daemon=True).start()
        url = "http://127.0.0.1:%d/asset.bin" % cls.up.server_address[1]
        catalog.get_release = lambda owner, repo, tag: {
            "tag_name": "v1", "assets": [{"name": "game.nds", "size": len(DATA), "id": 7,
                                          "updated_at": "T1", "browser_download_url": url}]}
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), catalog.Handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.url = "http://127.0.0.1:%d/file/o/r/v1/game.nds" % cls.server.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.up.shutdown()
        cls.server.shutdown()

    def setUp(self):
        FakeUpstream.supports_range = True
        FakeUpstream.seen_ranges = []

    def get(self, **headers):
        req = urllib.request.Request(self.url, headers=headers)
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()

    def test_whole_file(self):
        status, h, body = self.get()
        self.assertEqual(status, 200)
        self.assertEqual(body, DATA)
        self.assertEqual(h["Content-Length"], str(len(DATA)))
        self.assertEqual(h["Accept-Ranges"], "bytes")
        self.assertEqual(h["ETag"], '"T1"')
        self.assertEqual(FakeUpstream.seen_ranges, [None])

    def test_resume_from_the_middle(self):
        status, h, body = self.get(Range="bytes=100000-")
        self.assertEqual(status, 206)
        self.assertEqual(h["Content-Range"], "bytes 100000-299999/300000")
        self.assertEqual(h["Content-Length"], "200000")
        self.assertEqual(body, DATA[100000:])
        self.assertEqual(FakeUpstream.seen_ranges, ["bytes=100000-"])

    def test_upstream_without_range_support(self):
        FakeUpstream.supports_range = False
        status, h, body = self.get(Range="bytes=123457-")
        self.assertEqual(status, 206)
        self.assertEqual(h["Content-Range"], "bytes 123457-299999/300000")
        self.assertEqual(body, DATA[123457:])

    def test_if_range(self):
        status, _, body = self.get(Range="bytes=1000-", **{"If-Range": '"T1"'})
        self.assertEqual((status, body), (206, DATA[1000:]))
        # The file changed since the first part was downloaded: whole file, never a mix
        status, h, body = self.get(Range="bytes=1000-", **{"If-Range": '"OLD"'})
        self.assertEqual((status, body), (200, DATA))
        self.assertNotIn("Content-Range", h)

    def test_past_the_end(self):
        for start in (300_000, 300_001, 10**9):
            with self.assertRaises(urllib.error.HTTPError) as ctx:
                self.get(Range="bytes=%d-" % start)
            self.assertEqual(ctx.exception.code, 416)
            self.assertEqual(ctx.exception.headers["Content-Range"], "bytes */300000")

    def test_unsupported_forms_give_the_whole_file(self):
        for value in ("bytes=0-", "bytes=0-99", "bytes=-5", "items=1-", "bytes=1-2,5-6", "garbage",
                      "bytes=99999999999999999999-"):
            status, _, body = self.get(Range=value)
            self.assertEqual((status, len(body)), (200, len(DATA)), value)


if __name__ == "__main__":
    unittest.main()
