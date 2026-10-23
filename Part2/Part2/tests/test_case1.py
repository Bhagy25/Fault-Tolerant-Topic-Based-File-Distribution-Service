import hashlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'Part2_Case1/server'
CLIENT = ROOT / 'Part2_Case1/client'


def available_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Case1Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.dataset = self.root / 'dataset'
        (self.dataset / 'gatto').mkdir(parents=True)
        self.expected = {}
        for name, payload in [('small.bin', b'test123'), ('large.bin', os.urandom(512 * 1024))]:
            (self.dataset / 'gatto' / name).write_bytes(payload)
            self.expected[name] = hashlib.sha256(payload).digest()
        (self.dataset / 'empty').mkdir()
        self.port = available_port()
        self.proc = None

    def start_server(self, probability):
        self.proc = subprocess.Popen([str(SERVER), str(self.port), str(self.dataset), str(probability)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_server)
        for _ in range(100):
            if self.proc.poll() is not None:
                self.fail('server exited prematurely: ' + self.proc.stderr.read().decode())
            try:
                with socket.create_connection(('127.0.0.1', self.port), 0.1):
                    return
            except OSError:
                time.sleep(0.01)
        self.fail('server did not open port')

    def stop_server(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.communicate()

    def call_client(self, topic, retries=0):
        return subprocess.run([str(CLIENT), '127.0.0.1', str(self.port), topic,
                               str(self.root / 'download'), str(retries)],
                              text=True, capture_output=True, timeout=20)

    def test_no_fault_integrity(self):
        self.start_server(0)
        result = self.call_client('gatto')
        self.assertEqual(result.returncode, 0, result.stderr)
        for name, digest in self.expected.items():
            actual = self.root / 'download' / 'gatto' / name
            self.assertEqual(hashlib.sha256(actual.read_bytes()).digest(), digest)
        self.assertIn('redundant_bytes=0', result.stdout)

    def test_empty_topic(self):
        self.start_server(0)
        result = self.call_client('empty')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('files=0', result.stdout)

    def test_guaranteed_fault_bounded_retries(self):
        self.start_server(1)
        result = self.call_client('gatto', 2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('retries=2', result.stdout)
        self.assertIn('status=ERROR', result.stdout)
        self.assertEqual(list((self.root / 'download' / 'gatto').iterdir()), [])

    def test_invalid_probability(self):
        self.stop_server()
        for p in ('-1', '1.2', 'nan', 'nonsense'):
            proc = subprocess.run([str(SERVER), str(available_port()), str(self.dataset), p],
                                  capture_output=True, timeout=4)
            self.assertNotEqual(proc.returncode, 0, p)

    def test_unknown_topic(self):
        self.start_server(0)
        result = self.call_client('unknown')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('retries=0', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
