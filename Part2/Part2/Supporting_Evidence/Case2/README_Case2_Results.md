# IT305 Part 2 – Case 2 results package

This package contains only Case 2 results (no cross-case graphs/tables): transcribed run-level CSV, case-specific client timing/retry/goodput graphs, the known counter caveat, and evidence screenshots.

## Main result
The automatic two-client run used failure probability 0.001 on the same server throughout. Client A completed with 5 retries in 115.218855 s; Client B completed with 6 retries in 124.425056 s. Client logs show non-zero resume offsets and status=OK.

## Metric caveat
Raw completed-file/useful-byte counters exceed the source dataset totals after reconnect; see `Results/Case2_metric_counter_issue.md`. Normalized rate uses the expected unique source payload (60,686,716 bytes) divided by elapsed seconds. The saved context indicates the user completed the integrity-check step, but the exact diff output was not captured in the visible artifact evidence.

