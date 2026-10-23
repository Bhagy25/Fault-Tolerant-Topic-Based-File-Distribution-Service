#!/usr/bin/env python3
"""Measure end-to-end aggregate Part1 throughput with N simultaneous clients.

The server must already be running in the mode declared by --mode.
Examples (from project root):
  python3 tests/benchmark_part1.py --mode multi --topic gatto --clients 1 2 4
  python3 tests/benchmark_part1.py --mode single --topic gatto --clients 1 2 4
Do not interpret localhost benchmarks as the assignment's three-PC LAN results.
"""
import argparse
import csv
import os
from pathlib import Path
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parents[1]
METRIC_RE = re.compile(r"METRIC .*?status=(\w+) files=(\d+) bytes=(\d+) seconds=([0-9.]+)")


def one_client(host, port, topic, output_path):
    binary = ROOT / "Part1" / "client"
    proc = subprocess.run([str(binary), host, str(port), topic, str(output_path)],
                          text=True, capture_output=True, timeout=1800)
    match = METRIC_RE.search(proc.stdout)
    if proc.returncode or match is None or match.group(1) != "OK":
        raise RuntimeError(f"client failed: exit={proc.returncode}\n{proc.stdout}\n{proc.stderr}")
    return int(match.group(3))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", required=True, choices=["single", "multi"],
                        help="Label for server mode currently running")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--topic", required=True)
    parser.add_argument("--clients", nargs="+", type=int, default=[1, 2, 4, 8])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output-root", default="bench_downloads")
    parser.add_argument("--csv", default=str(ROOT / "Results" / "Part1_throughput.csv"))
    args = parser.parse_args()
    if args.runs < 1 or any(n < 1 for n in args.clients):
        parser.error("--runs and all --clients values must be positive")
    csv_file = Path(args.csv)
    csv_file.parent.mkdir(parents=True, exist_ok=True)
    is_empty = not csv_file.exists() or csv_file.stat().st_size == 0
    with open(csv_file, "a", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        if is_empty:
            writer.writerow(["server_mode", "clients", "run", "total_bytes", "wall_seconds", "aggregate_MBps"])
        for n in args.clients:
            for run in range(1, args.runs + 1):
                root = Path(args.output_root) / args.mode / f"clients_{n}_run_{run}"
                start = time.monotonic()
                with ThreadPoolExecutor(max_workers=n) as pool:
                    pending = [pool.submit(one_client, args.host, args.port, args.topic,
                                           root / f"client_{i}") for i in range(n)]
                    total_bytes = sum(future.result() for future in pending)
                wall = time.monotonic() - start
                aggregate_mbps = total_bytes / wall / 1_000_000 if wall > 0 else 0
                writer.writerow([args.mode, n, run, total_bytes, f"{wall:.6f}", f"{aggregate_mbps:.6f}"])
                output.flush()
                print(f"mode={args.mode} clients={n} run={run} bytes={total_bytes} "
                      f"wall={wall:.3f}s aggregate={aggregate_mbps:.3f} MB/s")
    print(f"CSV saved: {csv_file}")


if __name__ == "__main__":
    main()
