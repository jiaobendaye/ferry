## ADDED Requirements

### Requirement: Cgroup-v2 memory classification
Each periodic and SIGUSR1 stats line SHALL include `mem_anon`, `mem_file`, and
`mem_sock` byte gauges sampled from the server's own cgroup-v2 `memory.stat` at
the time the line is emitted. The gauges SHALL be described as cgroup-wide
memory classes, not as per-process RSS or per-file cache attribution.

#### Scenario: Memory gauges match cgroup
- **WHEN** the server emits stats from a readable cgroup-v2 hierarchy containing `anon`, `file`, and `sock`
- **THEN** `mem_anon`, `mem_file`, and `mem_sock` equal those fields from the same cgroup's `memory.stat`

#### Scenario: Cache remains after requests drain
- **WHEN** file requests finish so that `inflight` is zero and cgroup file cache remains resident
- **THEN** the stats line reports `inflight=0` with a positive `mem_file` rather than attributing that memory to mmap or anonymous response buffers

### Requirement: Explicit unavailable memory gauges
The server SHALL report an affected memory gauge as `-1` if the unified cgroup,
cgroup2 mount, `memory.stat`, or any required field cannot be resolved or read. Unavailable data
SHALL NOT be reported as zero, and failure to sample memory SHALL NOT terminate
the server or suppress the rest of the stats line.

#### Scenario: Cgroup v2 is unavailable
- **WHEN** the server runs without a readable cgroup-v2 memory controller
- **THEN** it continues serving and emits `mem_anon=-1 mem_file=-1 mem_sock=-1`

#### Scenario: One memory field is missing
- **WHEN** `memory.stat` is readable but lacks one required field
- **THEN** the missing field is `-1` while the successfully parsed gauges retain their measured values

### Requirement: Cache-advice accounting in stats output
Each periodic and SIGUSR1 stats line SHALL include cumulative
`cache_advice_calls`, `cache_advice_bytes`, and `cache_advice_errors` values from
the configured file-cache policy, while preserving all existing stats fields.

#### Scenario: Normal mode counters remain zero
- **WHEN** the server handles file responses under `page_cache_policy = normal`
- **THEN** all three cache-advice counters remain zero

#### Scenario: Advice activity is reported
- **WHEN** advice attempts have succeeded and failed since startup
- **THEN** the stats line reports their cumulative call, accepted-byte, and error totals without claiming physical eviction
