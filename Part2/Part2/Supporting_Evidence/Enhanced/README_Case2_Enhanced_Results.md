# IT305 Part 2 – Case 2 Enhanced results

This standalone package contains only Case 2 Enhanced result data, per-case graphs, metric caveats, and the user's run screenshot. It includes no cross-case comparison files.

## Experiment captured
Server: `./server 5003 <topics_root> 0.001`. Two clients were launched concurrently, each requesting `cane` with four parallel connections and a retry limit of ten. Both client processes exited 0 and reported `status=OK`. Client A: 4 retries, 25.680397 s, reported 2.365 MB/s. Client B: 3 retries, 25.628953 s, reported 2.369 MB/s. The server log shows injected faults followed by successful data-transfer sessions.


