# IT305 Part 2 – Case 1 results

This package contains Case 1 (no session management) result CSV, client/server logs, plotting script, and per-case throughput/overhead graphs. It intentionally contains no cross-case comparison files.

## Main two-client fault-recovery observation
At server failure probability 0.0002, Client A completed with 1 retry (16.160375 s, 41,341,614 redundant bytes reported) and Client B completed with 7 retries (74.318346 s, 148,466,451 redundant bytes reported). The previous p=0.001 run exhausted its retry limit and is retained as a failed trial.


