#!/usr/bin/env python3
"""Independent local functional and integrity checks for Part 1.

Run: python3 tests/test_part1.py
Uses temporary datasets; no Kaggle dataset or LAN is required.
"""
import concurrent.futures
import threading
import hashlib
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "Part1" / "server"
CLIENT = ROOT / "Part1" / "client"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def digest(path):
    hasher = hashlib.sha256()
    with open(path, "rb") as source:
        for data in iter(lambda: source.read(65536), b""):
            hasher.update(data)
    return hasher.hexdigest()


class Part1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "Part1")], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="it305_phase1_")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.topics = self.root / "topics"
        self.topics.mkdir()
        self.animals = self.topics / "gatto"
        self.animals.mkdir()
        self.empty = self.topics / "empty"
        self.empty.mkdir()
        files = {
            "zero.bin": b"",
            "tiny.txt": b"binary\x00\x01\xff\n",
            "image with spaces.jpeg": bytes((i % 251 for i in range(300))),
            "exact_chunk.bin": os.urandom(65536),
            "over_chunk.bin": os.urandom(65537),
            "multi_chunk.bin": os.urandom(262247),
        }
        for name, data in files.items():
            (self.animals / name).write_bytes(data)
        (self.animals / "subdir").mkdir()  # Directories are not files.
        (self.animals / "subdir" / "should_ignore.bin").write_bytes(b"NO")
        (self.animals / "pointer.bin").symlink_to(self.animals / "tiny.txt")
        self.expected_files = list(files)
        self.server = None

    def start_server(self, mode="multi"):
        port = free_port()
        self.port = port
        self.server = subprocess.Popen(
            [str(SERVER), str(port), str(self.topics), mode],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        self.addCleanup(self.stop_server)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                self.fail("server exited immediately")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.02)
        self.fail("server never started")

    def stop_server(self):
        if self.server is not None and self.server.poll() is None:
            self.server.send_signal(signal.SIGTERM)
            try:
                self.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=3)

    def run_client(self, topic, suffix="download"):
        out = self.root / suffix
        return subprocess.run([str(CLIENT), "127.0.0.1", str(self.port), topic, str(out)],
                              capture_output=True, text=True, timeout=10)

    def check_download(self, out):
        received = out / "gatto"
        actual_files = sorted(p.name for p in received.iterdir() if p.is_file())
        self.assertEqual(sorted(self.expected_files), actual_files)
        for name in self.expected_files:
            self.assertEqual(digest(self.animals / name), digest(received / name), name)

    def test_single_threaded_files_match(self):
        self.start_server("single")
        result = self.run_client("gatto")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("status=OK", result.stdout)
        self.check_download(self.root / "download")

    def test_multi_threaded_files_match(self):
        self.start_server("multi")
        result = self.run_client("gatto")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_download(self.root / "download")

    def test_default_cli_mode(self):
        port = free_port()
        self.port = port
        self.server = subprocess.Popen([str(SERVER), str(port), str(self.topics)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_server)
        time.sleep(0.1)
        self.assertIsNone(self.server.poll())
        result = self.run_client("gatto")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_empty_topic(self):
        self.start_server()
        result = self.run_client("empty")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("files=0", result.stdout)
        self.assertEqual(list((self.root / "download" / "empty").iterdir()), [])

    def test_unknown_topic(self):
        self.start_server()
        result = self.run_client("unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("UNKNOWN_TOPIC", result.stderr)

    def test_path_traversal_rejected(self):
        self.start_server()
        for request in (b"GET ../etc\n", b"GET gatto/../empty\n", b"BROKEN\n"):
            with self.subTest(request=request):
                with socket.create_connection(("127.0.0.1", self.port), timeout=2) as s:
                    s.sendall(request)
                    response = s.recv(256)
                    self.assertTrue(response.startswith(b"ERR "), response)

    def test_concurrent_clients(self):
        self.start_server("multi")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda i: self.run_client("gatto", f"worker_{i}"), range(8)))
        for i, result in enumerate(results):
            self.assertEqual(result.returncode, 0, f"worker {i}: {result.stderr}")
            self.check_download(self.root / f"worker_{i}")

    def test_sequential_clients(self):
        self.start_server("single")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda i: self.run_client("gatto", f"worker_{i}"), range(4)))
        for i, result in enumerate(results):
            self.assertEqual(result.returncode, 0, f"worker {i}: {result.stderr}")
            self.check_download(self.root / f"worker_{i}")

    def test_fragmented_server_response(self):
        """A synthetic server writes partial headers and partial payloads."""
        payload = b"\x00hello\xff" * 9500
        response = b"OK 1\nFILE fragmented.bin " + str(len(payload)).encode() + b"\n" + payload + b"DONE\n"
        port = free_port()
        self.port = port
        def fake_server():
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", port))
                listener.listen(1)
                with listener.accept()[0] as conn:
                    conn.recv(128)
                    # Deliberately split the metadata and binary stream arbitrarily.
                    for index in range(0, len(response), 793):
                        conn.sendall(response[index:index + 793])
        thread = threading.Thread(target=fake_server)
        thread.start()
        try:
            result = self.run_client("gatto")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((self.root / "download" / "gatto" / "fragmented.bin").read_bytes(), payload)
        finally:
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())

    def test_disconnection_removes_partial_file(self):
        port = free_port()
        self.port = port
        def fake_server():
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", port))
                listener.listen(1)
                with listener.accept()[0] as conn:
                    conn.recv(128)
                    conn.sendall(b"OK 1\nFILE partial.bin 100\n12345")
        thread = threading.Thread(target=fake_server)
        thread.start()
        try:
            result = self.run_client("gatto")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "download" / "gatto" / "partial.bin").exists())
        finally:
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())

    def test_missing_cli_arguments(self):
        for binary in (SERVER, CLIENT):
            result = subprocess.run([str(binary)], capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)

    def test_bad_ports(self):
        for value in ("0", "65536", "notaport", "-1"):
            result = subprocess.run([str(SERVER), value, str(self.topics)],
                                    capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)

    def test_symlinked_topic_ignored(self):
        (self.topics / "offroot").symlink_to(self.animals, target_is_directory=True)
        self.start_server()
        result = self.run_client("offroot")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
