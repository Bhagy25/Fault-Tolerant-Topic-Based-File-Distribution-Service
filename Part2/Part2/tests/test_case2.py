#!/usr/bin/env python3
"""Case 2 regression suite: localhost C server/client and checkpoint recovery."""
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'Part2_Case2'


class Case2Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        (self.path / 'topics' / 'gatto').mkdir(parents=True)
        (self.path / 'topics' / 'empty').mkdir()
        self.data = bytes(i % 251 for i in range(150000))
        (self.path / 'topics' / 'gatto' / 'cat with spaces.bin').write_bytes(self.data)
        (self.path / 'topics' / 'gatto' / 'another.txt').write_bytes(b'hello')
        import socket
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.server = None
        self.addCleanup(self.stop_server)

    def start_server(self, probability):
        self.server = subprocess.Popen([str(BIN / 'server'), str(self.port),
            str(self.path / 'topics'), str(probability)], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        time.sleep(0.10)
        self.assertIsNone(self.server.poll())

    def stop_server(self):
        if self.server and self.server.poll() is None:
            self.server.send_signal(signal.SIGINT)
            try: self.server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait()
        self.server = None

    def client(self, topic='gatto', retries='0'):
        return subprocess.run([str(BIN / 'client'), '127.0.0.1', str(self.port),
            topic, str(self.path / 'output'), retries], capture_output=True,
            text=True, timeout=12)

    def assert_matches(self):
        for item in (self.path / 'topics' / 'gatto').iterdir():
            self.assertEqual(item.read_bytes(),
                (self.path / 'output' / 'gatto' / item.name).read_bytes())

    def test_no_fault_and_spaces(self):
        self.start_server(0)
        result = self.client()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_matches()
        self.assertFalse((self.path / 'output' / 'gatto' / '.checkpoint_case2').exists())

    def test_empty(self):
        self.start_server(0)
        result = self.client('empty')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fault_then_restart_server_then_resume(self):
        self.start_server(1)
        fail = self.client()
        self.assertNotEqual(fail.returncode, 0)
        checkpoint = self.path / 'output' / 'gatto' / '.checkpoint_case2'
        self.assertTrue(checkpoint.exists())
        self.assertGreater(int(checkpoint.read_text().splitlines()[2]), 0)
        self.stop_server()
        self.start_server(0)
        success = self.client()
        self.assertEqual(success.returncode, 0, success.stderr)
        self.assertIn('RESUME accepted', success.stdout)
        self.assert_matches()
        self.assertFalse(checkpoint.exists())

    def test_stale_checkpoint_restarts(self):
        self.start_server(1)
        self.assertNotEqual(self.client().returncode, 0)
        self.stop_server()
        (self.path / 'topics' / 'gatto' / 'cat with spaces.bin').write_bytes(b'changed')
        self.start_server(0)
        success = self.client(retries='1')
        self.assertEqual(success.returncode, 0, success.stderr)
        self.assertIn('STALE CHECKPOINT', success.stderr)
        self.assert_matches()


if __name__ == '__main__':
    unittest.main(verbosity=2)
