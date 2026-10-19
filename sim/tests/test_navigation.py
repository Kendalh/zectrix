#!/usr/bin/env python3
"""Drives sim_device and checks the app-level navigation (Home page, launching apps, BOOT-long to Home).

    make -C sim test

The navigation code under test (HomeRenderer, AppShell, app registry) is the same code the firmware
runs; only the page/renderer glue in sim_ui.cc is simulator-specific.
"""
import json
import os
import subprocess
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

SIM = Path(__file__).resolve().parent.parent
PORT = 18080
TODAY = "2026-10-19"
REMINDERS_FILE = "/tmp/sim_test_reminders.json"
os.environ["SIM_AI_FAKE"] = "1"  # AI app: canned answers, no network, no real API key


class Device:
    def __init__(self):
        # Own reminders file and a fixed date: the tests must not touch the user's data or depend on the clock.
        Path(REMINDERS_FILE).unlink(missing_ok=True)
        self.proc = subprocess.Popen(
            [str(SIM / "build" / "sim_device"), "--http-port", str(PORT), "--out", "/tmp",
             "--reminders-file", REMINDERS_FILE, "--today", TODAY],
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

    # App order: 相册, 天气, 日历, 语音, AI 问答, 配网, 设置
    def open_app(self, index):
        for _ in range(index):
            self.dev.keys("down")
        return self.dev.keys("boot")

    def test_each_app_opens_and_boot_long_returns_home(self):
        expected = ["gallery", "weather", "calendar", "voice", "ai_chat", "ap_transfer", "settings"]
        for index, page in enumerate(expected):
            with self.subTest(app=index):
                self.assertEqual(self.open_app(index), page)
                self.assertEqual(self.dev.keys("bootlong"), "home")
                # Home remembers the last selection, so reset to the top for the next case.
                self.dev.cmd("reset")

    def test_home_scrolls_to_apps_beyond_the_fifth(self):
        home = self.dev.cmd("reset")[2]
        for _ in range(6):
            self.dev.keys("down")
        self.assertNotEqual(self.dev.cmd("screen")[2], home)  # the list scrolled
        self.assertEqual(self.dev.keys("boot"), "settings")  # 7th app

    def test_selection_wraps(self):
        self.assertEqual(self.dev.keys("up", "boot"), "settings")  # last entry: 设置
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


class VoiceAppTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dev = Device()

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.cmd("reset")
        self.dev.cmd("jump voice")

    def test_hold_up_records_and_release_saves_a_listed_recording(self):
        idle = self.dev.cmd("screen")[2]
        recording = self.dev.cmd("key uplong")[2]
        self.assertNotEqual(recording, idle)
        saved = self.dev.cmd("key uplongrelease")[2]
        self.assertNotEqual(saved, recording)
        self.assertNotEqual(saved, idle)  # the list now has an entry

    def test_boot_plays_and_stops_the_selected_recording(self):
        self.dev.keys("uplong", "uplongrelease")
        listed = self.dev.cmd("screen")[2]
        playing = self.dev.cmd("key boot")[2]
        self.assertNotEqual(playing, listed)
        self.assertEqual(self.dev.cmd("key boot")[2], listed)

    def test_down_long_deletes_the_selected_recording(self):
        self.dev.keys("uplong", "uplongrelease")
        with_one_more = self.dev.cmd("screen")[2]
        after_delete = self.dev.cmd("key downlong")[2]
        self.assertNotEqual(after_delete, with_one_more)
        # deleting everything leaves the empty-list screen, and more deletes do nothing
        for _ in range(25):
            self.dev.cmd("key downlong")
        empty = self.dev.cmd("screen")[2]
        self.assertEqual(self.dev.cmd("key downlong")[2], empty)
        self.dev.keys("uplong", "uplongrelease")
        self.assertNotEqual(self.dev.cmd("screen")[2], empty)

    def test_boot_long_returns_home_and_stops_recording(self):
        self.dev.cmd("key uplong")
        self.assertEqual(self.dev.keys("bootlong"), "home")
        self.dev.keys("down", "down", "down")  # 语音 is the 4th app
        self.assertEqual(self.dev.keys("boot"), "voice")
        idle = self.dev.cmd("screen")[2]
        # a new recording can start: the earlier one was stopped when the page was left
        self.assertNotEqual(self.dev.cmd("key uplong")[2], idle)

    def test_boot_long_does_not_record(self):
        before = self.dev.cmd("screen")[2]
        self.assertEqual(self.dev.keys("bootlong"), "home")
        self.dev.cmd("jump voice")
        self.assertEqual(self.dev.cmd("screen")[2], before)


class AiChatAppTest(unittest.TestCase):
    """AI 问答: everything on the device. Hold UP to speak (SIM_AI_FAKE gives a canned transcript and answer)."""

    SPOKEN = "（模拟语音）今天天气怎么样"

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.base = f"http://127.0.0.1:{PORT}"

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.cmd("reset")
        self.dev.cmd("jump ai_chat")

    def state(self):
        self.dev.proc.stdin.write("aistate\n")
        self.dev.proc.stdin.flush()
        line = self.dev.proc.stdout.readline().strip()
        assert line.startswith("AISTATE "), line
        return json.loads(line[len("AISTATE "):])

    def speak(self, wait_for_messages):
        """Hold and release UP, then wait until the conversation has `wait_for_messages` messages, answered."""
        self.dev.keys("uplong", "uplongrelease")
        for _ in range(200):
            st = self.state()
            if len(st["messages"]) >= wait_for_messages and not st["pending"]:
                return st
            time.sleep(0.05)
        self.fail("no answer: %s" % self.state())

    def test_screen_changes_once_when_the_answer_arrives(self):
        self.dev.cmd("jump ai_chat")
        before = self.dev.cmd("screen")[2]
        n = len(self.state()["messages"])
        self.dev.keys("uplong", "uplongrelease")
        seen = set()
        for _ in range(200):                                         # sample the screen while the question is processed
            st = self.state()
            seen.add(self.dev.cmd("screen")[2])
            if len(st["messages"]) >= n + 2 and not st["pending"]:
                break
            time.sleep(0.02)
        seen.add(self.dev.cmd("screen")[2])
        # Before the answer the panel shows exactly what it showed before (no question, no "thinking"); then question+answer.
        self.assertEqual(len(seen - {before}), 1, "question and answer must appear in a single refresh")

    def test_app_opens_from_home(self):
        self.dev.cmd("reset")
        self.dev.keys("down", "down", "down", "down")  # 相册, 天气, 日历, 语音, AI 问答
        self.assertEqual(self.dev.keys("boot"), "ai_chat")
        self.assertEqual(self.dev.keys("bootlong"), "home")

    def test_spoken_question_is_answered_and_remembered(self):
        self.new_session_via_keys()
        empty = self.dev.cmd("screen")[2]
        st = self.speak(2)
        self.assertEqual(st["messages"][0]["text"], self.SPOKEN)  # recording -> transcript -> question
        self.assertIn("1 条上下文", st["messages"][1]["text"])
        answered = self.dev.cmd("screen")[2]
        self.assertNotEqual(answered, empty)
        st = self.speak(4)                                          # same session: earlier turn is sent too
        self.assertIn("3 条上下文", st["messages"][3]["text"])
        self.assertNotEqual(self.dev.cmd("screen")[2], answered)

    def new_session_via_keys(self):
        before = self.state()
        self.dev.keys("downlong")
        after = self.state()
        self.assertEqual(len(after["sessions"]), len(before["sessions"]) + 1)
        self.assertEqual(after["messages"], [])
        return after["current"]

    def test_sessions_keep_separate_context_and_are_managed_in_the_list(self):
        first = self.new_session_via_keys()
        self.speak(2)
        second = self.new_session_via_keys()
        self.assertNotEqual(first, second)
        st = self.speak(2)
        self.assertIn("1 条上下文", st["messages"][1]["text"])      # not the first session's history

        page_list = self.dev.keys("boot")                            # BOOT opens the session list
        self.assertEqual(page_list, "ai_chat")
        list_screen = self.dev.cmd("screen")[2]
        self.assertNotEqual(list_screen, self.dev.cmd("key down")[2])  # selection moved
        sessions = self.state()["sessions"]
        # list rows: "+ 新会话", then sessions in order. The selection starts on the current (last) session.
        self.dev.keys("up", "up")                                    # the earlier `down` wrapped to row 0
        self.dev.keys("boot")                                        # open the previous session
        self.assertEqual(self.state()["current"], sessions[-2]["id"])
        self.assertEqual(len(self.state()["messages"]), 2)

        self.dev.keys("boot")                                        # list again: delete the selected session
        self.dev.keys("downlong")
        self.assertEqual(len(self.state()["sessions"]), len(sessions) - 1)
        self.dev.keys("bootlong")

    def test_list_new_session_row_and_delete_everything(self):
        self.dev.keys("boot", "boot")                                # list -> "+ 新会话" is row 0? select it
        n = len(self.state()["sessions"])
        self.dev.cmd("jump ai_chat")
        self.dev.keys("boot")
        for _ in range(n + 2):
            self.dev.keys("down")                                    # wraps around to "+ 新会话"
        self.assertEqual(len(self.state()["sessions"]), n)
        self.dev.keys("down", "downlong")                            # row 1: delete (repeat to empty the list)
        for _ in range(12):
            self.dev.keys("downlong")
        st = self.state()
        self.assertEqual(len(st["sessions"]), 1)                     # never zero sessions
        self.assertEqual(st["messages"], [])

    def test_long_up_in_the_list_creates_a_session(self):
        self.dev.cmd("jump ai_chat")
        before = self.state()
        n = len(before["sessions"])
        if n >= 8:
            self.skipTest("session limit reached")
        self.dev.keys("boot")                                        # session list
        self.dev.keys("uplong", "uplongrelease")                     # long UP: new session, back to the conversation
        after = self.state()
        self.assertEqual(len(after["sessions"]), n + 1)
        self.assertEqual(after["messages"], [])
        self.assertNotEqual(after["current"], before["current"])

    def test_leaving_while_recording_does_not_ask_a_question(self):
        self.dev.cmd("key uplong")
        self.assertEqual(self.dev.keys("bootlong"), "home")
        time.sleep(1.2)
        self.assertFalse(self.state()["pending"])
        self.assertEqual(self.state()["messages"], [])

    def test_spoken_question_does_not_show_up_in_the_voice_app(self):
        self.dev.cmd("jump voice")
        empty = self.dev.cmd("screen")[2]
        self.dev.cmd("jump ai_chat")
        self.speak(len(self.state()["messages"]) + 2)
        self.dev.cmd("jump voice")
        self.assertEqual(self.dev.cmd("screen")[2], empty)           # the recording was claimed and deleted
        self.dev.keys("uplong", "uplongrelease")                     # a real voice recording is still kept
        self.assertNotEqual(self.dev.cmd("screen")[2], empty)

    def api(self, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method="GET" if body is None else "POST",
                                     headers={"Content-Type": "application/json"})
        try:
            return json.load(urllib.request.urlopen(req))
        except urllib.error.HTTPError as e:
            return json.load(e) if e.headers.get("Content-Type", "").startswith("application/json") else {"http": e.code}

    def test_web_page_only_sets_keys(self):
        st = self.api("/api/ai/state")
        self.assertTrue(st["hasKey"])
        self.assertEqual(set(st), {"success", "started", "hasKey"})  # no key itself, no conversation
        self.assertFalse(self.api("/api/ai/key", {"key": "has space"})["success"])
        self.assertFalse(self.api("/api/ai/key", {"key": ""})["success"])
        self.assertTrue(self.api("/api/ai/key", {"key": "sk-abc.def"})["success"])
        self.assertIn("http", self.api("/api/ai/ask", {"text": "hi"}))  # asking from the web is not offered


class AiChatPagingTest(unittest.TestCase):
    """A long spoken question gives a long conversation: UP / DOWN turn pages."""

    @classmethod
    def setUpClass(cls):
        os.environ["SIM_AI_SPEECH_TEXT"] = "长" * 400
        cls.dev = Device()
        del os.environ["SIM_AI_SPEECH_TEXT"]

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def test_up_down_page_through_a_long_answer(self):
        self.dev.cmd("jump ai_chat")
        self.dev.keys("uplong", "uplongrelease")
        for _ in range(200):
            self.dev.proc.stdin.write("aistate\n")
            self.dev.proc.stdin.flush()
            st = json.loads(self.dev.proc.stdout.readline().strip()[len("AISTATE "):])
            if len(st["messages"]) >= 2 and not st["pending"]:
                break
            time.sleep(0.05)
        first = self.dev.cmd("screen")[2]
        second = self.dev.cmd("key down")[2]
        self.assertNotEqual(second, first)
        self.assertEqual(self.dev.cmd("key up")[2], first)


class CalendarAppTest(unittest.TestCase):
    """日历: month grid with a bell on dates that have reminders. The sim date is fixed to TODAY (2026-10-19)."""

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.base = f"http://127.0.0.1:{PORT}"

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.cmd("reset")
        self.clear("2026-10-19")
        self.clear("2026-10-20")
        self.dev.cmd("jump calendar")

    def api(self, date, items):
        req = urllib.request.Request(f"{self.base}/api/reminders", method="POST",
                                     data=json.dumps({"date": date, "items": items}).encode())
        try:
            return json.load(urllib.request.urlopen(req))
        except urllib.error.HTTPError as e:  # SendError answers 200, but be explicit about other codes
            return json.load(e)

    def clear(self, date):
        self.assertTrue(self.api(date, [])["success"])

    def screen(self):
        return self.dev.cmd("screen")[2]

    def test_up_down_change_month_and_boot_returns_to_current_month(self):
        current = self.screen()
        self.assertNotEqual(self.dev.cmd("key down")[2], current)
        self.assertNotEqual(self.dev.cmd("key down")[2], current)
        self.assertEqual(self.dev.cmd("key boot")[2], current)  # BOOT from another month: back to this one
        self.dev.cmd("key up")
        self.assertEqual(self.dev.cmd("key down")[2], current)  # previous then next is a round trip

    def test_boot_in_current_month_shows_today_and_boot_returns(self):
        month = self.screen()
        today = self.dev.cmd("key boot")[2]
        self.assertNotEqual(today, month)
        self.assertEqual(self.dev.cmd("key boot")[2], month)

    def test_today_view_lists_todays_reminders(self):
        self.dev.cmd("key boot")
        empty = self.screen()
        self.assertTrue(self.api("2026-10-19", ["09:30 开会"])["success"])
        self.assertNotEqual(self.screen(), empty)

    def test_reminder_adds_an_icon_to_its_date_only_in_that_month(self):
        month = self.screen()
        next_month = self.dev.cmd("key down")[2]
        self.dev.cmd("key boot")
        self.assertTrue(self.api("2026-10-20", ["交报告"])["success"])
        self.assertNotEqual(self.screen(), month)  # October gained a bell
        self.assertEqual(self.dev.cmd("key down")[2], next_month)  # November did not
        self.clear("2026-10-20")
        self.assertEqual(self.dev.cmd("key boot")[2], month)

    def test_boot_long_from_today_view_goes_home(self):
        self.dev.cmd("key boot")
        self.assertEqual(self.dev.keys("bootlong"), "home")

    def test_api_validation_and_persistence(self):
        self.assertEqual(self.api("2026-02-30", ["x"])["error"], "bad_date")
        self.assertEqual(self.api("1999-01-01", ["x"])["error"], "bad_date")
        self.assertEqual(self.api("2026-10-19", ["x" * 97])["error"], "text_too_long")
        self.assertEqual(self.api("2026-10-19", ["a"] * 7)["error"], "too_many")
        saved = self.api("2026-10-19", ["  买牛奶  ", "", "开会"])
        self.assertEqual(saved["reminders"]["2026-10-19"], ["买牛奶", "开会"])  # trimmed, blanks dropped
        self.assertEqual(json.loads(Path(REMINDERS_FILE).read_text(encoding="utf-8"))["2026-10-19"],
                         ["买牛奶", "开会"])
        self.assertNotIn("2026-10-19", self.api("2026-10-19", [])["reminders"])

    def test_festivals_api(self):
        j = json.load(urllib.request.urlopen(f"{self.base}/api/festivals?year=2026"))
        self.assertEqual(j["festivals"]["2026-02-17"], ["春节"])
        self.assertEqual(j["festivals"]["2026-10-01"], ["国庆节"])
        self.assertNotIn("2026-07-14", j["festivals"])
        for bad in ("2009", "2051", "abc", ""):
            r = json.load(urllib.request.urlopen(f"{self.base}/api/festivals?year={bad}"))
            self.assertEqual(r["error"], "bad_date")

    def test_calendar_web_page_is_served(self):
        html = urllib.request.urlopen(f"{self.base}/calendar").read().decode("utf-8")
        self.assertIn("/api/reminders", html)
        self.assertIn("/calendar", urllib.request.urlopen(f"{self.base}/").read().decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
