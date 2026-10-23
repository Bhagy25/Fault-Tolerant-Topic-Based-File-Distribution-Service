# IT305 — Fault-Tolerant Topic-Based File Distribution Service

**Group:** 050 & 055  
**Course:** IT305 — Computer Networks Lab  
**Implementation:** C11, TCP sockets, POSIX threads, Linux / Ubuntu (including WSL2)

## Team

| Student | Roll number |
|---|---|
| Bhagya Sheth | 202401200 |
| Om Shiloja | 202401201 |
| Dhruv Sodvadiya | 202401207 |
| Parth Sorathiya | 202401215 |
| Prins Satapara | 202401188 |
| Dhaval Popat | 202401161 |

For individual responsibilities and experimental interpretation, see `FinalReport_Group_050_055.pdf`.

## 1. Project overview

This project implements topic-based distribution of binary files over TCP. The server discovers topics from subdirectories of a supplied dataset root; a client requests one topic and downloads its regular files into `<output_dir>/<topic>/`. Part I compares sequential handling with multithreaded connection handling. Part II adds probabilistic interruptions and three recovery approaches:

- **Part I:** Basic file distribution, with single-threaded and multithreaded server modes.
- **Part II, Case 1:** On interruption, discard partial topic progress and restart the topic from the beginning.
- **Part II, Case 2:** Store a persistent client checkpoint and resume using a validated filename and byte offset; the server does not retain per-client resume sessions.
- **Part II, Case 2 Enhanced:** Use four parallel TCP data connections by default, with disjoint whole-file partitions and separate recovery checkpoints for each stream.

Files are transmitted as raw bytes; textual protocol messages are newline-terminated. The server accepts its port and dataset directory at runtime; no campus IP, dataset path, or port should be hard-coded.

## 2. Final submission structure

```text
FinalSubmission_Group_050_055/
├── FinalReport_Group_050_055.pdf
├── README.md
├── Part1/
│   ├── server.c
│   ├── client.c
│   └── Makefile
├── Part2_Case1/
│   ├── server.c
│   ├── client.c
│   └── Makefile
├── Part2_Case2/
│   ├── server.c
│   ├── client.c
│   ├── checkpoint.c
│   ├── checkpoint.h
│   └── Makefile
├── Part2_Case2_Enhanced/
│   ├── server.c
│   ├── client.c
│   ├── checkpoint.c
│   ├── checkpoint.h
│   └── Makefile
├── common/                  # Shared C utilities required by the supplied Makefiles
├── Results/                 # Raw measured CSVs and throughput/overhead figures
└── tests/                   # Optional local regression and verification helpers
```

The official submission requires `Part1/`, all three Part II folders, `Results/`, the final PDF, and this README. The supplied C build files also reference `../common/`; **include a verified compatible `common/` directory in the final archive**. The Part I and Part II source archives were originally handed off separately, so confirm the selected shared headers/utilities compile and pass tests with *all four* final implementations. Do not copy two different `common/` folders over one another without checking compatibility.

## 3. Build requirements

On Ubuntu 22.04 or a compatible Linux system:

```bash
sudo apt update
sudo apt install -y build-essential python3
```

From the root of the **assembled final submission**:

```bash
make -C Part1
make -C Part2_Case1
make -C Part2_Case2
make -C Part2_Case2_Enhanced
```

Each `make` must produce directly executable files named `server` and `client` inside that respective directory. No additional required CLI arguments are permitted for the auto-grader.

Optional cleanup:

```bash
make -C Part1 clean
make -C Part2_Case1 clean
make -C Part2_Case2 clean
make -C Part2_Case2_Enhanced clean
```

## 4. Dataset

The server requires a directory with one subdirectory per topic. The team used the Animals-10 JPEG dataset, whose topic keys are:

```text
cane  cavallo  elefante  farfalla  gallina
 gatto  mucca  pecora  ragno  scoiattolo
```

Example layout:

```text
/home/USER/datasets/animals10/raw-img/
├── cane/
│   ├── example1.jpeg
│   └── ...
├── gatto/
│   └── ...
└── ...
```

**Replace** `/path/to/raw-img` in the commands below with the actual existing dataset directory. The dataset is **not** required on client laptops. The grading system may supply a different dataset root with different topic names.

## 5. Running Part I

Open two Linux terminals. In Terminal A (server), choose one mode:

```bash
# Standard required server invocation (the supplied Part I code defaults to multi mode)
./Part1/server 9000 /path/to/raw-img

# Explicit single-threaded comparison mode
./Part1/server 9000 /path/to/raw-img single

# Explicit multithreaded comparison mode
./Part1/server 9000 /path/to/raw-img multi
```

Use **one** server process per TCP port. Stop the current server with Ctrl+C before switching modes.

In Terminal B (client):

```bash
./Part1/client 127.0.0.1 9000 cane ./downloads_part1
```

The resulting files should be at `downloads_part1/cane/`. From a different physical laptop, replace `127.0.0.1` with the **server laptop's current reachable LAN IP**. The `single`/`multi` options are server-only; the client invocation stays the same.

For benchmarking, use identical topic workloads and the same network conditions for each mode. Run 1, 2, 4, and 8 concurrent client processes, distributing them over the two client hosts. Collect real client elapsed times, actual completion status and the wall-clock duration of each group. Do not conflate eight measurement rows with eight simultaneously connected clients.

## 6. Running Part II — Case 1 (restart from beginning)

The final server command requires a **failure probability** between 0 and 1. Fault decisions are made after eligible data chunks; a triggered fault closes that TCP connection.

Terminal A:

```bash
./Part2_Case1/server 9001 /path/to/raw-img 0.0002
```

Terminal B:

```bash
./Part2_Case1/client 127.0.0.1 9001 cane ./downloads_case1
```

On interruption, the client discards partial topic output, reconnects and restarts from the first file. The retry count is bounded. Use `0.0` for a fault-free baseline. An optional trailing client retry bound is supported by this supplied case implementation:

```bash
./Part2_Case1/client 127.0.0.1 9001 cane ./downloads_case1 10
```

A high probability, especially `1.0`, can intentionally cause an incomplete transfer. Record such a run as a failure, not a successful delivery.

## 7. Running Part II — Case 2 (checkpoint resume)

Terminal A:

```bash
./Part2_Case2/server 9002 /path/to/raw-img 0.001
```

Terminal B:

```bash
./Part2_Case2/client 127.0.0.1 9002 cane ./downloads_case2
```

A disconnect triggers a bounded reconnect-and-resume procedure. The client stores its topic, current filename, file offset, completion progress and manifest information in a durable checkpoint. It issues a `RESUME` request; the server validates the claimed position and returns `READY` before continuing. Keep the **same output directory** when resuming, or the checkpoint cannot be reused correctly.

Optional client retry configuration:

```bash
./Part2_Case2/client 127.0.0.1 9002 cane ./downloads_case2 10
```

To demonstrate deliberate checkpoint creation: run the server with probability `1.0`, stop it, restart at `0.0`, then rerun the client with the original output directory and inspect the nonzero resume offset in the logs. Treat the staged test and an uninterrupted automatic-recovery run as **different** experimental scenarios.

## 8. Running Part II — Case 2 Enhanced (parallel resume)

Terminal A:

```bash
./Part2_Case2_Enhanced/server 9003 /path/to/raw-img 0.001
```

Terminal B:

```bash
./Part2_Case2_Enhanced/client 127.0.0.1 9003 cane ./downloads_enhanced
```

The Enhanced client first makes a short `META` request, then starts four parallel TCP data connections by default. Files are assigned by disjoint contiguous ranges of the sorted file list; each connection resumes independently. **Whole files, not intra-file byte segments, are distributed among the workers.**

Optional trailing arguments choose the number of connections and retry bound:

```bash
./Part2_Case2_Enhanced/client 127.0.0.1 9003 cane ./downloads_enhanced 4 10
```

The inspected Enhanced code uses **blocking** sockets and a receive timeout, not a non-blocking `select`/`poll`/`epoll` event loop. Its `META` exchange and the other implementation/design differences are discussed in the final report.

## 9. Running across physical computers (WSL2)

Part I evaluation uses one server host and two physical client hosts. The server runs on Ubuntu/WSL2 on the server laptop. When WSL2 uses default NAT networking, allow inbound traffic on the selected server TCP port through **Windows-to-WSL port forwarding** and a suitably scoped Windows Firewall rule. The commands below are a deployment example, not part of the required C program interface.

On the server laptop in **Administrator PowerShell** (example port 9000):

```powershell
$wslIP = (wsl -d Ubuntu-22.04 hostname -I).Trim().Split(' ')[0]
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=9000 connectaddress=$wslIP connectport=9000
netsh interface portproxy show v4tov4
```

Check the current server LAN address with `ipconfig` in Windows PowerShell. Allow only trusted client IPs through an inbound TCP firewall rule, with the correct Public/Private profile for that trusted network. Do **not** disable the entire Windows Firewall. Since both the Wi-Fi DHCP IP and WSL2 NAT IP can change, verify the portproxy and firewall after reconnecting or restarting.

On each client laptop (Windows PowerShell):

```powershell
Test-NetConnection SERVER_LAN_IP -Port 9000
```

Then inside its Ubuntu terminal:

```bash
./Part1/client SERVER_LAN_IP 9000 cane ./downloads_part1
```

Do not use `127.0.0.1` when the client runs on a different laptop. If the network isolates connected clients, use a trusted private LAN/hotspot that permits peer-to-peer connections.

For Part II cases using ports `9001`, `9002`, `9003`, configure the corresponding inbound and forwarding rules if running across separate WSL2 laptops.

## 10. Results, analysis and integrity verification

Store the actual measurements and generated plots under `Results/`. Supplied archive examples include:

```text
Results/
├── Part1_Comparison.csv
├── Part1_SingleThreaded_raw.csv
├── Part1_MultiThreaded_raw.csv
├── Part1_Comparison_throughput.png
├── Part1_Comparison_transfer_time.png
├── Part2_Case1_results.csv
├── Part2_Case1_throughput.png
├── Part2_Case1_overhead.png
├── Part2_Case2_results.csv
├── Part2_Case2_auto_recovery_normalized_goodput.png
├── Part2_Case2_auto_recovery_time.png
├── Part2_Case2_Enhanced_results.csv
├── Part2_Case2_Enhanced_normalized_goodput.png
├── All_Three_Cases_Comparison.csv
└── All_Three_Cases_throughput_line.png
```

See the actual `Results/` directory for additional retry, time and cross-case plots. The course requires **raw CSV measurements and graphs**, not merely terminal screenshots.

Definitions:

- **Transfer time:** observed end-to-end elapsed time for a client, including retry delays as applicable.
- **Normalized source-payload rate (MB/s):** unique source payload bytes divided by elapsed seconds and 1,000,000.
- **Aggregate throughput:** total unique successfully delivered bytes across all concurrent clients divided by the group's overall wall-clock duration and 1,000,000.
- **Retransmission overhead:** redundant bytes retransmitted beyond the unique source payload; report it only when accurately measured.

**Reporting cautions:** The archived Case 1 fault recovery used `p=0.0002`, whereas the automatic Case 2 and Enhanced runs used `p=0.001`, so these are descriptive rather than controlled same-failure-rate comparisons. Case 2 and Enhanced resumed-transfer client counters sometimes exceed the true 4,863 files / 60,686,716 bytes of the `cane` topic because of repeated counting at the resume anchor. Do not use those over-counted numbers to calculate unique useful payload or infer missing byte overhead. Where actual cumulative retransmitted bytes were not measured, record the overhead as unavailable, **not zero**. Retain raw data and label simulated/demo data explicitly; do not submit generated practice values as measured experiments.

For independent integrity verification when the supplied helper is present:

```bash
python3 tests/verify_transfer.py /path/to/raw-img/cane ./downloads_part1/cane
```

Alternatively, compare sorted per-file SHA-256 hashes on the server and on the receiving clients. File counts alone do not prove bit-for-bit equality.

## 11. Optional regression tests

If the assembled submission includes the `tests/` folder, run the case-specific tests from the project root:

```bash
python3 tests/test_part1.py
python3 tests/test_case1.py
python3 tests/test_case2.py
python3 tests/test_enhanced.py
```

These are development checks; full-dataset LAN experiments and valid evidence in `Results/` remain necessary. Some tests or helper scripts may require the exact original directory layout; confirm paths before invoking them in the consolidated package.

## 12. Final checks before packaging

1. Ensure there is **one compatible `common/` implementation**, and all four folders compile independently using `make` with no warnings/errors.
2. Check the standard CLI calls with **only** the required arguments, and verify the executables are named `server` and `client` in each case folder.
3. Verify the final report exists as `FinalReport_Group_050_055.pdf` and contains Part I and all three Part II cases.
4. Ensure `Results/` contains real CSV logs plus the throughput and overhead graphs used by the report; distinguish any practice/simulated rows.
5. Check all six team names and roll numbers, and the contribution descriptions, against the final report.
6. Confirm one complete transfer and one relevant fault/recovery test for each appropriate case, including file-integrity evidence when available.
7. Package **one** archive named `FinalSubmission_Group_050_055.zip` (or `.tar.gz`) containing a top-level `FinalSubmission_Group_050_055/` directory and the files/folders listed in Section 2.

Example packaging from the parent directory:

```bash
zip -r FinalSubmission_Group_050_055.zip FinalSubmission_Group_050_055/
```

**Note:** This README documents the inspected source packages and available experimental artifacts. It does not claim that the separately supplied Part I and Part II codebases have already been integrated into one verified final archive. Validate the combined build and measurements before submission.
