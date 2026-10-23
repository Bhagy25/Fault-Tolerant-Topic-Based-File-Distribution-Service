#!/usr/bin/env python3
"""Create deterministic small topic folders for testing without Animals-10."""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", nargs="?", default="demo_dataset")
    args = parser.parse_args()
    root = Path(args.directory)
    for topic in ("gatto", "cane", "empty"):
        (root / topic).mkdir(parents=True, exist_ok=True)
    (root / "gatto" / "small.txt").write_bytes(b"sample cat file\n")
    (root / "gatto" / "binary_150kb.bin").write_bytes(
        bytes(i % 256 for i in range(150_000)))
    (root / "gatto" / "file with spaces.txt").write_bytes(b"spaces are supported\n")
    (root / "cane" / "dog1.txt").write_bytes(b"sample dog file\n")
    (root / "cane" / "dog2.bin").write_bytes(bytes(i % 251 for i in range(12_000)))
    print(f"Created test topics at {root.resolve()}")


if __name__ == "__main__":
    main()
