#!/usr/bin/env python3
"""
Local dev server for main/www/index.html.

Serves the HTML and stubs out the device API endpoints so you can iterate
on the UI without flashing firmware.

Usage:
    python tools/serve.py          # http://localhost:8080
    python tools/serve.py 9000     # custom port
"""

import json
import os
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

WWW_DIR = os.path.join(os.path.dirname(__file__), "..", "main", "www")

# Fake kit folders returned by GET /sd/ls?path=sounds
FAKE_DIRS = ["3355606kit", "AVLDrumkits-BlackPearl-2023", "Classic-626", "MyCustomKit"]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(fmt % args)

    # ── routing ──────────────────────────────────────────────────────────────

    def do_GET(self):
        p = urlparse(self.path)
        if p.path == "/" or p.path == "/index.html":
            self._serve_file("index.html")
        elif p.path == "/sd/ls":
            qs = parse_qs(p.query)
            path = qs.get("path", [""])[0]
            self._json({"dirs": FAKE_DIRS if "sounds" in path else [], "files": []})
        else:
            self._404()

    def do_POST(self):
        p = urlparse(self.path)
        if p.path.startswith("/cmd/"):
            name = p.path[len("/cmd/"):]
            self._drain_body()
            if name.startswith("vol/"):
                self._text(f"vol {name[4:]}")
            else:
                self._404()
        elif p.path == "/upload":
            qs = parse_qs(p.query)
            target = qs.get("path", ["sounds"])[0]
            saved = self._receive_multipart(target)
            self._json({"ok": True, "saved": saved})
        else:
            self._404()

    # ── helpers ──────────────────────────────────────────────────────────────

    def _serve_file(self, name):
        path = os.path.join(WWW_DIR, name)
        try:
            with open(path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except FileNotFoundError:
            self._404()

    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _text(self, s):
        body = s.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _404(self):
        self.send_response(404)
        self.end_headers()

    def _drain_body(self):
        n = int(self.headers.get("Content-Length", 0))
        if n > 0:
            self.rfile.read(n)

    def _receive_multipart(self, target_dir):
        """Parse multipart/form-data, save .wav files to a temp dir, return filenames."""
        ct = self.headers.get("Content-Type", "")
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n) if n > 0 else b""

        boundary = None
        for part in ct.split(";"):
            part = part.strip()
            if part.startswith("boundary="):
                boundary = part[9:].strip('"').encode()
                break

        saved = []
        if not boundary or not body:
            return saved

        delim = b"--" + boundary
        parts = body.split(delim)
        out_dir = tempfile.mkdtemp(prefix="synth32_upload_")

        for chunk in parts:
            if not chunk or chunk in (b"", b"--\r\n", b"--\n"):
                continue
            # strip leading CRLF
            if chunk.startswith(b"\r\n"):
                chunk = chunk[2:]
            elif chunk.startswith(b"\n"):
                chunk = chunk[1:]
            # split headers from data
            sep = chunk.find(b"\r\n\r\n")
            if sep == -1:
                sep = chunk.find(b"\n\n")
                if sep == -1:
                    continue
                headers_raw = chunk[:sep].decode(errors="replace")
                data = chunk[sep + 2:]
            else:
                headers_raw = chunk[:sep].decode(errors="replace")
                data = chunk[sep + 4:]
            # strip trailing CRLF added before next boundary
            data = data.rstrip(b"\r\n")

            fname = None
            for line in headers_raw.splitlines():
                if "filename=" in line:
                    for tok in line.split(";"):
                        tok = tok.strip()
                        if tok.startswith("filename="):
                            fname = tok[9:].strip('"')
                            break

            if not fname or not fname.lower().endswith(".wav"):
                continue

            out_path = os.path.join(out_dir, os.path.basename(fname))
            with open(out_path, "wb") as f:
                f.write(data)
            print(f"  saved → {out_path} ({len(data)} bytes) [target: {target_dir}]")
            saved.append(fname)

        return saved


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Serving http://localhost:{port}  (Ctrl-C to stop)")
    HTTPServer(("", port), Handler).serve_forever()
