#!/usr/bin/env python3
"""Local web front-end for the rawdraw simulator.

    python3 sim/web/server.py [--port 8000]

The browser keeps the button history and asks GET /frame?page=P&keys=a,b,c for the screen
after replaying those presses; the server runs sim_main and returns a PNG.
"""
import argparse
import io
import re
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM / "tools"))
import fb2png  # noqa: E402

PAGES = ["gallery", "photo_detail", "weather", "weather_detail"]
KEYS = {"up", "down", "up2", "down2", "uplong", "downlong", "boot", "boot2", "bootlong"}
W, H = 400, 300


def render(page, keys):
    """Replay `keys` on `page` in a fresh sim_main; return (png_bytes, [consumed flags])."""
    with tempfile.TemporaryDirectory() as out:
        cmd = [str(SIM / "build" / "sim_main"), "--page", page, "--out", out]
        if keys:
            cmd += ["--keys", ",".join(keys)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        if res.returncode != 0:
            raise RuntimeError(res.stderr.strip() or "sim_main failed")
        last = sorted(Path(out).glob("*.fb"))[-1]
        png = Path(fb2png.convert(str(last), W, H))
        consumed = [int(m) for m in re.findall(r"consumed=(\d)", res.stdout)]
        return png.read_bytes(), consumed


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/":
            html = (Path(__file__).parent / "index.html").read_bytes()
            return self._send(200, html, "text/html; charset=utf-8")
        if url.path == "/frame":
            q = parse_qs(url.query)
            page = q.get("page", [""])[0]
            keys = [k for k in q.get("keys", [""])[0].split(",") if k]
            if page not in PAGES or any(k not in KEYS for k in keys):
                return self._send(400, b"bad page or key", "text/plain")
            try:
                png, consumed = render(page, keys)
            except Exception as e:  # surfaced in the UI instead of a blank screen
                return self._send(500, str(e).encode(), "text/plain; charset=utf-8")
            last = str(consumed[-1]) if consumed else ""
            return self._send(200, png, "image/png", {"X-Consumed": last})
        if url.path == "/pages":
            import json
            return self._send(200, json.dumps(PAGES).encode(), "application/json")
        self._send(404, b"not found", "text/plain")

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()
    if not (SIM / "build" / "sim_main").exists():
        sys.exit("sim_main not built. Run: make -C sim")
    print(f"Simulator UI: http://{args.host}:{args.port}/")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
