#!/usr/bin/env python3
"""End-to-end tests: run the real serialscope binary against a pseudo-terminal.

The pty master plays the role of the embedded board; serialscope opens the
pty slave as if it were /dev/ttyUSB0. No hardware needed.
"""
import os
import re
import select
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "serialscope")


class FakeBoard:
    def __init__(self):
        self.master, self.slave = os.openpty()
        self.path = os.ttyname(self.slave)

    def send(self, data: bytes):
        os.write(self.master, data)

    def recv(self, timeout=1.0) -> bytes:
        out = b""
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if r:
                out += os.read(self.master, 4096)
        return out

    def unplug(self):
        os.close(self.master)
        os.close(self.slave)


def run(board, *args, stdin=subprocess.DEVNULL):
    return subprocess.Popen([BIN, "--no-color", *args, board.path], stdin=stdin,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def finish(proc, timeout=2.0):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    out, err = proc.communicate(timeout=timeout)
    return out.decode(), err.decode()


class SerialscopeTests(unittest.TestCase):
    def setUp(self):
        self.board = FakeBoard()

    def tearDown(self):
        try:
            self.board.unplug()
        except OSError:
            pass

    def test_plain_text(self):
        p = run(self.board)
        time.sleep(0.2)
        self.board.send(b"hello\r\nworld\n")
        time.sleep(0.3)
        out, _ = finish(p)
        self.assertIn("-- connected to", out)
        self.assertIn("hello\nworld\n", out)       # CRLF normalised to LF

    def test_absolute_timestamps(self):
        p = run(self.board, "-t")
        time.sleep(0.2)
        self.board.send(b"boot ok\n")
        time.sleep(0.3)
        out, _ = finish(p)
        self.assertRegex(out, r"\[\d\d:\d\d:\d\d\.\d{3}\] boot ok")

    def test_relative_timestamps(self):
        p = run(self.board, "-T")
        time.sleep(0.2)
        self.board.send(b"a\nb\n")
        time.sleep(0.3)
        out, _ = finish(p)
        stamps = re.findall(r"\[\s*(\d+\.\d{3})\] [ab]", out)
        self.assertEqual(len(stamps), 2)
        self.assertLess(float(stamps[0]), 5.0)

    def test_filter(self):
        p = run(self.board, "-f", "ERR|WARN")
        time.sleep(0.2)
        self.board.send(b"INFO all good\nERR sensor timeout\ndebug\nWARN low battery\n")
        time.sleep(0.3)
        out, _ = finish(p)
        self.assertIn("ERR sensor timeout", out)
        self.assertIn("WARN low battery", out)
        self.assertNotIn("all good", out)
        self.assertNotIn("debug", out)

    def test_filter_ignore_case(self):
        p = run(self.board, "-i", "-f", "error")
        time.sleep(0.2)
        self.board.send(b"ERROR one\nfine\n")
        time.sleep(0.3)
        out, _ = finish(p)
        self.assertIn("ERROR one", out)
        self.assertNotIn("fine", out)

    def test_highlight_colors_terminal_not_log(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "s.log")
            p = subprocess.Popen([BIN, "--color", "-H", "fail", "-l", log, self.board.path],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(0.2)
            self.board.send(b"self test fail\n")
            time.sleep(0.3)
            out, _ = finish(p)
            self.assertIn("self test \033[1;31mfail\033[0m", out)   # highlighted on screen
            with open(log) as f:
                text = f.read()
            self.assertIn("self test fail\n", text)
            self.assertNotIn("\033[", text)

    def test_partial_line_flushed_when_idle(self):
        p = run(self.board, "-f", ".")           # line-buffered mode
        time.sleep(0.2)
        self.board.send(b"prompt> ")             # no newline, like a shell prompt
        time.sleep(0.5)
        out, _ = finish(p)
        self.assertIn("prompt>", out)

    def test_hex_dump(self):
        p = run(self.board, "-x")
        time.sleep(0.2)
        self.board.send(bytes(range(0x41, 0x41 + 20)))   # A..T
        time.sleep(0.4)
        out, _ = finish(p)
        self.assertIn("00000000  41 42 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  |ABCDEFGHIJKLMNOP|", out)
        self.assertIn("00000010  51 52 53 54", out)
        self.assertIn("|QRST|", out)

    def test_keyboard_forwarding_and_eol(self):
        p = run(self.board, "-e", "crlf", stdin=subprocess.PIPE)
        time.sleep(0.2)
        p.stdin.write(b"AT\n")
        p.stdin.flush()
        got = self.board.recv(0.5)
        finish(p)
        self.assertEqual(got, b"AT\r\n")

    def test_log_appends(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "s.log")
            for word in (b"first\n", b"second\n"):
                p = run(self.board, "-l", log)
                time.sleep(0.2)
                self.board.send(word)
                time.sleep(0.3)
                finish(p)
            with open(log) as f:
                text = f.read()
            self.assertIn("first", text)
            self.assertIn("second", text)

    def test_disconnect_without_reconnect_exits(self):
        p = run(self.board, "-n")
        time.sleep(0.2)
        self.board.unplug()
        out, _ = finish(p, timeout=3)
        self.assertIn("disconnected", out)
        self.assertEqual(p.returncode, 1)

    def test_disconnect_keeps_waiting(self):
        p = run(self.board)
        time.sleep(0.2)
        self.board.unplug()
        time.sleep(0.8)
        self.assertIsNone(p.poll(), "should keep running and wait for the device")
        out, _ = finish(p)
        self.assertIn("disconnected, waiting", out)


class CliTests(unittest.TestCase):
    def test_version(self):
        out = subprocess.run([BIN, "--version"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0)
        self.assertRegex(out.stdout, r"serialscope \d+\.\d+\.\d+")

    def test_bad_baud(self):
        out = subprocess.run([BIN, "-b", "12345", "/dev/null"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 2)
        self.assertIn("unsupported baud", out.stderr)

    def test_bad_regex(self):
        out = subprocess.run([BIN, "-f", "(", "/dev/null"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 2)
        self.assertIn("bad filter regex", out.stderr)

    def test_missing_device(self):
        out = subprocess.run([BIN, "-n", "/dev/does-not-exist"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 1)

    def test_list(self):
        out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
