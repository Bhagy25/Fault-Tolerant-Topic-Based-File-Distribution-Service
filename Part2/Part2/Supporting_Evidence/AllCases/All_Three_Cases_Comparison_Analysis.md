# All-three-case comparison: Case 1 vs Case 2 vs Case 2 Enhanced

## What is included
The comparison package includes only all-three-case comparison datasets, line graphs, and analysis. It does not contain pairwise-only plots (such as Case 1 vs Case 2) or standalone individual-case results packages.

## Comparison basis
All recorded faulted-recovery runs used two clients requesting the `cane` topic (expected source: 4,863 files, 60,686,716 bytes) on one Windows laptop through WSL2/Ubuntu localhost. Case 1 used failure probability 0.0002; Case 2 and Case 2 Enhanced used 0.001. Case 2 Enhanced used four parallel TCP connections per client.

## Main observations
- Case 1 (no session management): client A/B reported 1/7 retries and took 16.160/74.318 seconds. Reported redundant bytes are available: 41,341,614 for A and 148,466,451 for B, because Case 1 restarts the topic from the beginning.
- Case 2 (checkpoint resume): client A/B reported 5/6 retries and took 115.219/124.425 seconds. The run log's `files` and `useful_bytes` counters overcount the completed checkpoint anchor after reconnect. Normalized useful-payload rate in the chart is calculated using 60,686,716 source bytes divided by elapsed time, not the inflated reported useful counter.
- Case 2 Enhanced (four parallel connections per client): client A/B reported 4/3 retries and took 25.680/25.629 seconds. The reported file/useful-byte counters exceed expected source totals; normalized rate likewise uses 60,686,716 source bytes. The supplied screenshot does not include exact verification-command exit codes, so integrity evidence is marked not captured in this comparison artifact.

## Graph interpretation limits
All three charts are line graphs with markers and use Case 1, Case 2, Case 2 Enhanced as the x-axis categories, and Client A / Client B as series. They show one observed run per case, not averages or trends over a fault-probability sweep. Error bars are omitted because there are no repeat runs under identical conditions. The failure probability for Case 1 differs from Cases 2 and Enhanced, so this is descriptive comparison, not an apples-to-apples benchmark.

Payload retransmission overhead cannot be reliably compared across all three cases. Case 1 has directly reported redundant payload bytes; Case 2 and Enhanced do not have unambiguous per-client aggregate transmitted-byte totals across all reconnects / parallel streams. The accompanying availability CSV records these gaps instead of plotting missing values as zero.

