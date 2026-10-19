#!/usr/bin/env python3
"""Local web front-end for the PC simulator.

    python3 sim/web/server.py [--port 8000] [--device-port 8080] [--no-seed] [--demo-weather]

Starts one long-lived sim_device process. It runs the same UI code as the firmware (Home page,
app shell, page renderers) and the firmware's real HTTP upload service on --device-port (open it in a
browser, or curl the API from docs/LAN_PHOTO_PUSH_API.md); uploaded photos show up in the gallery.

The device keeps its own state, so each button press is sent once:
    GET /key?name=boot      press a key (up down boot, <name>long, updownlong) -> PNG of the screen
    GET /screen             current screen
    GET /jump?page=weather  debug: jump straight to a page
    GET /reset              like a reboot: back to the Home page
Response headers: X-Consumed (did the page handle the key: 1/0/-1) and X-Page (current page).
"""
import argparse
import atexit
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM / "tools"))
import fb2png  # noqa: E402

PAGES = ["home", "gallery", "weather", "weather_detail", "photo_detail", "settings", "ap_transfer", "voice", "ai_chat", "calendar"]
MIC_FILE = "/tmp/zectrix_sim_mic.wav"  # keep in sync with kMicFile in fake/voice_backend_fake.cc
KEYS = {"up", "down", "boot", "uplong", "downlong", "bootlong", "updownlong", "uplongrelease"}
W, H = 400, 300


class Device:
    """Owns the sim_device subprocess; commands are serialised because stdin/stdout are shared."""

    def __init__(self, http_port, seed, demo_weather):
        cmd = [str(SIM / "build" / "sim_device"), "--http-port", str(http_port), "--out", "/tmp"]
        if seed:
            cmd.append("--seed-photos")
        if demo_weather:
            cmd.append("--demo-weather")
        self.http_port = http_port
        self.lock = threading.Lock()
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        if not self.proc.stdout.readline().startswith("READY"):
            raise RuntimeError("sim_device failed to start (is port %d free?)" % http_port)
        atexit.register(self.close)

    def command(self, line):
        """Send one command; returns (png_bytes, consumed, page)."""
        with self.lock:
            if self.proc.poll() is not None:
                raise RuntimeError("sim_device exited")
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
            reply = self.proc.stdout.readline().strip()
        if reply.startswith("ERR"):
            raise ValueError(reply[4:])
        _, fb, consumed, page = reply.split(" ")
        return Path(fb2png.convert(fb, W, H)).read_bytes(), consumed, page

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()


DEVICE = None


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

    def _frame(self, line):
        try:
            png, consumed, page = DEVICE.command(line)
        except ValueError as e:
            return self._send(400, str(e).encode(), "text/plain; charset=utf-8")
        except Exception as e:  # surfaced in the UI instead of a blank screen
            return self._send(500, str(e).encode(), "text/plain; charset=utf-8")
        self._send(200, png, "image/png", {"X-Consumed": consumed, "X-Page": page})

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        if url.path == "/":
            html = (Path(__file__).parent / "index.html").read_bytes()
            return self._send(200, html, "text/html; charset=utf-8")
        if url.path == "/key":
            name = q.get("name", [""])[0]
            if name not in KEYS:
                return self._send(400, b"unknown key", "text/plain")
            return self._frame("key " + name)
        if url.path == "/screen":
            return self._frame("screen")
        if url.path == "/jump":
            page = q.get("page", [""])[0]
            if page not in PAGES:
                return self._send(400, b"unknown page", "text/plain")
            return self._frame("jump " + page)
        if url.path == "/reset":
            return self._frame("reset")
        if url.path == "/config":
            body = json.dumps({"device_port": DEVICE.http_port, "pages": PAGES}).encode()
            return self._send(200, body, "application/json")
        self._send(404, b"not found", "text/plain")

    def do_POST(self):
        # Browser microphone: a 16 kHz mono WAV recorded while UP is held. The fake recorder picks it up when the
        # recording ends (see fake/voice_backend_fake.cc, kMicFile).
        if urlparse(self.path).path != "/voice":
            return self._send(404, b"not found", "text/plain")
        n = int(self.headers.get("Content-Length", "0"))
        if n <= 0 or n > 8 * 1024 * 1024:
            return self._send(400, b"bad size", "text/plain")
        Path(MIC_FILE).write_bytes(self.rfile.read(n))
        self._send(200, b"ok", "text/plain")

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--device-port", type=int, default=8080,
                    help="port of the simulated device's HTTP upload service")
    ap.add_argument("--no-seed", action="store_true", help="start with an empty album")
    ap.add_argument("--demo-weather", action="store_true",
                    help="feed demo data to the weather pages (the device has no weather data source yet)")
    args = ap.parse_args()
    if not (SIM / "build" / "sim_device").exists():
        sys.exit("sim_device not built. Run: make -C sim")
    global DEVICE
    DEVICE = Device(args.device_port, seed=not args.no_seed, demo_weather=args.demo_weather)
    print(f"Simulator UI:           http://{args.host}:{args.port}/")
    print(f"Device upload service:  http://127.0.0.1:{args.device_port}/")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
