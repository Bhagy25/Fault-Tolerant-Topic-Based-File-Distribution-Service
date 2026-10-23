#!/usr/bin/env python3
"""Compare files recursively by name, length and SHA256 digest."""
import argparse
import hashlib
from pathlib import Path
import sys


def file_digest(path):
    dig = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            dig.update(chunk)
    return dig.hexdigest()


def files_with_hashes(root):
    return {str(path.relative_to(root)): file_digest(path)
            for path in root.rglob("*") if path.is_file() and not path.is_symlink()}


def main():
    parser = argparse.ArgumentParser(description="Verify transferred topic directory")
    parser.add_argument("original_topic_dir")
    parser.add_argument("received_topic_dir")
    args = parser.parse_args()
    original = Path(args.original_topic_dir)
    received = Path(args.received_topic_dir)
    if not original.is_dir() or not received.is_dir():
        print("ERROR: both inputs must be existing topic directories", file=sys.stderr)
        sys.exit(1)
    expected = files_with_hashes(original)
    actual = files_with_hashes(received)
    missing = expected.keys() - actual.keys()
    extra = actual.keys() - expected.keys()
    changed = {name for name in expected.keys() & actual.keys()
               if expected[name] != actual[name]}
    if missing or extra or changed:
        print(f"FAIL: missing={sorted(missing)} extra={sorted(extra)} mismatched={sorted(changed)}")
        sys.exit(1)
    print(f"PASS: {len(expected)} files matched, SHA256-verified")


if __name__ == "__main__":
    main()
