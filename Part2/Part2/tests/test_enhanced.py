#!/usr/bin/env python3
"""Black-box functional regression for enhanced C server/client."""
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'Part2_Case2_Enhanced/server'
CLIENT = ROOT / 'Part2_Case2_Enhanced/client'

def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port

def equals(src, dst):
    return (sorted(p.relative_to(src) for p in src.rglob('*') if p.is_file()) ==
            sorted(p.relative_to(dst) for p in dst.rglob('*') if p.is_file() and not any(x.startswith('.') for x in p.relative_to(dst).parts))
            and all(hashlib.sha256(p.read_bytes()).digest() == hashlib.sha256((dst / p.relative_to(src)).read_bytes()).digest()
                    for p in src.rglob('*') if p.is_file()))

class EnhancedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'
        (self.data / 'gatto').mkdir(parents=True)
        (self.data / 'empty').mkdir()
        (self.data / 'gatto' / 'file with spaces.bin').write_bytes(os.urandom(151000))
        (self.data / 'gatto' / 'small.txt').write_bytes(b'hello world')
        self.port = free_port()
        self.process = None
        self.addCleanup(self.stop)

    def start(self, probability):
        self.stop()
        self.process = subprocess.Popen([str(SERVER), str(self.port), str(self.data), str(probability)],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(.12)
        self.assertIsNone(self.process.poll(), 'server did not start')

    def stop(self):
        if self.process is not None:
            self.process.terminate()
            try: self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
            self.process = None

    def client(self, name, n=4, retries=0, topic='gatto'):
        return subprocess.run([str(CLIENT), '127.0.0.1', str(self.port), topic,
                               str(self.root / name), str(n), str(retries)],
                              capture_output=True, text=True, timeout=20)

    def test_four_parallel_connections(self):
        self.start(0)
        response = self.client('out', 4)
        self.assertEqual(response.returncode, 0, response.stdout + response.stderr)
        self.assertTrue(equals(self.data / 'gatto', self.root / 'out' / 'gatto'))

    def test_one_parallel_connection(self):
        self.start(0)
        response = self.client('out', 1)
        self.assertEqual(response.returncode, 0, response.stdout + response.stderr)
        self.assertTrue(equals(self.data / 'gatto', self.root / 'out' / 'gatto'))

    def test_empty_topic(self):
        self.start(0)
        response = self.client('out', 4, topic='empty')
        self.assertEqual(response.returncode, 0, response.stdout + response.stderr)

    def test_forced_fault_and_resume(self):
        self.start(1)
        response = self.client('out', 4)
        self.assertNotEqual(response.returncode, 0)
        self.assertTrue(list((self.root / 'out').rglob('.checkpoint_case2')))
        self.start(0)
        response = self.client('out', 4)
        self.assertEqual(response.returncode, 0, response.stdout + response.stderr)
        self.assertIn('RESUME accepted', response.stdout)
        self.assertTrue(equals(self.data / 'gatto', self.root / 'out' / 'gatto'))
        self.assertFalse(list((self.root / 'out').rglob('.checkpoint_case2')))

    def test_stale_manifest(self):
        self.start(1)
        self.assertNotEqual(self.client('out', 4).returncode, 0)
        self.stop()
        (self.data / 'gatto' / 'new_file.txt').write_text('changed')
        self.start(0)
        response = self.client('out', 4)
        self.assertEqual(response.returncode, 0, response.stdout + response.stderr)
        self.assertTrue(equals(self.data / 'gatto', self.root / 'out' / 'gatto'))

if __name__ == '__main__':
    unittest.main(verbosity=2)
