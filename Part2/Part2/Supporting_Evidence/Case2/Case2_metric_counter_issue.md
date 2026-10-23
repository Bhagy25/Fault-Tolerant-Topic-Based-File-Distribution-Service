# Case 2 metric-accounting caveat

The client log reports 4,868 files and 60,745,977 useful bytes for Client A, and 4,869 files and 60,767,371 useful bytes for Client B after automatic checkpoint recovery at p=0.001. These exceed the known source values (4,863 files; 60,686,716 bytes). The most likely explanation is that the completed resume-anchor file is counted again after reconnect, as described in the prior analysis. Preserve the raw counters; do not treat these overcounts as unique delivered data or as retransmission overhead.

The normalized useful-payload rate chart uses the expected unique topic size (60,686,716 bytes) divided by client elapsed time. It is explicitly a normalized calculation, not a replacement for fixing the implementation counters.
