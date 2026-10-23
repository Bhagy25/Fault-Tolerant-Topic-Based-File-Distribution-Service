# Part 2 — Case 1: Restart after disconnection

Implemented in C11. This part has a thread-per-client TCP server and client that retries an interrupted topic transfer **from the beginning**. There is no checkpoint or session management in this case.

## Build

From the project root:

```bash
make
make test
```

Or from this folder:

```bash
make
```

## Required CLI

```bash
./server <port> <topics_root_dir> <failure_probability>
./client <server_ip> <server_port> <topic> <output_dir>
```

Optional client trailing argument: `[max_retries]` (default 10). For tests that deliberately cause a failure every chunk, use a small value such as 2 so the test finishes quickly.

## Mechanism

- Each server client session runs in a detached POSIX thread.
- The server tests the failure probability after each successfully sent binary-data chunk (up to 64 KiB).
- An injected fault closes the connection without sending `DONE`.
- The client detects the incomplete transfer, removes only files it wrote during that failed attempt, reconnects after a bounded exponential delay, and sends a fresh `GET` request.
- Completion occurs only on receiving `DONE` after the expected number of files.
- The client logs bytes received across attempts, useful bytes from the successful attempt, redundant received bytes, retry count, elapsed time, and useful-data throughput.
- Use a **dedicated output directory** for these experiments: existing same-name output files can be replaced when a transfer writes them. Do not run over valuable existing user files.

## Local demonstration

From the project root:

```bash
python3 tests/create_demo_dataset.py
cd Part2_Case1
./server 9001 ../demo_dataset 0.0
```

In a second Ubuntu terminal:

```bash
cd ~/IT305_Group_050_055_Working/Part2_Case1
./client 127.0.0.1 9001 gatto ../case1_downloads
cd ..
python3 tests/verify_transfer.py demo_dataset/gatto case1_downloads/gatto
```

For forced disconnections:

```bash
# restart server with probability 1.0
./server 9001 ../demo_dataset 1.0
# run client with a bounded retry count:
./client 127.0.0.1 9001 gatto ../case1_fault_demo 2
```

The forced-disconnection client is expected to exit with an error after exhausting retries. A low nonzero probability may finish, but stochastic success is not guaranteed. The Part 2 performance comparisons should use controlled multiple-run trials and larger test files; localhost demo throughput is not the required three-machine LAN measurement.

## Deviation and qualification

The fault generator uses a mutex-protected process-wide pseudorandom state so concurrent threads do not race over `rand()`. The algorithm remains an independent probability decision after each chunk. Results are nondeterministic. `total transmitted bytes` for reporting must be gathered at the server side: the client's `received_bytes` metric counts the bytes actually read by the client, which can differ slightly from socket send-byte counts around abrupt disconnections.
