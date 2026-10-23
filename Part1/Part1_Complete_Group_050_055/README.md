# IT305 Group 050 & 055 — Part 1 C source package

Contains the **Part 1** server and client in C11, both threading modes, shared C networking/topic modules, and optional local test helpers. This is a code-only handoff, **not** a complete final assignment archive. The CSVs and graphs previously collected by the team are not embedded here.

## Contents
- `Part1/server.c`, `Part1/client.c`, `Part1/Makefile`
- `common/protocol.h`, `common/topics.{c,h}`, `common/utils.{c,h}`
- `tests/` optional Python scripts for generating small datasets, verifying transfers and local development tests
- Root `Makefile` convenience build

## Build / test (Ubuntu or WSL2)
```bash
cd Part1
make
cd ..
python3 tests/test_part1.py
```

## Required assignment CLI
```bash
# Default multi-threaded (no optional mode):
./Part1/server 9000 /absolute/path/to/raw-img
./Part1/client 127.0.0.1 9000 gatto ./downloads

# Optional mode to compare single and multi; no change to required CLI:
./Part1/server 9000 /absolute/path/to/raw-img single
./Part1/server 9000 /absolute/path/to/raw-img multi
```
One server process per port; stop it using Ctrl+C before starting another mode.
Output files appear inside `downloads/gatto/`.

## Independent verification of file contents
```bash
python3 tests/verify_transfer.py /absolute/path/to/raw-img/gatto downloads/gatto
```

## Integration warning
The team's existing CSV results must be tied to the exact code build and experimental setup used to collect them. This standalone code package has passed local automated tests, but it does **not** establish that the previously supplied measurements were collected using these identical files. Confirm this before claiming it in the final report. The earlier separately developed Part 2 code also requires protocol/integration verification with this Part 1 package.
