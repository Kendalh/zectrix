#!/usr/bin/env python3
"""Drives sim_device and checks the app-level navigation (Home page, launching apps, BOOT-long to Home).

    make -C sim test

The navigation code under test (HomeRenderer, AppShell, app registry) is the same code the firmware
runs; only the page/renderer glue in sim_ui.cc is simulator-specific.
"""
import json
import subprocess
import unittest
import urllib.request
from pathlib import Path

SIM = Path(__file__).resolve().parent.parent
PORT = 18080


class Device:
    def __init__(self):
        self.proc = subprocess.Popen(
            [str(SIM / "build" / "sim_device"), "--http-port", str(PORT), "--out", "/tmp"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        assert self.proc.stdout.readline().startswith("READY")

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline().strip()
        assert reply.startswith("FRAME"), reply
        _, path, consumed, page = reply.split(" ")
        return page, int(consumed), Path(path).read_bytes()

    def keys(self, *names):
        page = None
        for n in names:
            page, _, _ = self.cmd("key " + n)
        return page

    def close(self):
        self.proc.stdin.write("quit\n")
        self.proc.stdin.flush()
        self.proc.wait(timeout=5)


class NavigationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dev = Device()

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        page, _, _ = self.dev.cmd("reset")
        self.assertEqual(page, "home")

    # App order: 相册, 天气, 传图, 设置, 配网
    def open_app(self, index):
        for _ in range(index):
            self.dev.keys("down")
        return self.dev.keys("boot")

    def test_each_app_opens_and_boot_long_returns_home(self):
        expected = ["gallery", "weather", "ap_transfer", "settings", "ap_transfer"]
        for index, page in enumerate(expected):
            with self.subTest(app=index):
                self.assertEqual(self.open_app(index), page)
                self.assertEqual(self.dev.keys("bootlong"), "home")
                # Home remembers the last selection, so reset to the top for the next case.
                self.dev.cmd("reset")

    def test_selection_wraps(self):
        self.assertEqual(self.dev.keys("up", "boot"), "ap_transfer")  # last entry: 配网
        self.dev.keys("bootlong")
        self.assertEqual(self.dev.keys("down", "boot"), "gallery")    # wrapped to first

    def test_home_remembers_selection(self):
        self.assertEqual(self.open_app(1), "weather")
        self.assertEqual(self.dev.keys("bootlong"), "home")
        self.assertEqual(self.dev.keys("boot"), "weather")

    def test_boot_long_on_home_stays_home(self):
        self.assertEqual(self.dev.keys("bootlong"), "home")

    def test_boot_long_from_fullscreen_photo_goes_home_not_to_hotspot(self):
        self.assertEqual(self.open_app(0), "gallery")
        self.assertEqual(self.dev.keys("boot"), "gallery")  # fullscreen (no photos seeded: still the gallery page)
        self.assertEqual(self.dev.keys("bootlong"), "home")

    def test_up_down_chord_starts_wifi_config(self):
        self.assertEqual(self.dev.keys("updownlong"), "ap_transfer")
        self.assertEqual(self.dev.keys("bootlong"), "home")

    def test_unknown_key_is_an_error(self):
        self.dev.proc.stdin.write("key nonsense\n")
        self.dev.proc.stdin.flush()
        self.assertTrue(self.dev.proc.stdout.readline().startswith("ERR"))

    def test_show_photo_from_http_works_from_the_home_page(self):
        """Regression: /photo/show used to select the photo before the page switch reset the gallery."""
        # A recognisable full-screen image: fullscreen mode draws it 1:1, so the frame equals the data.
        data = bytes((i * 7) % 256 for i in range(30000))
        base = f"http://127.0.0.1:{PORT}"
        req = urllib.request.Request(f"{base}/upload?format=bwry2bpp", data=data, method="POST")
        photo_id = json.load(urllib.request.urlopen(req))["id"]
        self.assertEqual(self.dev.cmd("reset")[0], "home")

        show = urllib.request.Request(f"{base}/photo/show", data=json.dumps({"id": photo_id}).encode(),
                                      method="POST")
        self.assertTrue(json.load(urllib.request.urlopen(show))["success"])
        page, _, frame = self.dev.cmd("screen")
        self.assertEqual(page, "gallery")
        self.assertEqual(frame, data)


if __name__ == "__main__":
    unittest.main()
