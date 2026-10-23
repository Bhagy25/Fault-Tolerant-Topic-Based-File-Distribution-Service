# IT305 Group 050 & 055 — Part 2 standalone package

This `Part2/` folder is a staging bundle for Part 2 only. In the final course submission, put the three `Part2_Case*` directories, `common/` and `Results/` directly inside `FinalSubmission_Group_050_055/` alongside `Part1/`, the final PDF and README.

## Compile and run
From within this directory:

```bash
make -C Part2_Case1
make -C Part2_Case2
make -C Part2_Case2_Enhanced
```

Each folder creates `server` and `client`. The mandatory calls are:
`./server <port> <topics_root_dir> <failure_probability>` and `./client <server_ip> <server_port> <topic> <output_dir>`.

Examples (from each corresponding case folder):
- Case 1 server: `./server 9001 ~/datasets/animals10/raw-img 0.0`
- Case 2 server: `./server 9002 ~/datasets/animals10/raw-img 0.0`
- Enhanced server: `./server 9003 ~/datasets/animals10/raw-img 0.0`
- Client: `./client 127.0.0.1 9002 gatto ../downloads_case2`

Run automated tests from this `Part2/` directory:

```bash
python3 tests/test_case1.py
python3 tests/test_case2.py
python3 tests/test_enhanced.py
```

## Source provenance and limitations
C source is from the previously provided `IT305_Group_050_055_Part2_With_Code` package, not generated from graphs. Results files come from the four updated ZIPs supplied by your teammate and have been retained byte-for-byte. `SOURCE_MANIFEST.csv` maps their original locations and SHA-256 hashes.

**Warnings before final submission:** The Case 2 and Enhanced raw measurements may overcount useful payload on retries, the across-cases comparison does not use identical fault rates, and these records do not establish the required multi-machine LAN measurements. Verify the code build matches the exact version used for each CSV, correct or disclose the metrics issues, and repeat a controlled comparison if necessary. These caveats are detailed in `Supporting_Evidence/`.

No simulated values or artificial experimental measurements were added.
